"""pytest configuration and shared fixtures for the db test suite.

Run tests from atx-impl/ so that `import db` resolves correctly and
db/calendar.py does not shadow the stdlib calendar module.

Speed model: bootstrapping the full warehouse schema (~60 tables + indexes +
views + catalog seed) costs ~2s. Paying that per test made the suite take
minutes. Instead we build the schema ONCE per session into a template DuckDB
file, then give each test its own fast copy (a file copy + a fresh connection,
no schema rebuild). Each test still gets a fully isolated, writable warehouse.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

# Ensure atx-impl/ is on sys.path so `import db` works regardless of how
# pytest was invoked. pyproject.toml also sets pythonpath=["."] which handles
# the normal case, but being explicit here keeps conftest self-contained.
_ATXIMPL = Path(__file__).resolve().parents[2]
if str(_ATXIMPL) not in sys.path:
    sys.path.insert(0, str(_ATXIMPL))


@pytest.fixture(scope="session")
def _schema_template(tmp_path_factory) -> Path:
    """Build the full warehouse schema ONCE per test session into a template file.

    Uses the real DuckDBStore/ensure_quant_schema/apply_pending_migrations path so
    the template is byte-for-byte what production bootstrap produces (schema +
    migrations + catalog seed), then closes cleanly (checkpoint, no WAL) so it can
    be copied.
    """
    from db.connection import DuckDBStore

    template_path = tmp_path_factory.mktemp("warehouse_template") / "template.duckdb"
    with DuckDBStore(template_path):
        # __enter__ runs initialize() -> ensure_quant_schema + migrations.
        pass
    return template_path


@pytest.fixture
def tmp_store(_schema_template, tmp_path):
    """Yield a fresh, fully-bootstrapped, writable DuckDBStore for one test.

    Copies the session schema template (fast) and opens a new connection WITHOUT
    re-running initialization, so each test gets an isolated warehouse in
    milliseconds instead of rebuilding the schema (~2s) every time.
    """
    import duckdb

    from db.connection import DuckDBStore

    db_path = tmp_path / "test_warehouse.duckdb"
    shutil.copy(_schema_template, db_path)

    store = DuckDBStore(db_path)
    store.connection = duckdb.connect(str(db_path))
    try:
        yield store
    finally:
        if store.connection is not None:
            store.connection.close()
            store.connection = None


@pytest.fixture
def fresh_store(tmp_path):
    """Yield a store built via the FULL real initialize() path (no template copy).

    Use only for tests that must observe the live bootstrap/migration flow itself.
    Slower (~2s) than `tmp_store`; prefer `tmp_store` for everything else.
    """
    from db.connection import DuckDBStore

    db_path = tmp_path / "fresh_warehouse.duckdb"
    with DuckDBStore(db_path) as store:
        yield store
