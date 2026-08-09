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
from .bodies_0194 import MIGRATIONS as _MIGRATIONS_0194
from .bodies_0195 import MIGRATIONS as _MIGRATIONS_0195
from .bodies_0196 import MIGRATIONS as _MIGRATIONS_0196
from .bodies_0197 import MIGRATIONS as _MIGRATIONS_0197
from .bodies_0198 import MIGRATIONS as _MIGRATIONS_0198
from .bodies_0199 import MIGRATIONS as _MIGRATIONS_0199
from .bodies_0200 import MIGRATIONS as _MIGRATIONS_0200
from .bodies_0201 import MIGRATIONS as _MIGRATIONS_0201
from .bodies_0202 import MIGRATIONS as _MIGRATIONS_0202
from .bodies_0203 import MIGRATIONS as _MIGRATIONS_0203
from .bodies_0204 import MIGRATIONS as _MIGRATIONS_0204
from .bodies_0205 import MIGRATIONS as _MIGRATIONS_0205
from .bodies_0206 import MIGRATIONS as _MIGRATIONS_0206
from .bodies_0207 import MIGRATIONS as _MIGRATIONS_0207
from .bodies_0208 import MIGRATIONS as _MIGRATIONS_0208
from .bodies_0209 import MIGRATIONS as _MIGRATIONS_0209
from .bodies_0210 import MIGRATIONS as _MIGRATIONS_0210
from .bodies_0211 import MIGRATIONS as _MIGRATIONS_0211
from .bodies_0212 import MIGRATIONS as _MIGRATIONS_0212
from .bodies_0213 import MIGRATIONS as _MIGRATIONS_0213
from .bodies_0214 import MIGRATIONS as _MIGRATIONS_0214
from .bodies_0215 import MIGRATIONS as _MIGRATIONS_0215
from .bodies_0216 import MIGRATIONS as _MIGRATIONS_0216
from .bodies_0217 import MIGRATIONS as _MIGRATIONS_0217
from .bodies_0218 import MIGRATIONS as _MIGRATIONS_0218


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
    *_MIGRATIONS_0194,
    *_MIGRATIONS_0195,
    *_MIGRATIONS_0196,
    *_MIGRATIONS_0197,
    *_MIGRATIONS_0198,
    *_MIGRATIONS_0199,
    *_MIGRATIONS_0200,
    *_MIGRATIONS_0201,
    *_MIGRATIONS_0202,
    *_MIGRATIONS_0203,
    *_MIGRATIONS_0204,
    *_MIGRATIONS_0205,
    *_MIGRATIONS_0206,
    *_MIGRATIONS_0207,
    *_MIGRATIONS_0208,
    *_MIGRATIONS_0209,
    *_MIGRATIONS_0210,
    *_MIGRATIONS_0211,
    *_MIGRATIONS_0212,
    *_MIGRATIONS_0213,
    *_MIGRATIONS_0214,
    *_MIGRATIONS_0215,
    *_MIGRATIONS_0216,
    *_MIGRATIONS_0217,
    *_MIGRATIONS_0218,
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
