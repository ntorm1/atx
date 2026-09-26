"""Kill-and-replay drill (index §4 M7, task 1.1): a WAL left by a killed writer replays inside the
budget that ``DuckDBStore`` puts in the connect config, before any SQL statement can run."""

from __future__ import annotations

import os
import subprocess
import sys

import duckdb
import pytest

from atx_db.connection import DuckDBStore

ROWS = 2_000_000
BATCH = 50_000  # below a row group, so the committed rows go to the WAL, not optimistic blocks

WRITER = f"""
import os, sys
import duckdb
con = duckdb.connect(sys.argv[1], config={{"memory_limit": "256MB", "threads": 1,
                                           "preserve_insertion_order": False, "checkpoint_threshold": "100GB"}})
con.execute("CREATE TABLE drill (id BIGINT, v DOUBLE, tag VARCHAR, created_at TIMESTAMP NOT NULL DEFAULT now())")
for start in range(0, {ROWS}, {BATCH}):
    con.execute("INSERT INTO drill (id, v, tag) SELECT range, range * 0.5, 'row-' || range FROM range(?, ?)",
                [start, start + {BATCH}])
sys.stdout.flush()
os._exit(0)  # killed before CHECKPOINT and close: the committed rows live only in the WAL
"""


@pytest.mark.slow
def test_wal_left_by_a_killed_writer_replays_inside_the_connect_budget(tmp_path, monkeypatch):
    path = tmp_path / "drill.duckdb"
    subprocess.run([sys.executable, "-c", WRITER, str(path)], check=True, timeout=900,
                   env={**os.environ, "OPENBLAS_NUM_THREADS": "1"})
    wal = path.with_name(path.name + ".wal")
    assert wal.stat().st_size > 16 * 2 ** 20  # the 2M rows were never checkpointed

    with duckdb.connect(":memory:", config={"memory_limit": "256MB"}) as reference:
        bounded_memory = reference.execute("SELECT current_setting('memory_limit')").fetchone()[0]
    original_connect = duckdb.connect
    first_statements = []

    def observe_connect(*args, **kwargs):
        # DuckDB replays the WAL inside connect(); this is the first statement on the connection.
        con = original_connect(*args, **kwargs)
        first_statements.append(con.execute(
            "SELECT current_setting('memory_limit'), current_setting('threads')").fetchone())
        return con

    monkeypatch.setattr(duckdb, "connect", observe_connect)
    monkeypatch.setattr(DuckDBStore, "initialize", lambda _store: None)  # a drill file, not a warehouse
    store = DuckDBStore(path, memory_limit="256MB", threads=1)
    with store:
        assert first_statements == [(bounded_memory, 1)]
        assert store.con.execute("SELECT count(*), count(created_at), max(id) FROM drill").fetchone() == (
            ROWS, ROWS, ROWS - 1)
        store.close()  # CHECKPOINT folds the replayed WAL into the file
    assert not wal.exists()
