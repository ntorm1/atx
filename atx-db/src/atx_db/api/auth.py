"""API-key authentication and dataset entitlement primitives."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

import duckdb

API_KEYS_ENV = "ATX_DB_API_KEYS_JSON"


@dataclass(frozen=True)
class ApiPrincipal:
    account_id: str
    key_id: str
    scopes: frozenset[str]
    datasets: frozenset[str]
    allowed_schemas: Mapping[str, frozenset[str]] = field(default_factory=dict)
    max_sync_rows_by_dataset: Mapping[str, int] = field(default_factory=dict)
    requests_per_minute_by_dataset: Mapping[str, int] = field(default_factory=dict)
    bytes_per_month_by_dataset: Mapping[str, int | None] = field(default_factory=dict)

    def has_scope(self, scope: str) -> bool:
        return scope in self.scopes or "*" in self.scopes

    def can_read(self, dataset: str, schema: str | None = None) -> bool:
        if not self.has_scope("data:read") or not ("*" in self.datasets or dataset in self.datasets):
            return False
        allowed = self.allowed_schemas.get(dataset)
        return schema is None or allowed is None or "*" in allowed or schema in allowed

    def max_sync_rows(self, dataset: str, default: int) -> int:
        return min(default, self.max_sync_rows_by_dataset.get(dataset, default))

    def requests_per_minute(self, dataset: str) -> int | None:
        return self.requests_per_minute_by_dataset.get(dataset)

    def bytes_per_month(self, dataset: str) -> int | None:
        return self.bytes_per_month_by_dataset.get(dataset)


class Authenticator(Protocol):
    def authenticate(self, api_key: str) -> ApiPrincipal | None: ...


def _digest(api_key: str) -> bytes:
    return hashlib.sha256(api_key.encode("utf-8")).digest()


class StaticApiKeyAuthenticator:
    """Authenticate high-entropy keys without retaining their plaintext values.

    This adapter is intended for local and single-node deployments.  The production
    control plane uses the same ``Authenticator`` interface with a durable key store.
    """

    def __init__(self, principals: Mapping[str, ApiPrincipal]) -> None:
        self._principals = tuple((_digest(key), principal) for key, principal in principals.items())

    def authenticate(self, api_key: str) -> ApiPrincipal | None:
        candidate = _digest(api_key)
        for expected, principal in self._principals:
            if hmac.compare_digest(candidate, expected):
                return principal
        return None

    @classmethod
    def from_environment(cls) -> StaticApiKeyAuthenticator:
        payload = os.environ.get(API_KEYS_ENV, "{}")
        parsed = json.loads(payload)
        if not isinstance(parsed, dict):
            raise ValueError(f"{API_KEYS_ENV} must be a JSON object keyed by API key")
        principals: dict[str, ApiPrincipal] = {}
        for api_key, raw in parsed.items():
            if not isinstance(api_key, str) or not isinstance(raw, dict):
                raise ValueError(f"{API_KEYS_ENV} entries must map strings to objects")
            account_id = raw.get("account_id")
            key_id = raw.get("key_id")
            scopes = raw.get("scopes", ["data:read"])
            datasets = raw.get("datasets", ["*"])
            if not isinstance(account_id, str) or not isinstance(key_id, str):
                raise ValueError("each API key needs string account_id and key_id")
            if not isinstance(scopes, list) or not all(isinstance(value, str) for value in scopes):
                raise ValueError("scopes must be a list of strings")
            if not isinstance(datasets, list) or not all(isinstance(value, str) for value in datasets):
                raise ValueError("datasets must be a list of strings")
            principals[api_key] = ApiPrincipal(
                account_id=account_id,
                key_id=key_id,
                scopes=frozenset(scopes),
                datasets=frozenset(datasets),
            )
        return cls(principals)


class DuckDBApiKeyAuthenticator:
    """Durable local authenticator backed by the SaaS control-plane tables."""

    def __init__(self, database_path: Path | str) -> None:
        self.database_path = Path(database_path)

    def authenticate(self, api_key: str) -> ApiPrincipal | None:
        digest = hashlib.sha256(api_key.encode("utf-8")).hexdigest()
        with duckdb.connect(str(self.database_path)) as conn:
            rows = conn.execute(
                """
                SELECT k.key_id,k.account_id,k.secret_digest,k.digest_algorithm,k.scopes_json
                FROM saas_api_keys k
                JOIN saas_accounts a USING (account_id)
                WHERE k.status = 'active'
                  AND a.status = 'active'
                  AND (k.expires_at IS NULL OR k.expires_at > now())
                  AND starts_with(?, k.key_prefix)
                """,
                [api_key],
            ).fetchall()
            matched = next(
                (
                    row
                    for row in rows
                    if str(row[3]).lower() == "sha256" and hmac.compare_digest(digest, str(row[2]).lower())
                ),
                None,
            )
            if matched is None:
                return None
            key_id, account_id, _, _, scopes_json = matched
            entitlement_rows = conn.execute(
                """
                SELECT dataset_id,allowed_schemas_json,max_sync_rows,
                       requests_per_minute,bytes_per_month
                FROM saas_entitlements
                WHERE account_id = ?
                  AND valid_from <= now()
                  AND (valid_to IS NULL OR valid_to > now())
                """,
                [account_id],
            ).fetchall()
            conn.execute(
                "UPDATE saas_api_keys SET last_used_at = now(), updated_at = now() WHERE key_id = ?",
                [key_id],
            )

        allowed_schemas: dict[str, frozenset[str]] = {}
        maximums: dict[str, int] = {}
        request_limits: dict[str, int] = {}
        byte_limits: dict[str, int | None] = {}
        for dataset_id, allowed_json, max_sync_rows, requests_per_minute, bytes_per_month in entitlement_rows:
            values = json.loads(str(allowed_json))
            if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
                continue
            dataset = str(dataset_id)
            allowed_schemas[dataset] = frozenset(values)
            maximums[dataset] = int(max_sync_rows)
            request_limits[dataset] = int(requests_per_minute)
            byte_limits[dataset] = None if bytes_per_month is None else int(bytes_per_month)
        scopes = json.loads(str(scopes_json))
        if not isinstance(scopes, list) or not all(isinstance(value, str) for value in scopes):
            return None
        return ApiPrincipal(
            account_id=str(account_id),
            key_id=str(key_id),
            scopes=frozenset(scopes),
            datasets=frozenset(allowed_schemas),
            allowed_schemas=allowed_schemas,
            max_sync_rows_by_dataset=maximums,
            requests_per_minute_by_dataset=request_limits,
            bytes_per_month_by_dataset=byte_limits,
        )
