"""Versioned migration framework for the atx-db DuckDB warehouse."""

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
globals().pop("bodies_0152_0155", None)
globals().pop("bodies_0156_0159", None)
globals().pop("bodies_0160_0163", None)
globals().pop("bodies_0164_0167", None)
globals().pop("bodies_0176_0179", None)
globals().pop("bodies_0180_0183", None)
globals().pop("bodies_0185_0188", None)
globals().pop("bodies_0189", None)
globals().pop("bodies_0190", None)
globals().pop("bodies_0191", None)
globals().pop("bodies_0192", None)
globals().pop("bodies_0193", None)
globals().pop("bodies_0194", None)
globals().pop("bodies_0195", None)
globals().pop("bodies_0196", None)
globals().pop("bodies_0197", None)
globals().pop("bodies_0198", None)
globals().pop("bodies_0199", None)
globals().pop("bodies_0200", None)
globals().pop("bodies_0201", None)
globals().pop("bodies_0202", None)
globals().pop("bodies_0203", None)
globals().pop("bodies_0204", None)
globals().pop("bodies_0205", None)
globals().pop("bodies_0206", None)
globals().pop("bodies_0207", None)
globals().pop("bodies_0208", None)
globals().pop("bodies_0209", None)
globals().pop("bodies_0210", None)
globals().pop("bodies_0211", None)
globals().pop("bodies_0212", None)
globals().pop("bodies_0213", None)
globals().pop("bodies_0214", None)
globals().pop("bodies_0215", None)
globals().pop("bodies_0216", None)
globals().pop("bodies_0217", None)
globals().pop("bodies_0218", None)
globals().pop("bodies_0219", None)
globals().pop("bodies_0220", None)
globals().pop("bodies_0221", None)
globals().pop("bodies_0222", None)
globals().pop("bodies_0223", None)
globals().pop("bodies_0224", None)
globals().pop("bodies_0225", None)
globals().pop("bodies_0226", None)
globals().pop("bodies_0227", None)
globals().pop("bodies_0228", None)
globals().pop("bodies_0229", None)
globals().pop("bodies_0230", None)
globals().pop("bodies_0231", None)
globals().pop("bodies_0232", None)
globals().pop("bodies_0233", None)
globals().pop("bodies_0234", None)
globals().pop("bodies_0235", None)
globals().pop("bodies_0236", None)
globals().pop("bodies_0237", None)
globals().pop("bodies_0238", None)
globals().pop("bodies_0239", None)
globals().pop("bodies_0240", None)
globals().pop("bodies_0241", None)
globals().pop("bodies_0242", None)
globals().pop("bodies_0243", None)
globals().pop("bodies_0244", None)
globals().pop("bodies_0245", None)
globals().pop("bodies_0246", None)
globals().pop("bodies_0247", None)
globals().pop("bodies_0248", None)
globals().pop("bodies_0249", None)
globals().pop("bodies_0250", None)
globals().pop("bodies_0251", None)
globals().pop("bodies_0252", None)
globals().pop("bodies_0253", None)
globals().pop("bodies_0254", None)
globals().pop("bodies_0255", None)
globals().pop("bodies_0256", None)
globals().pop("bodies_0257", None)
globals().pop("bodies_0258", None)
globals().pop("bodies_0259", None)
globals().pop("bodies_0260", None)
globals().pop("bodies_0261", None)
globals().pop("bodies_0262", None)
globals().pop("bodies_0263", None)
globals().pop("bodies_0264", None)
globals().pop("bodies_0265", None)
globals().pop("bodies_0266", None)
globals().pop("bodies_0267", None)
globals().pop("bodies_0268", None)
globals().pop("bodies_0269", None)
globals().pop("bodies_0270", None)
globals().pop("bodies_0271", None)
globals().pop("bodies_0272", None)
globals().pop("bodies_0273", None)
globals().pop("bodies_0274", None)
globals().pop("bodies_0275", None)
globals().pop("bodies_0276", None)
globals().pop("bodies_0277", None)
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
