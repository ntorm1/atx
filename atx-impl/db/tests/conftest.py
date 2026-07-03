"""pytest configuration and shared fixtures for the db test suite.

Run tests from atx-impl/ so that `import db` resolves correctly and
db/calendar.py does not shadow the stdlib calendar module.

Speed model
-----------
Bootstrapping the full warehouse schema (~60 tables + indexes + views + catalog
seed) costs ~9s. Paying that per test made the suite take minutes. Two levers:

1. Schema template: build the schema ONCE into a template DuckDB file, then give
   each test its own fast copy (a file copy + a fresh connection, no schema
   rebuild). Each test still gets a fully isolated, writable warehouse.

2. Parallelism: the suite runs under pytest-xdist (``-n auto``). Every test uses
   an isolated, per-test file DB, so tests are process-safe. The schema template
   is shared across xdist workers behind a file lock so the ~9s build happens
   ONCE for the whole run, not once per worker.

Connections are capped to a small DuckDB thread count so N parallel workers do
not each spin up (cores) threads and oversubscribe the box.
"""

from __future__ import annotations

import os
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

# Keep per-connection DuckDB thread pools small. Under xdist we already have one
# process per core; letting each connection default to (cores) threads would
# oversubscribe badly on the heavy quality-check tests.
_DUCKDB_TEST_THREADS = 2


def _cap_threads(con) -> None:
    try:
        con.execute(f"PRAGMA threads={_DUCKDB_TEST_THREADS}")
    except Exception:
        pass


def _build_template(dest: Path) -> None:
    """Build the full warehouse schema into ``dest`` via the real bootstrap path.

    Uses DuckDBStore/ensure_quant_schema/apply_pending_migrations so the template
    is byte-for-byte what production bootstrap produces (schema + migrations +
    catalog seed), then closes cleanly (checkpoint, no WAL) so it can be copied.
    """
    from db.connection import DuckDBStore

    with DuckDBStore(dest) as store:
        # __enter__ runs initialize() -> ensure_quant_schema + migrations.
        _cap_threads(store.con)


@pytest.fixture(scope="session")
def _schema_template(tmp_path_factory) -> Path:
    """A ready-to-copy warehouse schema template, shared across xdist workers.

    The template is built exactly once per test run. Under xdist, workers race
    for a file lock; the winner builds, the rest wait and reuse the artifact.
    Without xdist this collapses to a plain single build.
    """
    # getbasetemp() is per-worker (…/popen-gw0); its parent is shared by all
    # workers of a run, so the template and its lock live there.
    root = tmp_path_factory.getbasetemp().parent
    template_path = root / "warehouse_template.duckdb"

    worker = os.environ.get("PYTEST_XDIST_WORKER")
    if worker is None:
        # Serial run: no cross-process coordination needed.
        if not template_path.exists():
            _build_template(template_path)
        return template_path

    from filelock import FileLock

    lock = FileLock(str(template_path) + ".lock")
    with lock:
        done = template_path.with_suffix(".duckdb.ready")
        if not done.exists():
            _build_template(template_path)
            done.touch()
    return template_path


@pytest.fixture
def tmp_store(_schema_template, tmp_path):
    """Yield a fresh, fully-bootstrapped, writable DuckDBStore for one test.

    Copies the shared schema template (fast) and opens a new connection WITHOUT
    re-running initialization, so each test gets an isolated warehouse in
    milliseconds instead of rebuilding the schema (~9s) every time.
    """
    import duckdb

    from db.connection import DuckDBStore

    db_path = tmp_path / "test_warehouse.duckdb"
    shutil.copy(_schema_template, db_path)

    store = DuckDBStore(db_path)
    store.connection = duckdb.connect(str(db_path))
    _cap_threads(store.connection)
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
    Slower (~9s) than `tmp_store`; prefer `tmp_store` for everything else.
    """
    from db.connection import DuckDBStore

    db_path = tmp_path / "fresh_warehouse.duckdb"
    with DuckDBStore(db_path) as store:
        _cap_threads(store.con)
        yield store
