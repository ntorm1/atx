"""Kill-and-resume e2e of a ledgered batch stage (tier-1 v2 1.3; the one allowed test of the node).

A scratch warehouse (the head schema template) gets ~2M real bars of the retained
``TickerHistory3.parquet`` loaded the way the old loader stored them (vendor shares in THOUSANDS,
``market_cap_usd = shares x close``) plus the file's two C-81 suspect lines, a unit-aware run
(stored units, kept) and another source (untouched). ``bars_unit_correction`` then runs twice:

* clean: one slice of 16 batches, then the finalize slice;
* killed: slice workers are terminated (TerminateProcess) at random points inside a batch until
  three kills have landed mid-batch (proved by the next worker restarting that same batch),
  then a slice runs to completion and the finalize slice publishes.

Asserted: both published tables are identical (``EXCEPT ALL`` both ways = 0, ledger too), every
value equals the seed x1000 / NULL (suspect) / unchanged (kept, other source), 0 duplicate keys,
exactly 16 ledgered batches, no WAL; ``recover_stale_run`` refuses while a live guard receipt
names the file, then marks the seeded stale ``running`` rows failed (0 left, ``verify_schema ==
()``, no WAL).

Slow (``--run-slow``), needs the retained parquet, and runs only under the memory guard (its
slice workers refuse to run unguarded):

    run_memory_guarded.py --job-gb 0.6 --wait-minutes 30 -- python -m pytest \\
        tests/test_batch_runner_kill_resume.py -q -p no:cacheprovider -n 0 --run-slow
"""

from __future__ import annotations

import hashlib
import json
import os
import queue
import random
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

import duckdb
import pytest

pytestmark = pytest.mark.slow

ATX_DB = Path(__file__).resolve().parents[1]
RUN_SLICES = ATX_DB / "scripts" / "run_slices.py"
RECOVER = ATX_DB / "scripts" / "recover_stale_run.py"
TH3 = Path(os.environ.get("ATX_TEST_TH3", "C:/atx/atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet"))
SOURCE = "tbltickerhistory3_10y"
LOAD_RUN = "kr-old-parquet-load"
UNITS_RUN = "kr-unit-aware-load"
OTHER_SOURCE = "kr_other_source"
SUSPECT_VENDOR_IDS = (6130036, 71050)  # the file's two C-81 lines (1.2 inventory: 33 rows above 1e8)
STAGE, RUN_KEY = "bars_unit_correction", "kr-e2e"
WORKER_MEMORY = "160MB"
KILLS = 3
THOUSANDS_ROW_CEILING = 100_000_000  # migrations.bodies_0327.THOUSANDS_ROW_CEILING


def _connect(path: Path | str, tmp: Path, *, read_only: bool = False) -> duckdb.DuckDBPyConnection:
    con = duckdb.connect(str(path), read_only=read_only,
                         config={"memory_limit": "256MB", "threads": 1, "preserve_insertion_order": False,
                                 "temp_directory": str(tmp / f"spill-{os.getpid()}-{time.monotonic_ns()}")})
    con.execute("SET TimeZone = 'UTC'")
    return con


def _seed(db: Path, tmp: Path) -> tuple[dict[str, int], dict[str, dict[str, int]]]:
    """~2M real bars as the old parquet loader stored them, plus the kept/untouched controls and stale rows."""
    con = _connect(db, tmp)
    try:
        bars = f"""
            SELECT '{SOURCE}' AS source, 'TH3-' || securityID AS security_id,
                   CAST(securityID AS VARCHAR) AS vendor_security_id,
                   coalesce(nullif(trim(ticker_tk), ''), nullif(trim(todayTicker), ''), 'UNK') AS symbol,
                   CAST(tradingDate AS DATE) AS trade_date, open, high, low, close,
                   CASE WHEN close > 0 AND cumulReturnFactor > 0 THEN close * cumulReturnFactor END AS adjusted_close,
                   CAST(volume AS BIGINT) AS volume, NULL::DOUBLE AS vwap, NULL::DOUBLE AS dividend_amount,
                   NULL::DOUBLE AS split_factor, false AS is_adjusted,
                   CAST(tradingDate AS DATE) + INTERVAL 22 HOUR AS available_at, ? AS run_id,
                   TIMESTAMP '2026-09-20 18:00:09' AS source_loaded_at, NULL::DATE AS as_of_date,
                   NULL::BOOLEAN AS is_latest_revision, CAST(shares AS BIGINT) AS shares_outstanding,
                   CAST(shares AS BIGINT) * close AS market_cap_usd
            FROM read_parquet(?)
            WHERE securityID > 0 AND tradingDate IS NOT NULL AND {{where}}
            QUALIFY count(*) OVER (PARTITION BY securityID, CAST(tradingDate AS DATE)) = 1
        """
        suspects = ", ".join(str(v) for v in SUSPECT_VENDOR_IDS)
        sample: dict[str, dict[str, int]] = {}
        for run, predicate in ((LOAD_RUN, f"(securityID % 16 = 3 OR securityID IN ({suspects}))"),
                               (UNITS_RUN, "securityID % 256 = 21")):
            row = con.execute(f"""
                SELECT count(*), count(*) FILTER (WHERE key_rows = 1), count(*) FILTER (WHERE key_rows > 1)
                FROM (SELECT count(*) OVER (PARTITION BY securityID, CAST(tradingDate AS DATE)) AS key_rows
                      FROM read_parquet(?)
                      WHERE securityID > 0 AND tradingDate IS NOT NULL AND {predicate})
            """, [str(TH3)]).fetchone()
            assert row is not None
            sample[run] = dict(zip(("selected_raw_rows", "unique_key_rows", "duplicate_key_rows_excluded"),
                                   map(int, row), strict=True))
        con.execute("INSERT INTO equity_daily_bars BY NAME "
                    + bars.format(where=f"(securityID % 16 = 3 OR securityID IN ({suspects}))"), [LOAD_RUN, str(TH3)])
        # A unit-aware loader run (params name shares_unit => stored units: kept), on other lines.
        con.execute(f"""
            INSERT INTO equity_daily_bars BY NAME
            SELECT * REPLACE ('TH3U-' || vendor_security_id AS security_id,
                              shares_outstanding * 1000 AS shares_outstanding,
                              market_cap_usd * 1000 AS market_cap_usd)
            FROM ({bars.format(where="securityID % 256 = 21")})""", [UNITS_RUN, str(TH3)])
        # Another source on the same keys: never touched by the stage.
        con.execute(f"""
            INSERT INTO equity_daily_bars BY NAME
            SELECT * REPLACE ('{OTHER_SOURCE}' AS source, 'kr-other-run' AS run_id)
            FROM equity_daily_bars WHERE run_id = ? AND trade_date >= DATE '2026-06-01'""", [LOAD_RUN])
        stamp = "TIMESTAMP '2026-09-20 18:00:00'"
        con.execute(f"""
            INSERT INTO dataset_runs (run_id, dataset_id, status, started_at, finished_at, rows_loaded, source,
                                      params_json, error_message)
            VALUES (?, 'tbltickerhistory_daily', 'succeeded', {stamp}, {stamp}, 0, ?, ?, NULL),
                   (?, 'tbltickerhistory_daily', 'succeeded', {stamp}, {stamp}, 0, ?, ?, NULL),
                   ('kr-stale-dataset-run', 'sec_submissions', 'running', {stamp}, NULL, NULL, 'SEC', '{{}}', NULL)
        """, [LOAD_RUN, SOURCE, json.dumps({"source_path": str(TH3), "tsv_path": None}),
              UNITS_RUN, SOURCE, json.dumps({"source_path": str(TH3), "shares_unit": "thousands"})])
        con.execute(f"""
            INSERT INTO activation_stage_runs (stage, run_id, status, started_at, finished_at, "rows", params_json,
                                               error)
            VALUES ('submissions_load', 'kr-activation', 'running', {stamp}, NULL, NULL, '{{}}', NULL)""")
        counts = dict(con.execute(
            "SELECT run_id, count(*) FROM equity_daily_bars GROUP BY 1").fetchall())
        con.execute("CHECKPOINT")
    finally:
        con.close()
    return {str(k): int(v) for k, v in counts.items()}, sample


class _Worker:
    """One ``run_slices.py --single-slice`` process with its JSON events read on a thread."""

    def __init__(self, db: Path, phase: str, log_dir: Path, tag: str) -> None:
        self.result = log_dir / f"{tag}.result.json"
        self.stderr = (log_dir / f"{tag}.err.log").open("w", encoding="utf-8")
        self.stdout = (log_dir / f"{tag}.events.jsonl").open("w", encoding="utf-8")
        self.eof = False
        argv = [sys.executable, str(RUN_SLICES), "--db", str(db), "--stage", STAGE, "--run-key", RUN_KEY,
                "--single-slice", "--phase", phase, "--result-json", str(self.result), "--max-batches", "16",
                "--max-minutes", "30", "--duckdb-memory", WORKER_MEMORY, "--code-sha", "kr-test"]
        self.process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=self.stderr, text=True,
                                        env={**os.environ, "OPENBLAS_NUM_THREADS": "1"})
        self.events: queue.Queue[dict[str, Any] | None] = queue.Queue()
        self.seen: list[dict[str, Any]] = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.stdout.write(line)
            self.stdout.flush()
            try:
                self.events.put(json.loads(line))
            except ValueError:
                continue
        self.stdout.close()
        self.events.put(None)

    def next_event(self, timeout: float = 600.0) -> dict[str, Any] | None:
        event = self.events.get(timeout=timeout)
        if event is not None:
            self.seen.append(event)
        else:
            self.eof = True
        return event

    def kill(self) -> None:
        assert self.process.poll() is None, "worker already ended"
        if os.name == "nt":
            # Kill this freshly launched process tree, including the Windows venv redirector's child.
            done = subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"],
                                  capture_output=True, text=True, timeout=30, check=False)
            assert done.returncode == 0, done.stderr
        else:
            self.process.kill()

    def finish(self, timeout: float = 1200.0) -> int:
        code = self.process.wait(timeout=timeout)
        while not self.eof and self.next_event(timeout=30) is not None:
            pass
        self.stderr.close()
        return code

    def outcome(self) -> dict[str, Any]:
        return json.loads(self.result.read_text(encoding="utf-8"))


def _run_to_end(db: Path, phase: str, log_dir: Path, tag: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    worker = _Worker(db, phase, log_dir, tag)
    code = worker.finish()
    outcome = worker.outcome()
    assert code == 0, (tag, outcome.get("error"), outcome.get("traceback"))
    return outcome, worker.seen


def _count(con: duckdb.DuckDBPyConnection, sql: str, params: list[object] | None = None) -> int:
    row = con.execute(sql, params or []).fetchone()
    return int(row[0]) if row else 0


@pytest.mark.skipif(not TH3.is_file(), reason="retained TickerHistory3.parquet not present")
def test_bars_unit_correction_kill_resume_matches_clean_run(tmp_path: Path) -> None:
    if not os.environ.get("ATX_GUARD_JOB"):
        pytest.skip("run under run_memory_guarded.py: slice workers refuse to run unguarded")
    # The retained-file drill is run from a committed export. Avoid the shared fixture's
    # implicit connection budget; all bootstrap and WAL replay uses 256MB/one thread.
    from atx_db.connection import DuckDBStore

    seed_db = tmp_path / "kr_seed.duckdb"
    with DuckDBStore(seed_db, memory_limit="256MB", threads=1) as store:
        store.initialize()
        store.con.execute("CHECKPOINT")
    counts, sample = _seed(seed_db, tmp_path)
    assert all(counts[run] == values["unique_key_rows"] for run, values in sample.items())
    assert 1_500_000 <= counts[LOAD_RUN] <= 2_600_000, counts
    clean_db, killed_db = tmp_path / "kr_clean.duckdb", tmp_path / "kr_killed.duckdb"
    shutil.copyfile(seed_db, clean_db)
    shutil.copyfile(seed_db, killed_db)
    logs = tmp_path / "logs"
    logs.mkdir()

    # Clean reference run.
    outcome, events = _run_to_end(clean_db, "batches", logs, "clean-batches")
    assert outcome["stopped"] == "complete" and outcome["remaining"] == 0 and len(outcome["committed"]) == 16
    batch_seconds = [float(e["seconds"]) for e in events if e.get("event") == "batch_committed"]
    _run_to_end(clean_db, "finalize", logs, "clean-finalize")

    # Killed run: terminate workers inside a batch until three kills are proved mid-batch.
    rng = random.Random(int(time.time()))
    typical = statistics.median(batch_seconds)
    kills: list[dict[str, Any]] = []
    killed_pids: list[int] = []
    pending_kill: dict[str, Any] | None = None
    attempt = 0
    while sum(1 for k in kills if k["mid_batch"]) < KILLS and attempt < 12:
        attempt += 1
        worker = _Worker(killed_db, "batches", logs, f"kill-{attempt}")
        target = rng.randrange(0, 3)  # which batch of this worker's slice to kill inside
        begins, killed_in = 0, None
        while True:
            event = worker.next_event()
            if event is None:
                break
            if event.get("event") != "batch_begin":
                continue
            if pending_kill is not None:  # the previous kill's batch restarts first <=> it never committed
                pending_kill["mid_batch"] = event["batch_id"] == pending_kill["batch_id"]
                kills.append(pending_kill)
                pending_kill = None
            if begins == target:
                delay = rng.uniform(0.1, 0.8) * typical
                time.sleep(delay)
                worker.kill()
                killed_in = {"attempt": attempt, "batch_id": event["batch_id"], "delay_s": round(delay, 3),
                             "pid": worker.process.pid}
                killed_pids.append(worker.process.pid)
                break
            begins += 1
        worker.finish()
        if killed_in is None:
            break  # the slice ran out of batches before the target
        pending_kill = killed_in
    outcome, events = _run_to_end(killed_db, "batches", logs, "resume-batches")
    if pending_kill is not None:
        first = next(e for e in events if e.get("event") == "batch_begin")
        pending_kill["mid_batch"] = first["batch_id"] == pending_kill["batch_id"]
        kills.append(pending_kill)
    print(json.dumps({"typical_batch_s": typical, "kills": kills}), flush=True)
    assert sum(1 for k in kills if k["mid_batch"]) >= KILLS, kills
    assert outcome["remaining"] == 0 and outcome["stopped"] == "complete"
    final, _ = _run_to_end(killed_db, "finalize", logs, "resume-finalize")
    assert final["finalized"] and final["receipt"]["duplicate_keys"] == 0
    for db in (clean_db, killed_db):
        assert not Path(str(db) + ".wal").exists()

    con = _connect(":memory:", tmp_path)
    try:
        for alias, db in (("c", clean_db), ("k", killed_db), ("s", seed_db)):
            con.execute(f"ATTACH '{db.as_posix()}' AS {alias} (READ_ONLY)")
        both_ways = [
            _count(con, f"SELECT count(*) FROM (SELECT * FROM {a}.equity_daily_bars "
                        f"EXCEPT ALL SELECT * FROM {b}.equity_daily_bars)") for a, b in (("c", "k"), ("k", "c"))]
        ledger = "security_id, run_start, run_end, unit_basis, multiplier, evidence"
        ledger_both_ways = [
            _count(con, f"SELECT count(*) FROM (SELECT {ledger} FROM {a}.equity_bar_unit_corrections "
                        f"EXCEPT ALL SELECT {ledger} FROM {b}.equity_bar_unit_corrections)")
            for a, b in (("c", "k"), ("k", "c"))]
        assert both_ways == [0, 0] and ledger_both_ways == [0, 0]
        assert _count(con, "SELECT count(*) FROM k.equity_daily_bars") == sum(counts.values())
        assert _count(con, """SELECT count(*) FROM (SELECT 1 FROM k.equity_daily_bars
                              GROUP BY security_id, trade_date, source HAVING count(*) > 1)""") == 0
        # Every value vs the seed: x1000 (thousands run), NULL (suspect lines), unchanged (kept, other source).
        suspects = sorted(str(row[0]) for row in con.execute(
            "SELECT DISTINCT security_id FROM s.equity_daily_bars WHERE run_id = ? AND shares_outstanding > ?",
            [LOAD_RUN, THOUSANDS_ROW_CEILING]).fetchall())
        assert {f"TH3-{v}" for v in SUSPECT_VENDOR_IDS} <= set(suspects), suspects
        wrong = _count(con, """
            SELECT count(*) FROM s.equity_daily_bars o
            JOIN k.equity_daily_bars n USING (security_id, trade_date, source)
            WHERE CASE
                WHEN o.run_id = ? AND list_contains(?::VARCHAR[], o.security_id)
                    THEN n.shares_outstanding IS NOT NULL OR n.market_cap_usd IS NOT NULL
                WHEN o.run_id = ? AND o.source = ?
                    THEN n.shares_outstanding IS DISTINCT FROM o.shares_outstanding * 1000
                         OR n.market_cap_usd IS DISTINCT FROM o.market_cap_usd * 1000.0
                ELSE n.shares_outstanding IS DISTINCT FROM o.shares_outstanding
                     OR n.market_cap_usd IS DISTINCT FROM o.market_cap_usd END
               OR n.close IS DISTINCT FROM o.close OR n.adjusted_close IS DISTINCT FROM o.adjusted_close
               OR n.run_id IS DISTINCT FROM o.run_id OR n.source_loaded_at IS DISTINCT FROM o.source_loaded_at""",
                       [LOAD_RUN, suspects, LOAD_RUN, SOURCE])
        assert wrong == 0
        assert _count(con, "SELECT count(*) FROM k.equity_daily_bars WHERE list_contains(?::VARCHAR[], security_id) "
                           "AND shares_outstanding IS NULL", [suspects]) > 0
        assert con.execute("SELECT count(*), count(DISTINCT batch_id), min(batch_id), max(batch_id) "
                           "FROM k.build_batches WHERE run_key = ?", [RUN_KEY]).fetchone() == (16, 16, 0, 15)
        assert con.execute("SELECT status FROM k.build_runs WHERE run_key = ?", [RUN_KEY]).fetchall() == [
            ("published",)]
        bases = dict(con.execute("SELECT unit_basis, count(*) FROM k.equity_bar_unit_corrections GROUP BY 1")
                     .fetchall())
        lines = dict(con.execute("SELECT run_id, count(DISTINCT security_id) FROM s.equity_daily_bars "
                                 "WHERE source = ? AND shares_outstanding IS NOT NULL GROUP BY 1", [SOURCE]).fetchall())
        assert bases == {"thousands": lines[LOAD_RUN] - len(suspects), "shares_unit_suspect": len(suspects),
                         "units": lines[UNITS_RUN]}
        stale_before = _count(con, "SELECT count(*) FROM k.dataset_runs WHERE status = 'running'") + _count(
            con, "SELECT count(*) FROM k.activation_stage_runs WHERE status = 'running'")
        assert stale_before == 2
    finally:
        con.close()

    # recover_stale_run: refused while a live guard receipt names the file, then recovers.
    receipts = tmp_path / "receipts"
    receipts.mkdir()
    live = receipts / "live-writer.json"
    live.write_text(json.dumps({"guard_version": "test", "status": "running", "guard_pid": os.getpid(),
                                "child_pid": os.getpid(), "command": ["python", "writer.py", "--db", str(killed_db)]}),
                    encoding="utf-8")
    recover = [sys.executable, str(RECOVER), "--db", str(killed_db), "--receipts-dir", str(receipts)]
    refused = subprocess.run(recover, capture_output=True, text=True, timeout=600,
                             env={**os.environ, "OPENBLAS_NUM_THREADS": "1"})
    assert refused.returncode == 3 and json.loads(refused.stdout)["status"] == "refused_live_writer"
    live.unlink()
    done = subprocess.run(recover, capture_output=True, text=True, timeout=600,
                          env={**os.environ, "OPENBLAS_NUM_THREADS": "1"})
    report = json.loads(done.stdout)
    assert done.returncode == 0, done.stderr[-3000:]
    assert len(report["recovered"]) == 2 and report["running_rows_after"] == 0
    assert report["verify_schema"] == [] and report["wal_after_close"] is None
    assert not Path(str(killed_db) + ".wal").exists()

    with TH3.open("rb") as stream:
        source_sha = hashlib.file_digest(stream, "sha256").hexdigest()
    evidence = {"status": "passed", "source": str(TH3), "source_sha256": source_sha,
                "source_bytes": TH3.stat().st_size, "seed_rows_by_run": counts,
                "sample_selection": sample,
                "bars_except_all": both_ways, "corrections_except_all": ledger_both_ways,
                "duplicate_keys": 0, "wrong_values": wrong, "kills": kills,
                "correction_bases": bases, "recovery": report, "work": str(tmp_path)}
    (tmp_path / "measurement.json").write_text(json.dumps(evidence, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(evidence), flush=True)

    from atx_db.connection import spill_root

    for pid in killed_pids:  # a killed DuckDB leaves its private spill directory behind
        from atx_db.connection import private_temp_directory
        suffix = private_temp_directory(killed_db).name.split("-", 2)[2]
        for leftover in spill_root().glob(f"conn-{pid}-{suffix}"):
            shutil.rmtree(leftover, ignore_errors=True)
