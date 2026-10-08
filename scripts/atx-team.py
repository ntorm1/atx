"""Issue individual ATX credentials into a new operator-owned directory."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import time


ROLES = {"viewer", "researcher", "reviewer", "admin"}
RESERVED = {"local", "gateway", "system", "migration_unknown"}


def create_documents(team_name: str, members: list[str], valid_days: int,
                     now_ms: int) -> tuple[dict, dict]:
    """Validate before issuing secrets; the native policy contains only token hashes."""
    if not team_name.strip(" ") or len(team_name.encode("utf-8")) > 128 or any(
            ord(char) < 32 or ord(char) == 127 for char in team_name):
        raise ValueError("Team name must contain 1..128 UTF-8 bytes without control characters")
    if not 1 <= len(members) <= 128 or not 0 <= valid_days <= 3650:
        raise ValueError("Use 1..128 members and a validity of 0..3650 days")
    parsed: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    for member in members:
        parts = member.split(":", 2)
        if len(parts) < 2:
            raise ValueError("Each member must be ID:ROLE or ID:ROLE:DISPLAY NAME")
        actor, role = parts[:2]
        name = parts[2] if len(parts) == 3 else actor
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", actor) or actor in RESERVED or actor in seen:
            raise ValueError("Member IDs must be unique, nonreserved, 1..64 letters/digits/_/-")
        if role not in ROLES:
            raise ValueError("Roles are viewer, researcher, reviewer or admin")
        if not name.strip(" ") or len(name.encode("utf-8")) > 128 or any(
                ord(char) < 32 or ord(char) == 127 for char in name):
            raise ValueError("Display names require 1..128 UTF-8 bytes without control characters")
        seen.add(actor)
        parsed.append((actor, role, name))
    if not any(role == "admin" for _, role, _ in parsed):
        raise ValueError("At least one administrator is required")
    expiry = now_ms + valid_days * 86_400_000 if valid_days else 0
    principals = []
    credentials = []
    for actor, role, name in parsed:
        token = secrets.token_urlsafe(32)
        principals.append({"id": actor, "name": name, "role": role,
                           "token_sha256": hashlib.sha256(token.encode("ascii")).hexdigest(),
                           "expires_at_ms": expiry})
        credentials.append({"id": actor, "role": role, "token": token,
                            "expires_at_ms": expiry})
    return ({"schema_version": 1, "team_name": team_name, "principals": principals},
            {"team_name": team_name, "credentials": credentials})


def write_documents(directory: Path, policy: dict, credentials: dict) -> None:
    """Refuse any existing directory; never overwrite an operator's credentials."""
    directory = directory.resolve()
    directory.mkdir(mode=0o700)
    created: list[Path] = []
    try:
        for name, document in (("team.json", policy), ("credentials.json", credentials)):
            target = directory / name
            with target.open("x", encoding="utf-8", newline="\n") as stream:
                created.append(target)
                target.chmod(0o600)
                json.dump(document, stream, indent=2, ensure_ascii=False)
                stream.write("\n")
    except BaseException:
        for target in created:
            target.unlink()
        directory.rmdir()
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True,
                        help="New private directory beneath an existing parent; must not exist")
    parser.add_argument("--team-name", required=True)
    parser.add_argument("--member", action="append", required=True, metavar="ID:ROLE:NAME")
    parser.add_argument("--valid-days", type=int, default=90,
                        help="Credential validity in days, default 90; 0 disables expiry")
    args = parser.parse_args()
    try:
        policy, credentials = create_documents(args.team_name, args.member, args.valid_days,
                                                time.time_ns() // 1_000_000)
        write_documents(args.directory, policy, credentials)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(f"Native policy: {args.directory.resolve() / 'team.json'}")
    print(f"Private credentials: {args.directory.resolve() / 'credentials.json'}")
    print("Give each member only their own token; restrict access to the credential file.")


if __name__ == "__main__":
    main()
