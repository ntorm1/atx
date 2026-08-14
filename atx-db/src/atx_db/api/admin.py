"""Local control-plane administration for accounts, keys, and entitlements."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import secrets
import uuid
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from decimal import Decimal
from pathlib import Path

from ..connection import DuckDBStore, resolve_data_dir
from .app import CONTROL_PATH_ENV


def default_control_path() -> Path:
    configured = os.environ.get(CONTROL_PATH_ENV)
    return Path(os.path.expandvars(configured)).expanduser() if configured else resolve_data_dir() / "control.duckdb"


def initialize_control_database(path: Path | str) -> Path:
    resolved = Path(path).resolve()
    with DuckDBStore(resolved):
        pass
    return resolved


def upsert_account(
    path: Path | str,
    *,
    account_id: str,
    account_name: str,
    plan_code: str,
) -> None:
    initialize_control_database(path)
    with DuckDBStore(path) as store:
        store.con.execute(
            """
            INSERT INTO saas_accounts (account_id,account_name,status,plan_code)
            VALUES (?,?, 'active', ?)
            ON CONFLICT (account_id) DO UPDATE SET
                account_name = excluded.account_name,
                plan_code = excluded.plan_code,
                updated_at = now()
            """,
            [account_id, account_name, plan_code],
        )


@dataclass(frozen=True)
class IssuedApiKey:
    key_id: str
    account_id: str
    api_key: str
    key_prefix: str
    scopes: tuple[str, ...]
    expires_at: dt.datetime | None


def issue_api_key(
    path: Path | str,
    *,
    account_id: str,
    scopes: Sequence[str] = ("data:read", "batch:read", "batch:write"),
    expires_at: dt.datetime | None = None,
) -> IssuedApiKey:
    if not scopes or any(not scope.strip() for scope in scopes):
        raise ValueError("at least one non-empty scope is required")
    initialize_control_database(path)
    api_key = f"atx_live_{secrets.token_urlsafe(32)}"
    key_id = str(uuid.uuid4())
    prefix = api_key[:12]
    with DuckDBStore(path) as store:
        account = store.con.execute("SELECT status FROM saas_accounts WHERE account_id = ?", [account_id]).fetchone()
        if account is None:
            raise ValueError(f"unknown account {account_id!r}")
        if account[0] != "active":
            raise ValueError(f"account {account_id!r} is not active")
        store.con.execute(
            """
            INSERT INTO saas_api_keys (
                key_id,account_id,key_prefix,secret_digest,digest_algorithm,status,
                scopes_json,expires_at
            ) VALUES (?,?,?,?, 'sha256', 'active', ?, ?)
            """,
            [
                key_id,
                account_id,
                prefix,
                hashlib.sha256(api_key.encode("utf-8")).hexdigest(),
                json.dumps(sorted(set(scopes)), separators=(",", ":")),
                expires_at,
            ],
        )
    return IssuedApiKey(
        key_id=key_id,
        account_id=account_id,
        api_key=api_key,
        key_prefix=prefix,
        scopes=tuple(sorted(set(scopes))),
        expires_at=expires_at,
    )


def grant_entitlement(
    path: Path | str,
    *,
    account_id: str,
    dataset: str,
    schemas: Sequence[str],
    max_sync_rows: int = 50_000,
    requests_per_minute: int = 600,
    bytes_per_month: int | None = None,
) -> str:
    if not schemas:
        raise ValueError("at least one schema is required")
    if max_sync_rows < 1 or requests_per_minute < 1:
        raise ValueError("row and request limits must be positive")
    initialize_control_database(path)
    entitlement_id = hashlib.sha256(f"{account_id}|{dataset}".encode()).hexdigest()
    with DuckDBStore(path) as store:
        account = store.con.execute("SELECT 1 FROM saas_accounts WHERE account_id = ?", [account_id]).fetchone()
        if account is None:
            raise ValueError(f"unknown account {account_id!r}")
        catalogued = store.con.execute("SELECT 1 FROM api_dataset_catalog WHERE dataset_id = ?", [dataset]).fetchone()
        if catalogued is None:
            raise ValueError(f"unknown public dataset {dataset!r}")
        valid_schemas = {
            str(row[0])
            for row in store.con.execute(
                "SELECT schema_code FROM api_schema_catalog WHERE dataset_id = ?", [dataset]
            ).fetchall()
        }
        unknown = set(schemas) - valid_schemas - {"*"}
        if unknown:
            raise ValueError(f"unknown schemas for {dataset!r}: {sorted(unknown)}")
        store.con.execute(
            """
            INSERT INTO saas_entitlements (
                entitlement_id,account_id,dataset_id,allowed_schemas_json,valid_from,
                max_sync_rows,requests_per_minute,bytes_per_month
            ) VALUES (?,?,?,?,now(),?,?,?)
            ON CONFLICT (entitlement_id) DO UPDATE SET
                allowed_schemas_json = excluded.allowed_schemas_json,
                max_sync_rows = excluded.max_sync_rows,
                requests_per_minute = excluded.requests_per_minute,
                bytes_per_month = excluded.bytes_per_month,
                valid_to = NULL,
                updated_at = now()
            """,
            [
                entitlement_id,
                account_id,
                dataset,
                json.dumps(sorted(set(schemas)), separators=(",", ":")),
                max_sync_rows,
                requests_per_minute,
                bytes_per_month,
            ],
        )
    return entitlement_id


def revoke_api_key(path: Path | str, key_id: str) -> bool:
    initialize_control_database(path)
    with DuckDBStore(path) as store:
        row = store.con.execute(
            """
            UPDATE saas_api_keys
            SET status = 'revoked', revoked_at = now(), updated_at = now()
            WHERE key_id = ? AND status <> 'revoked'
            RETURNING key_id
            """,
            [key_id],
        ).fetchone()
    return row is not None


def set_unit_price(
    path: Path | str,
    *,
    dataset: str,
    schema: str,
    unit_price_per_gb: Decimal,
    mode: str = "historical",
) -> str:
    if unit_price_per_gb < 0:
        raise ValueError("unit price cannot be negative")
    initialize_control_database(path)
    now = dt.datetime.now(dt.UTC)
    price_id = hashlib.sha256(f"{dataset}|{schema}|{mode}|USD|{now.isoformat()}".encode()).hexdigest()
    with DuckDBStore(path) as store:
        catalogued = store.con.execute(
            "SELECT 1 FROM api_schema_catalog WHERE dataset_id = ? AND schema_code = ?",
            [dataset, schema],
        ).fetchone()
        if catalogued is None:
            raise ValueError(f"unknown public schema {dataset!r}/{schema!r}")
        store.con.execute(
            """
            UPDATE api_unit_price_catalog
            SET valid_to = ?, updated_at = now()
            WHERE dataset_id = ? AND schema_code = ? AND mode = ?
              AND valid_to IS NULL AND valid_from < ?
            """,
            [now, dataset, schema, mode, now],
        )
        store.con.execute(
            """
            INSERT INTO api_unit_price_catalog (
                price_id,dataset_id,schema_code,mode,currency,billing_unit,
                unit_price_per_gb,status,valid_from
            ) VALUES (?,?,?,?, 'USD','uncompressed_arrow_bytes',?,'active',?)
            """,
            [price_id, dataset, schema, mode, unit_price_per_gb, now],
        )
    return price_id


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Administer the local ATX SaaS control plane.")
    parser.add_argument("--path", type=Path, default=default_control_path())
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("init", help="Initialize or migrate the control database.")
    account = commands.add_parser("upsert-account", help="Create or update an active customer account.")
    account.add_argument("--account-id", required=True)
    account.add_argument("--name", required=True)
    account.add_argument("--plan", default="institutional")

    key = commands.add_parser("issue-key", help="Issue an API key; the secret is printed exactly once.")
    key.add_argument("--account-id", required=True)
    key.add_argument("--scopes", default="data:read,batch:read,batch:write")

    grant = commands.add_parser("grant", help="Grant or replace a dataset entitlement.")
    grant.add_argument("--account-id", required=True)
    grant.add_argument("--dataset", required=True)
    grant.add_argument("--schemas", required=True)
    grant.add_argument("--max-sync-rows", type=int, default=50_000)
    grant.add_argument("--requests-per-minute", type=int, default=600)
    grant.add_argument("--bytes-per-month", type=int)

    revoke = commands.add_parser("revoke-key", help="Revoke an API key immediately.")
    revoke.add_argument("--key-id", required=True)

    price = commands.add_parser("set-price", help="Version a schema's historical USD/GB price.")
    price.add_argument("--dataset", required=True)
    price.add_argument("--schema", required=True)
    price.add_argument("--usd-per-gb", type=Decimal, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    args = _parser().parse_args(argv)
    path: Path = args.path
    if args.command == "init":
        print(json.dumps({"control_database": str(initialize_control_database(path))}))
    elif args.command == "upsert-account":
        upsert_account(path, account_id=args.account_id, account_name=args.name, plan_code=args.plan)
        print(json.dumps({"account_id": args.account_id, "status": "active"}))
    elif args.command == "issue-key":
        issued = issue_api_key(path, account_id=args.account_id, scopes=args.scopes.split(","))
        print(json.dumps(asdict(issued), default=str))
    elif args.command == "grant":
        entitlement_id = grant_entitlement(
            path,
            account_id=args.account_id,
            dataset=args.dataset,
            schemas=args.schemas.split(","),
            max_sync_rows=args.max_sync_rows,
            requests_per_minute=args.requests_per_minute,
            bytes_per_month=args.bytes_per_month,
        )
        print(json.dumps({"entitlement_id": entitlement_id}))
    elif args.command == "revoke-key":
        print(json.dumps({"key_id": args.key_id, "revoked": revoke_api_key(path, args.key_id)}))
    elif args.command == "set-price":
        price_id = set_unit_price(
            path,
            dataset=args.dataset,
            schema=args.schema,
            unit_price_per_gb=args.usd_per_gb,
        )
        print(json.dumps({"price_id": price_id}))


if __name__ == "__main__":
    main()
