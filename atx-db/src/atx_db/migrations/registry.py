"""Ordered migration registry assembly."""

from __future__ import annotations

from .bodies_0001_0137 import MIGRATIONS as _MIGRATIONS_0001_0137
from .bodies_0140_0143 import MIGRATIONS as _MIGRATIONS_0140_0143
from .bodies_0144_0147 import MIGRATIONS as _MIGRATIONS_0144_0147
from .bodies_0148_0151 import MIGRATIONS as _MIGRATIONS_0148_0151
from .bodies_0152_0155 import MIGRATIONS as _MIGRATIONS_0152_0155
from .bodies_0156_0159 import MIGRATIONS as _MIGRATIONS_0156_0159
from .bodies_0160_0163 import MIGRATIONS as _MIGRATIONS_0160_0163
from .bodies_0164_0167 import MIGRATIONS as _MIGRATIONS_0164_0167
from .bodies_0176_0179 import MIGRATIONS as _MIGRATIONS_0176_0179
from .bodies_0180_0183 import MIGRATIONS as _MIGRATIONS_0180_0183
from .bodies_0185_0188 import MIGRATIONS as _MIGRATIONS_0185_0188
from .bodies_0189 import MIGRATIONS as _MIGRATIONS_0189
from .bodies_0190 import MIGRATIONS as _MIGRATIONS_0190
from .bodies_0191 import MIGRATIONS as _MIGRATIONS_0191
from .bodies_0192 import MIGRATIONS as _MIGRATIONS_0192
from .bodies_0193 import MIGRATIONS as _MIGRATIONS_0193


MIGRATIONS = [
    *_MIGRATIONS_0001_0137,
    *_MIGRATIONS_0140_0143,
    *_MIGRATIONS_0144_0147,
    *_MIGRATIONS_0148_0151,
    *_MIGRATIONS_0152_0155,
    *_MIGRATIONS_0156_0159,
    *_MIGRATIONS_0160_0163,
    *_MIGRATIONS_0164_0167,
    *_MIGRATIONS_0176_0179,
    *_MIGRATIONS_0180_0183,
    *_MIGRATIONS_0185_0188,
    *_MIGRATIONS_0189,
    *_MIGRATIONS_0190,
    *_MIGRATIONS_0191,
    *_MIGRATIONS_0192,
    *_MIGRATIONS_0193,
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
