#!/usr/bin/env python3
import argparse
import secrets
import sys

from sqlalchemy import select

from db import Admin, SessionLocal, init_db
from security import hash_password


def create_admin(session, username: str, password: str | None) -> None:
    if session.scalar(select(Admin).where(Admin.username == username)):
        print(f"Администратор '{username}' уже существует", file=sys.stderr)
        sys.exit(1)

    password = password or secrets.token_urlsafe(12)
    session.add(
        Admin(
            id=secrets.token_hex(16),
            username=username,
            password_hash=hash_password(password),
        )
    )
    session.commit()
    print(f"Создан администратор '{username}'")
    print(f"Пароль: {password}")


def reset_password(session, username: str, password: str | None) -> None:
    admin = session.scalar(select(Admin).where(Admin.username == username))
    if admin is None:
        print(f"Администратор '{username}' не найден", file=sys.stderr)
        sys.exit(1)

    password = password or secrets.token_urlsafe(12)
    admin.password_hash = hash_password(password)
    admin.token_version = (admin.token_version or 0) + 1
    admin.is_active = True
    session.commit()
    print(f"Пароль для '{username}' сброшен, активные сессии инвалидированы.")
    print(f"Новый пароль: {password}")


def list_admins(session) -> None:
    for admin in session.scalars(select(Admin).order_by(Admin.username)):
        state = "active" if admin.is_active else "disabled"
        print(f"{admin.username}\t{state}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Управление администраторами СКУД")
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_create = subparsers.add_parser("create-admin", help="Создать администратора")
    p_create.add_argument("username")
    p_create.add_argument("--password", help="Пароль (по умолчанию сгенерируется)")

    p_reset = subparsers.add_parser("reset-password", help="Сбросить пароль администратора")
    p_reset.add_argument("username")
    p_reset.add_argument("--password", help="Пароль (по умолчанию сгенерируется)")

    subparsers.add_parser("list-admins", help="Показать администраторов")

    args = parser.parse_args()
    init_db()

    with SessionLocal() as session:
        if args.command == "create-admin":
            create_admin(session, args.username, args.password)
        elif args.command == "reset-password":
            reset_password(session, args.username, args.password)
        elif args.command == "list-admins":
            list_admins(session)


if __name__ == "__main__":
    main()
