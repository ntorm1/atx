"""Versioned migration framework for the atx-impl DuckDB warehouse."""

from __future__ import annotations

import importlib as _importlib
import sys as _sys
import types as _types

from . import _runner as _runner
from . import bodies_0001_0137 as _bodies_0001_0137
from .registry import MIGRATIONS

_runner.MIGRATIONS = MIGRATIONS

for _module_name in (
    "fundamental_statements",
    "industry_templates",
    "panel_contract",
    "quality",
    "schema",
    "schema_contract",
    "security_master",
):
    _sys.modules[f"{__name__}.{_module_name}"] = _importlib.import_module(
        f"..{_module_name}", __name__
    )

# Runner/governance surface.
Migration = _runner.Migration
_migration_version_text = _runner._migration_version_text
_migration_source_checksum = _runner._migration_source_checksum
_migration_by_version = _runner._migration_by_version
_validate_migration_registry = _runner._validate_migration_registry
verify_migration_checksums = _runner.verify_migration_checksums
_backfill_missing_migration_checksums = _runner._backfill_missing_migration_checksums
_ensure_apply_lock_table = _runner._ensure_apply_lock_table
release_apply_lock = _runner.release_apply_lock
claim_apply_lock = _runner.claim_apply_lock
acquire_apply_lock = _runner.acquire_apply_lock
_apply_pending_migrations_unlocked = _runner._apply_pending_migrations_unlocked
apply_pending_migrations = _runner.apply_pending_migrations

# Preserve import compatibility for migration body helpers/constants used by tests and
# any downstream callers that reached through the old single-file module.
for _name, _value in vars(_bodies_0001_0137).items():
    if not _name.startswith("__"):
        globals().setdefault(_name, _value)

# A few callers historically reached imported module/type names through db.migrations.
ast = _runner.ast
contextlib = _runner.contextlib
duckdb = _runner.duckdb
hashlib = _runner.hashlib
inspect = _runner.inspect
dataclass = _runner.dataclass
textwrap = _runner.textwrap
uuid = _runner.uuid
Callable = _runner.Callable

__migration_runner = _runner
__migration_bodies = _bodies_0001_0137


class _MigrationsModule(_types.ModuleType):
    def __setattr__(self, name: str, value: object) -> None:
        super().__setattr__(name, value)
        runner = globals()["__migration_runner"]
        bodies = globals()["__migration_bodies"]
        if name == "MIGRATIONS":
            runner.MIGRATIONS = value
        if hasattr(runner, name):
            setattr(runner, name, value)
        if hasattr(bodies, name):
            setattr(bodies, name, value)


_sys.modules[__name__].__class__ = _MigrationsModule

globals().pop("bodies_0001_0137", None)
globals().pop("bodies_0140_0143", None)
globals().pop("bodies_0144_0147", None)
globals().pop("bodies_0148_0151", None)
globals().pop("registry", None)
del _bodies_0001_0137, _importlib, _module_name, _MigrationsModule, _name, _runner, _sys, _types, _value

__all__ = [
    "MIGRATIONS",
    "Migration",
    "acquire_apply_lock",
    "apply_pending_migrations",
    "claim_apply_lock",
    "release_apply_lock",
    "verify_migration_checksums",
]
