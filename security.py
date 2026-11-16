import secrets
import time
from collections import defaultdict

from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError, VerifyMismatchError
from fastapi import HTTPException, Request

from db import Admin, SessionLocal

_hasher = PasswordHasher()

MAX_ATTEMPTS = 5
ATTEMPT_WINDOW_SECONDS = 300

_login_attempts: dict[str, list[float]] = defaultdict(list)


def hash_password(password: str) -> str:
    return _hasher.hash(password)


def verify_password(password_hash: str, password: str) -> bool:
    try:
        return _hasher.verify(password_hash, password)
    except (VerifyMismatchError, VerificationError, InvalidHashError):
        return False


def is_login_allowed(ip: str) -> bool:
    now = time.time()
    attempts = [t for t in _login_attempts[ip] if now - t < ATTEMPT_WINDOW_SECONDS]
    _login_attempts[ip] = attempts
    return len(attempts) < MAX_ATTEMPTS


def add_login_failure(ip: str) -> None:
    _login_attempts[ip].append(time.time())


def clear_login_failures(ip: str) -> None:
    _login_attempts.pop(ip, None)


def get_csrf(request: Request) -> str:
    token = request.session.get("csrf")
    if not token:
        token = secrets.token_urlsafe(32)
        request.session["csrf"] = token
    return token


def verify_csrf(request: Request, token: str | None = None) -> None:
    if token is None:
        token = request.headers.get("X-CSRF-Token")
    expected = request.session.get("csrf")
    if not expected or not token or not secrets.compare_digest(expected, token):
        raise HTTPException(status_code=400, detail="Invalid CSRF token")


def require_admin(request: Request) -> dict:
    admin_id = request.session.get("admin_id")
    version = request.session.get("admin_ver")
    if not admin_id:
        raise HTTPException(status_code=303, headers={"Location": "/login"})

    with SessionLocal() as session:
        admin = session.get(Admin, admin_id)
        if (
            admin is None
            or not admin.is_active
            or version is None
            or admin.token_version != version
        ):
            request.session.clear()
            raise HTTPException(status_code=303, headers={"Location": "/login"})
        return {"id": admin.id, "username": admin.username}
