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

import hashlib
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

# Keep per-connection DuckDB thread pools tiny. Under xdist we already have one
# process per core; letting each connection default to (cores) threads would
# oversubscribe badly on the heavy quality-check tests.
_DUCKDB_TEST_THREADS = 1

_SCHEMA_CACHE_DIR = _ATXIMPL / ".pytest_cache" / "db_schema_templates"
_SCHEMA_FINGERPRINT_FILES = (
    _ATXIMPL / "db" / "connection.py",
    _ATXIMPL / "db" / "schema.py",
    _ATXIMPL / "db" / "schema_contract.py",
    _ATXIMPL / "db" / "fundamental_statements.py",
    _ATXIMPL / "db" / "parity.py",
)


def pytest_addoption(parser):
    """Add ``--run-slow`` so the release gate can opt into heavy integration tests.

    The default (fast) lane skips ``@pytest.mark.slow`` tests. A handful of
    integration tests do full warehouse builds, DB backup/restore roundtrips,
    WAL-recovery, and multi-dataset backfills; together they were ~55% of the
    suite's wall time and crushed CPU/disk on every iterative run. They still
    run in the gate (`pytest ... --run-slow`), just not on the inner dev loop.
    """
    parser.addoption(
        "--run-slow",
        action="store_true",
        default=False,
        help="Run @pytest.mark.slow heavy integration tests (default: skipped).",
    )


def pytest_collection_modifyitems(config, items):
    if config.getoption("--run-slow"):
        return
    skip_slow = pytest.mark.skip(reason="slow heavy integration test; pass --run-slow to run")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip_slow)


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


def _schema_fingerprint() -> str:
    """Hash the bootstrap inputs that make a warehouse template valid."""
    import duckdb

    h = hashlib.sha256()
    h.update(f"python={sys.implementation.cache_tag}\n".encode())
    h.update(f"duckdb={duckdb.__version__}\n".encode())
    paths = sorted(
        (
            *_SCHEMA_FINGERPRINT_FILES,
            *(_ATXIMPL / "db" / "migrations").glob("*.py"),
            *(_ATXIMPL / "db" / "seeds").glob("*.csv"),
        ),
        key=lambda p: p.relative_to(_ATXIMPL).as_posix(),
    )
    for path in paths:
        rel = path.relative_to(_ATXIMPL).as_posix()
        h.update(rel.encode())
        h.update(b"\0")
        h.update(path.read_bytes())
        h.update(b"\0")
    return h.hexdigest()[:24]


def _cached_schema_template() -> Path:
    """Return a persistent, fingerprinted schema template shared across runs."""
    from filelock import FileLock

    key = _schema_fingerprint()
    cache_dir = _SCHEMA_CACHE_DIR / key
    cache_dir.mkdir(parents=True, exist_ok=True)
    template_path = cache_dir / "warehouse_template.duckdb"
    ready_path = cache_dir / "warehouse_template.duckdb.ready"

    lock = FileLock(str(cache_dir / "warehouse_template.lock"))
    with lock:
        if template_path.exists() and ready_path.exists():
            return template_path

        tmp_path = cache_dir / f"warehouse_template.{os.getpid()}.tmp.duckdb"
        for stale in (tmp_path, tmp_path.with_suffix(".duckdb.wal")):
            if stale.exists():
                stale.unlink()

        _build_template(tmp_path)
        os.replace(tmp_path, template_path)
        ready_path.write_text(key, encoding="ascii")
    return template_path


@pytest.fixture(scope="session")
def _schema_template(tmp_path_factory) -> Path:
    """A ready-to-copy warehouse schema template, shared across xdist workers.

    The template is cached under ``.pytest_cache`` using a hash of the bootstrap
    code, seed CSVs, Python cache tag, and DuckDB version. Normal iterative runs
    reuse the expensive schema+migration build across pytest invocations.
    """
    # Touch the base temp so pytest still creates/cleans the per-run root in the
    # usual way; the actual reusable template lives in the project cache.
    tmp_path_factory.getbasetemp()
    return _cached_schema_template()


def _open_template_copy(template_path: Path, db_path: Path):
    import duckdb

    from db.connection import DuckDBStore

    shutil.copyfile(template_path, db_path)
    store = DuckDBStore(db_path)
    store.connection = duckdb.connect(str(db_path))
    store._configure_session(store.connection)
    _cap_threads(store.connection)
    store._initialized = True
    return store


def _close_store(store) -> None:
    if store.connection is not None:
        store.connection.close()
        store.connection = None


@pytest.fixture
def tmp_store(_schema_template, tmp_path):
    """Yield a fresh, fully-bootstrapped, writable DuckDBStore for one test.

    Copies the shared schema template (fast) and opens a new connection WITHOUT
    re-running initialization, so each test gets an isolated warehouse in
    milliseconds instead of rebuilding the schema (~180s at 150+ migrations)
    every time.
    """
    db_path = tmp_path / "test_warehouse.duckdb"
    store = _open_template_copy(_schema_template, db_path)
    try:
        yield store
    finally:
        _close_store(store)


@pytest.fixture
def fresh_store(_schema_template, tmp_path):
    """Yield a second independent, writable store for cross-store assertions.

    This uses the same fingerprinted bootstrap template as ``tmp_store`` but a
    separate file copy and connection. It is for tests that need two warehouses;
    tests that must exercise the live initialize() path should create their own
    ``DuckDBStore`` explicitly.
    """
    db_path = tmp_path / "fresh_warehouse.duckdb"
    store = _open_template_copy(_schema_template, db_path)
    try:
        yield store
    finally:
        _close_store(store)


@pytest.fixture
def built_warehouse(_schema_template, tmp_path):
    """Factory -> filesystem path to a fresh, fully-built warehouse file.

    ``path = built_warehouse("my_name.duckdb")`` copies the shared schema template
    to ``tmp_path/my_name.duckdb`` and returns the path. For tests that must
    open/reopen/back up/restore a real warehouse *file* via ``DuckDBStore(path)``
    but do NOT test the build/migration path itself: opening the copy sees
    ``schema_migrations`` already at head, so ``initialize()`` takes the
    schema-current fast path (~0.6s) instead of rebuilding from scratch (~180s at
    150+ migrations). The factory lets each test keep its own filename (some couple
    the WAL path to the db name). Copies are private to the test and fully writable.

    Tests that genuinely exercise the from-scratch build or a partial/old-version
    migration state must still construct their own ``DuckDBStore`` (and stay
    ``@pytest.mark.slow``).
    """

    def _make(name: str = "built_warehouse.duckdb") -> Path:
        db_path = tmp_path / name
        shutil.copyfile(_schema_template, db_path)
        return db_path

    return _make
