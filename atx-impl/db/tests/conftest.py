"""pytest configuration and shared fixtures for the db test suite.

Run tests from atx-impl/ so that `import db` resolves correctly and
db/calendar.py does not shadow the stdlib calendar module.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Ensure atx-impl/ is on sys.path so `import db` works regardless of how
# pytest was invoked. pyproject.toml also sets pythonpath=["."] which handles
# the normal case, but being explicit here keeps conftest self-contained.
_ATXIMPL = Path(__file__).resolve().parents[2]
if str(_ATXIMPL) not in sys.path:
    sys.path.insert(0, str(_ATXIMPL))


@pytest.fixture
def tmp_store(tmp_path):
    """Yield a fresh DuckDBStore bootstrapped in a temp directory.

    Uses the normal DuckDBStore/ensure_quant_schema/apply_pending_migrations path
    so tests exercise the real initialization flow without touching the production DB.
    """
    import db
    from db.connection import DuckDBStore

    db_path = tmp_path / "test_warehouse.duckdb"
    with DuckDBStore(db_path) as store:
        yield store
