"""The shared schema-template cache must never serve a template built from another registry.

A pytest session imports the migration registry at collection time. If a migration is
added on disk afterwards, that session still builds its template from the older imported
registry; the fingerprint must therefore cover the imported registry, and a template that
is not at the imported registry's head must never be marked ready.
"""

from __future__ import annotations

import shutil

import duckdb
import pytest

from tests import conftest


def test_fingerprint_covers_the_imported_registry_not_only_files_on_disk(monkeypatch):
    import atx_db.migrations as migrations

    current = conftest._schema_fingerprint()
    # Same files on disk, but this "session" imported a registry one migration behind.
    monkeypatch.setattr(migrations, "MIGRATIONS", list(migrations.MIGRATIONS)[:-1])
    assert conftest._schema_fingerprint() != current


def test_a_template_behind_the_imported_registry_is_never_marked_ready(monkeypatch, tmp_path, _schema_template):
    from atx_db.migrations import MIGRATIONS

    head = max(migration.version for migration in MIGRATIONS)

    def stale_build(dest):
        # A template built by an older registry: one migration short of the imported head.
        shutil.copyfile(_schema_template, dest)
        con = duckdb.connect(str(dest), config={"memory_limit": "256MB", "threads": 1})
        try:
            con.execute(
                "DELETE FROM schema_migrations WHERE version ~ '^[0-9]+$' AND CAST(version AS INTEGER) = ?",
                [head],
            )
            con.execute("CHECKPOINT")
        finally:
            con.close()

    monkeypatch.setattr(conftest, "_SCHEMA_CACHE_DIR", tmp_path / "templates")
    monkeypatch.setattr(conftest, "_build_template", stale_build)
    with pytest.raises(RuntimeError, match=rf"at migration {head - 1}, but the imported registry head is {head}"):
        conftest._cached_schema_template()
    cache_dir = next((tmp_path / "templates").iterdir())
    assert not (cache_dir / "warehouse_template.duckdb.ready").exists()
    assert not (cache_dir / "warehouse_template.duckdb").exists()
    assert not list(cache_dir.glob("warehouse_template.*.tmp.duckdb"))


def test_the_shared_template_is_at_the_imported_registry_head(_schema_template):
    conftest._assert_template_at_registry_head(_schema_template)
