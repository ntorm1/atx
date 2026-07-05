"""Ordered migration registry assembly."""

from __future__ import annotations

from .bodies_0001_0137 import MIGRATIONS


def _validate_registry_versions() -> None:
    versions = [migration.version for migration in MIGRATIONS]
    if versions != sorted(versions):
        raise RuntimeError(f"MIGRATIONS must be sorted ascending: {versions}")
    duplicates = sorted({version for version in versions if versions.count(version) > 1})
    if duplicates:
        formatted = ", ".join(str(version).zfill(4) for version in duplicates)
        raise RuntimeError(f"MIGRATIONS contains duplicate versions: {formatted}")


_validate_registry_versions()
