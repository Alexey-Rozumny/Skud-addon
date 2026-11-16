import asyncio
import ipaddress
import os
import secrets
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from typing import List

import httpx
from fastapi import Depends, FastAPI, Form, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from sqlalchemy import select, text
from sqlalchemy.orm import Session
from starlette.middleware.sessions import SessionMiddleware

from db import (
    Admin,
    Door,
    Event,
    Permission,
    SessionLocal,
    User,
    get_session,
    get_setting,
    init_db,
    set_setting,
)
from security import (
    add_login_failure,
    clear_login_failures,
    get_csrf,
    hash_password,
    is_login_allowed,
    require_admin,
    verify_csrf,
    verify_password,
)

MODE = os.getenv("MODE", "user")
SECRET_KEY = os.getenv("SECRET_KEY", "change-me-in-production")
SESSION_COOKIE = "skud_admin_session" if MODE == "admin" else "skud_user_session"
COOKIE_SECURE = os.getenv("COOKIE_SECURE", "false").lower() in ("1", "true", "yes")

PHRASE_TTL_SECONDS = int(os.getenv("PHRASE_TTL_SECONDS", "86400"))
PULSE_MS = int(os.getenv("PULSE_MS", "2000"))
COMMAND_CONFIRM_SECONDS = int(os.getenv("COMMAND_CONFIRM_SECONDS", "10"))
DOOR_OFFLINE_SECONDS = int(os.getenv("DOOR_OFFLINE_SECONDS", "120"))
RETENTION_DAYS_DEFAULT = int(os.getenv("EVENTS_RETENTION_DAYS", "90"))

ACTION_LABELS = {
    "open_command": "Команда открытия",
    "door_opened": "Дверь открыта",
    "door_closed": "Дверь закрыта",
    "device_online": "Устройство онлайн",
    "secret_rotated": "Смена секрета",
}
TRIGGER_LABELS = {
    "remote_pulse": "по команде",
    "local": "локально",
    "registration": "регистрация",
}

_background_tasks: set[asyncio.Task] = set()


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def spawn(coro) -> None:
    task = asyncio.create_task(coro)
    _background_tasks.add(task)
    task.add_done_callback(_background_tasks.discard)


def bootstrap_admin() -> None:
    with SessionLocal() as session:
        if session.scalar(select(Admin.id).limit(1)) is not None:
            return

        username = os.getenv("ADMIN_USERNAME", "admin")
        password = os.getenv("ADMIN_PASSWORD") or secrets.token_urlsafe(12)
        session.add(
            Admin(
                id=uuid.uuid4().hex,
                username=username,
                password_hash=hash_password(password),
            )
        )
        session.commit()

        if not os.getenv("ADMIN_PASSWORD"):
            print(
                "\n".join(
                    [
                        "=" * 64,
                        f"Создан администратор: {username}",
                        f"Пароль: {password}",
                        "Смените пароль: python manage.py reset-password " + username,
                        "=" * 64,
                    ]
                ),
                flush=True,
            )


def cleanup_once() -> None:
    with SessionLocal() as session:
        locked = session.scalar(text("SELECT pg_try_advisory_lock(4242)"))
        if not locked:
            return

        try:
            days = int(get_setting(session, "retention_days", str(RETENTION_DAYS_DEFAULT)) or 0)
            max_rows = int(get_setting(session, "retention_max_rows", "0") or 0)

            if days > 0:
                while True:
                    result = session.execute(
                        text(
                            """
                            WITH old AS (
                                SELECT id FROM events
                                WHERE ts < now() - make_interval(days => :days)
                                ORDER BY ts
                                LIMIT 10000
                            )
                            DELETE FROM events e
                            USING old
                            WHERE e.id = old.id
                            """
                        ),
                        {"days": days},
                    )
                    session.commit()
                    if result.rowcount == 0:
                        break

            if max_rows > 0:
                session.execute(
                    text(
                        """
                        DELETE FROM events
                        WHERE id < COALESCE(
                            (
                                SELECT id FROM events
                                ORDER BY id DESC
                                OFFSET :keep
                                LIMIT 1
                            ),
                            0
                        )
                        """
                    ),
                    {"keep": max_rows},
                )
                session.commit()
        finally:
            try:
                session.rollback()
                session.execute(text("SELECT pg_advisory_unlock(4242)"))
                session.commit()
            except Exception:
                pass


async def retention_loop() -> None:
    await asyncio.sleep(10)
    while True:
        try:
            await asyncio.to_thread(cleanup_once)
        except Exception as exc:  # noqa: BLE001
            print("retention error:", exc, flush=True)
        await asyncio.sleep(24 * 3600)


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    if MODE == "admin":
        bootstrap_admin()
    task = asyncio.create_task(retention_loop())
    try:
        yield
    finally:
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass


app = FastAPI(lifespan=lifespan)
app.add_middleware(
    SessionMiddleware,
    secret_key=SECRET_KEY,
    session_cookie=SESSION_COOKIE,
    same_site="lax",
    https_only=COOKIE_SECURE,
)

os.makedirs("static", exist_ok=True)
app.mount("/static", StaticFiles(directory="static"), name="static")

templates = Jinja2Templates(directory="templates")


def fmt_dt(value: datetime | None) -> str:
    if value is None:
        return "—"
    return value.astimezone().strftime("%d.%m.%Y %H:%M:%S")


def is_online(value: datetime | None) -> bool:
    if value is None:
        return False
    return (utcnow() - value).total_seconds() < DOOR_OFFLINE_SECONDS


templates.env.filters["dt"] = fmt_dt
templates.env.filters["online"] = is_online


def normalize_ip(raw: str | None) -> str | None:
    if not raw:
        return None
    try:
        return str(ipaddress.ip_address(raw.strip()))
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="Некорректный IP-адрес") from exc


def get_api_key(request: Request) -> str | None:
    key = request.headers.get("X-Api-Key")
    if key:
        return key.strip()
    auth = request.headers.get("Authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return None


async def confirm_command(command_id: int, door_id: str) -> None:
    await asyncio.sleep(COMMAND_CONFIRM_SECONDS)
    with SessionLocal() as session:
        command = session.get(Event, command_id)
        if command is None or command.result != "ok":
            return
        child = session.scalar(
            select(Event.id).where(Event.parent_event_id == command_id).limit(1)
        )
        if child is not None:
            return
        door = session.get(Door, door_id)
        if door is not None and door.state == "open":
            return
        command.result = "no_confirm"
        session.commit()


async def trigger_door(
    session: Session,
    door: Door,
    actor_type: str,
    actor_name: str,
    source_ip: str | None,
) -> dict:
    command = Event(
        door_id=door.id,
        door_name=door.name,
        actor_type=actor_type,
        actor_name=actor_name,
        action="open_command",
        result="sent",
        trigger="remote_pulse",
        detail={"source_ip": source_ip},
    )
    session.add(command)
    session.commit()

    if not door.ip:
        command.result = "no_ip"
        session.commit()
        return {"status": "error", "message": "У двери не задан IP-адрес"}

    now = utcnow()
    if not door.phrase or not door.phrase_expires or door.phrase_expires < now:
        command.result = "not_registered"
        session.commit()
        return {
            "status": "error",
            "message": "Устройство не зарегистрировано (нет кодовой фразы)",
        }

    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            response = await client.post(
                f"http://{door.ip}/pulse",
                headers={
                    "X-Api-Key": door.secret,
                    "X-Phrase": door.phrase,
                },
            )
    except httpx.TimeoutException:
        command.result = "timeout"
        session.commit()
        return {"status": "error", "message": "Таймаут связи с ESP"}
    except httpx.HTTPError as exc:
        command.result = "connection_error"
        command.detail = {"source_ip": source_ip, "error": str(exc)}
        session.commit()
        return {"status": "error", "message": "Ошибка связи с ESP"}

    if response.status_code in (200, 303):
        command.result = "ok"
        session.commit()
        spawn(confirm_command(command.id, door.id))
        return {"status": "success", "message": "Дверь открыта"}

    command.result = f"esp_{response.status_code}"
    command.detail = {
        "source_ip": source_ip,
        "status_code": response.status_code,
        "body": response.text[:500],
    }
    session.commit()
    message = f"ESP вернул код {response.status_code}"
    body = response.text.strip()
    if body:
        message += f": {body[:200]}"
    return {"status": "error", "message": message}


# ==========================================
# API для ESP (доступно в обоих режимах)
# ==========================================
@app.post("/api/register")
async def api_register(request: Request, session: Session = Depends(get_session)):
    key = get_api_key(request)
    if not key:
        return JSONResponse(
            {"status": "error", "message": "Нет ключа устройства"}, status_code=401
        )

    door = session.scalar(select(Door).where(Door.secret == key))
    if door is None:
        return JSONResponse(
            {"status": "error", "message": "Неизвестный ключ устройства"},
            status_code=403,
        )

    try:
        body = await request.json()
    except Exception:
        return JSONResponse(
            {"status": "error", "message": "Ожидается JSON"}, status_code=400
        )

    if body.get("ip"):
        try:
            door.ip = str(ipaddress.ip_address(str(body["ip"]).strip()))
        except ValueError:
            pass

    now = utcnow()
    previous_seen = door.last_seen
    door.last_seen = now
    if body.get("contact") is False:
        door.state = "unknown"
    elif "door_open" in body:
        door.state = "open" if body["door_open"] else "closed"
    if not door.phrase or not door.phrase_expires or door.phrase_expires < now:
        door.phrase = secrets.token_hex(8)
    door.phrase_expires = now + timedelta(seconds=PHRASE_TTL_SECONDS)

    if previous_seen is None or (now - previous_seen).total_seconds() > DOOR_OFFLINE_SECONDS:
        session.add(
            Event(
                door_id=door.id,
                door_name=door.name,
                actor_type="esp",
                actor_name=door.name,
                action="device_online",
                result="ok",
                trigger="registration",
                detail={"ip": door.ip, "fw": body.get("fw")},
            )
        )

    session.commit()
    return {
        "status": "ok",
        "door_id": door.id,
        "phrase": door.phrase,
        "ttl": PHRASE_TTL_SECONDS,
        "pulse_ms": PULSE_MS,
    }


@app.post("/api/event")
async def api_event(request: Request, session: Session = Depends(get_session)):
    key = get_api_key(request)
    if not key:
        return JSONResponse(
            {"status": "error", "message": "Нет ключа устройства"}, status_code=401
        )

    door = session.scalar(select(Door).where(Door.secret == key))
    if door is None:
        return JSONResponse(
            {"status": "error", "message": "Неизвестный ключ устройства"},
            status_code=403,
        )

    try:
        body = await request.json()
    except Exception:
        return JSONResponse(
            {"status": "error", "message": "Ожидается JSON"}, status_code=400
        )

    now = utcnow()
    door.last_seen = now
    is_open = bool(body.get("open"))
    trigger = body.get("trigger")
    door.state = "open" if is_open else "closed"

    if is_open:
        command = None
        if trigger != "local":
            used = select(Event.parent_event_id).where(Event.parent_event_id.isnot(None))
            command = session.scalar(
                select(Event)
                .where(
                    Event.door_id == door.id,
                    Event.action == "open_command",
                    Event.result == "ok",
                    Event.ts >= now - timedelta(seconds=COMMAND_CONFIRM_SECONDS),
                    Event.id.notin_(used),
                )
                .order_by(Event.id.desc())
                .limit(1)
            )

        if command is not None:
            event = Event(
                door_id=door.id,
                door_name=door.name,
                actor_type=command.actor_type,
                actor_name=command.actor_name,
                action="door_opened",
                result="ok",
                trigger=trigger or "remote_pulse",
                parent_event_id=command.id,
            )
        else:
            event = Event(
                door_id=door.id,
                door_name=door.name,
                actor_type="local",
                actor_name="Локально (ключ/кнопка)",
                action="door_opened",
                result="local",
                trigger=trigger or "local",
            )
    else:
        event = Event(
            door_id=door.id,
            door_name=door.name,
            actor_type="system",
            actor_name="Геркон",
            action="door_closed",
            result="ok",
            trigger=trigger,
        )

    session.add(event)
    session.commit()
    return {"status": "ok"}


# ==========================================
# ЛОГИКА АДМИНА (порт 8081)
# ==========================================
if MODE == "admin":

    @app.get("/login", response_class=HTMLResponse)
    def login_page(request: Request):
        if request.session.get("admin_id"):
            return RedirectResponse("/", status_code=303)
        return templates.TemplateResponse(
            "login.html",
            {"request": request, "csrf_token": get_csrf(request), "error": None},
        )

    @app.post("/login")
    def login_submit(
        request: Request,
        username: str = Form(...),
        password: str = Form(...),
        csrf_token: str = Form(...),
    ):
        verify_csrf(request, csrf_token)
        ip = request.client.host if request.client else "unknown"

        if not is_login_allowed(ip):
            return templates.TemplateResponse(
                "login.html",
                {
                    "request": request,
                    "csrf_token": get_csrf(request),
                    "error": "Слишком много попыток. Попробуйте позже.",
                },
                status_code=429,
            )

        with SessionLocal() as session:
            admin = session.scalar(select(Admin).where(Admin.username == username))
            if (
                admin is None
                or not admin.is_active
                or not verify_password(admin.password_hash, password)
            ):
                add_login_failure(ip)
                return templates.TemplateResponse(
                    "login.html",
                    {
                        "request": request,
                        "csrf_token": get_csrf(request),
                        "error": "Неверный логин или пароль",
                    },
                    status_code=401,
                )

            request.session.clear()
            request.session["admin_id"] = admin.id
            request.session["admin_ver"] = admin.token_version
            request.session["csrf"] = secrets.token_urlsafe(32)

        clear_login_failures(ip)
        return RedirectResponse("/", status_code=303)

    @app.post("/logout")
    def logout(request: Request, csrf_token: str = Form(...)):
        verify_csrf(request, csrf_token)
        request.session.clear()
        return RedirectResponse("/login", status_code=303)

    @app.get("/", response_class=HTMLResponse)
    def admin_dashboard(request: Request, session: Session = Depends(get_session)):
        admin = require_admin(request)

        doors = session.scalars(select(Door).order_by(Door.name)).all()
        user_rows = session.scalars(select(User).order_by(User.name)).all()

        users = []
        for user in user_rows:
            permissions = set(
                session.scalars(
                    select(Permission.door_id).where(Permission.user_id == user.id)
                ).all()
            )
            users.append(
                {
                    "id": user.id,
                    "name": user.name,
                    "token": user.token,
                    "is_active": user.is_active,
                    "permissions": permissions,
                }
            )

        events = session.scalars(
            select(Event).order_by(Event.ts.desc(), Event.id.desc()).limit(200)
        ).all()
        retention_days = get_setting(session, "retention_days", str(RETENTION_DAYS_DEFAULT))
        retention_max_rows = get_setting(session, "retention_max_rows", "0")
        oldest_event = session.scalar(select(Event.ts).order_by(Event.ts.asc()).limit(1))

        host_ip = request.url.hostname or "IP_СЕРВЕРА"

        return templates.TemplateResponse(
            "admin.html",
            {
                "request": request,
                "admin": admin,
                "doors": doors,
                "users": users,
                "events": events,
                "host_ip": host_ip,
                "csrf_token": get_csrf(request),
                "retention_days": retention_days,
                "retention_max_rows": retention_max_rows,
                "oldest_event": oldest_event,
                "action_labels": ACTION_LABELS,
                "trigger_labels": TRIGGER_LABELS,
            },
        )

    @app.post("/admin_open/{door_id}")
    async def admin_open_door(
        request: Request, door_id: str, session: Session = Depends(get_session)
    ):
        admin = require_admin(request)
        verify_csrf(request)

        door = session.get(Door, door_id)
        if door is None:
            return JSONResponse(
                {"status": "error", "message": "Дверь не найдена"}, status_code=404
            )

        source_ip = request.client.host if request.client else None
        return JSONResponse(
            await trigger_door(session, door, "admin", admin["username"], source_ip)
        )

    @app.post("/add_door")
    def add_door(
        request: Request,
        name: str = Form(...),
        ip: str = Form(""),
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        require_admin(request)
        verify_csrf(request, csrf_token)

        session.add(
            Door(
                id=uuid.uuid4().hex[:8],
                name=name,
                ip=normalize_ip(ip),
                secret=secrets.token_hex(16),
                state="unknown",
            )
        )
        session.commit()
        return RedirectResponse("/", status_code=303)

    @app.post("/delete_door/{door_id}")
    def delete_door(
        request: Request,
        door_id: str,
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        require_admin(request)
        verify_csrf(request, csrf_token)

        door = session.get(Door, door_id)
        if door is not None:
            session.delete(door)
            session.commit()
        return RedirectResponse("/", status_code=303)

    @app.post("/rotate_secret/{door_id}")
    def rotate_secret(
        request: Request,
        door_id: str,
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        admin = require_admin(request)
        verify_csrf(request, csrf_token)

        door = session.get(Door, door_id)
        if door is not None:
            door.secret = secrets.token_hex(16)
            door.phrase = None
            door.phrase_expires = None
            door.state = "unknown"
            session.add(
                Event(
                    door_id=door.id,
                    door_name=door.name,
                    actor_type="admin",
                    actor_name=admin["username"],
                    action="secret_rotated",
                    result="ok",
                )
            )
            session.commit()
        return RedirectResponse("/", status_code=303)

    @app.post("/add_user")
    def add_user(
        request: Request,
        name: str = Form(...),
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        require_admin(request)
        verify_csrf(request, csrf_token)

        session.add(
            User(
                id=uuid.uuid4().hex[:8],
                name=name,
                token=uuid.uuid4().hex[:12],
            )
        )
        session.commit()
        return RedirectResponse("/", status_code=303)

    @app.post("/delete_user/{user_id}")
    def delete_user(
        request: Request,
        user_id: str,
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        require_admin(request)
        verify_csrf(request, csrf_token)

        user = session.get(User, user_id)
        if user is not None:
            session.delete(user)
            session.commit()
        return RedirectResponse("/", status_code=303)

    @app.post("/update_permissions/{user_id}")
    def update_permissions(
        request: Request,
        user_id: str,
        door_ids: List[str] = Form(default=[]),
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        require_admin(request)
        verify_csrf(request, csrf_token)

        session.execute(
            text("DELETE FROM permissions WHERE user_id = :user_id"),
            {"user_id": user_id},
        )
        for door_id in door_ids:
            session.add(Permission(user_id=user_id, door_id=door_id))
        session.commit()
        return RedirectResponse("/", status_code=303)

    @app.post("/settings/retention")
    def update_retention(
        request: Request,
        retention_days: int = Form(...),
        retention_max_rows: int = Form(0),
        csrf_token: str = Form(...),
        session: Session = Depends(get_session),
    ):
        require_admin(request)
        verify_csrf(request, csrf_token)

        if retention_days < 0 or retention_max_rows < 0:
            raise HTTPException(status_code=400, detail="Значения не могут быть отрицательными")

        set_setting(session, "retention_days", str(retention_days))
        set_setting(session, "retention_max_rows", str(retention_max_rows))
        session.commit()
        return RedirectResponse("/", status_code=303)


# ==========================================
# ЛОГИКА ПОЛЬЗОВАТЕЛЯ (порт 8080)
# ==========================================
if MODE == "user":

    @app.get("/u/{token}", response_class=HTMLResponse)
    def user_panel(
        request: Request, token: str, session: Session = Depends(get_session)
    ):
        user = session.scalar(
            select(User).where(User.token == token, User.is_active.is_(True))
        )
        if user is None:
            return HTMLResponse("<h1>Доступ запрещён</h1>", status_code=403)

        doors = session.scalars(
            select(Door)
            .join(Permission, Permission.door_id == Door.id)
            .where(Permission.user_id == user.id)
            .order_by(Door.name)
        ).all()

        return templates.TemplateResponse(
            "user.html",
            {
                "request": request,
                "user": user,
                "doors": doors,
                "csrf_token": get_csrf(request),
            },
        )

    @app.post("/u/{token}/open/{door_id}")
    async def open_door_user(
        request: Request,
        token: str,
        door_id: str,
        session: Session = Depends(get_session),
    ):
        verify_csrf(request)

        user = session.scalar(
            select(User).where(User.token == token, User.is_active.is_(True))
        )
        if user is None:
            return JSONResponse(
                {"status": "error", "message": "Неверный токен"}, status_code=403
            )

        permission = session.scalar(
            select(Permission).where(
                Permission.user_id == user.id, Permission.door_id == door_id
            )
        )
        if permission is None:
            return JSONResponse(
                {"status": "error", "message": "Нет прав доступа"}, status_code=403
            )

        door = session.get(Door, door_id)
        if door is None:
            return JSONResponse(
                {"status": "error", "message": "Дверь не найдена"}, status_code=404
            )

        source_ip = request.client.host if request.client else None
        return JSONResponse(
            await trigger_door(session, door, "user", user.name, source_ip)
        )
