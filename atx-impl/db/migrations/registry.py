"""Ordered migration registry assembly."""

from __future__ import annotations

from .bodies_0001_0137 import MIGRATIONS as _MIGRATIONS_0001_0137
from .bodies_0140_0143 import MIGRATIONS as _MIGRATIONS_0140_0143
from .bodies_0144_0147 import MIGRATIONS as _MIGRATIONS_0144_0147
from .bodies_0148_0151 import MIGRATIONS as _MIGRATIONS_0148_0151
from .bodies_0152_0155 import MIGRATIONS as _MIGRATIONS_0152_0155


MIGRATIONS = [
    *_MIGRATIONS_0001_0137,
    *_MIGRATIONS_0140_0143,
    *_MIGRATIONS_0144_0147,
    *_MIGRATIONS_0148_0151,
    *_MIGRATIONS_0152_0155,
]


def _validate_registry_versions() -> None:
    versions = [migration.version for migration in MIGRATIONS]
    if versions != sorted(versions):
        raise RuntimeError(f"MIGRATIONS must be sorted ascending: {versions}")
    duplicates = sorted({version for version in versions if versions.count(version) > 1})
    if duplicates:
        formatted = ", ".join(str(version).zfill(4) for version in duplicates)
        raise RuntimeError(f"MIGRATIONS contains duplicate versions: {formatted}")


_validate_registry_versions()
