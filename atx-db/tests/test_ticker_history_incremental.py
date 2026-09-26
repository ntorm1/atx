"""P14: incremental TickerHistory3 date partitions (0.14 fix: stored bars are never rebased).

One fixture, daily partitions of three vendor lines (the vendor's
``cumulReturnFactor`` is 1.0 on each partition's own date, as on the real feed):

* the second partition appends only its own date and continues each line's
  series (``run_id``) and stored factor; re-applying it changes nothing;
* an ex-dividend on one line in the third partition (``returnFactor`` 0.98)
  writes one ledger row and a new bar whose factor continues the line's
  (1 / 0.98); no stored bar changes; the vendor-basis point-in-time read
  applies the multiplier only from the rebase's receipt;
* a re-delivered third partition (a raw close correction, a corrected
  returnFactor) restates those keys through the revisions table, adds a
  correcting ledger row, and the as-of read returns the superseded versions
  before its receipt;
* the ledger / revisions tables are refused, with nothing written, on a
  warehouse below migration 0328;
* a partition load killed before COMMIT leaves nothing and resumes; one killed
  after COMMIT, before its CHECKPOINT, replays from the WAL, reruns as
  ``unchanged`` and the next partition applies (the replayed indexes are sound);
  the result equals an uninterrupted load;
* a skipped vendor session is refused with nothing written, also under an
  override when the partition's previous closes show the session was
  published; an excluded no-trade row keeps its returnFactor (a carried ledger
  step the next bar continues through, visible to the vendor-basis read from
  its receipt); a session the vendor never published is accepted only with a
  recorded reason; a stale late file is refused without blocking newer ones.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import subprocess
import sys
from pathlib import Path

import duckdb
import pytest

from atx_db.ticker_history_incremental import (
    BASIS_STORED,
    REBASES_TABLE,
    REVISIONS_TABLE,
    DailyRefreshRequest,
    PartitionFile,
    PartitionIngestOptions,
    RevisionsTableNotMigratedError,
    SessionGapError,
    bars_asof_sql,
    ingest_ticker_history_partition,
    refresh_daily_prices,
)

_DAYS = tuple(dt.date(2026, 9, day) for day in (21, 22, 23, 24, 25, 28, 29))
_RECEIPTS = tuple(dt.datetime.combine(day + dt.timedelta(days=1), dt.time(10)) for day in _DAYS)
_CLOSES = {101: (10.0, 10.5, 11.0, 11.2, 11.1, 11.4, 11.3), 102: (20.0, 20.2, 20.4, 20.3, 20.6, 20.8, 20.7),
           103: (30.0, 31.0, 30.2, 30.5, 30.9, 31.2, 31.0)}
_LINES = {vendor_id: f"TBLTICKERHISTORY-{vendor_id}" for vendor_id in _CLOSES}
_SRC = str(Path(__file__).resolve().parents[1] / "src")


def _write_partition(path, index, *, rf=None, close=None, dn=None, invalid=(), day=None):
    """One vendor session: ``dn`` defaults to index + 1; ``closeUnadjPr`` is the previous session's close;
    ``invalid`` lines are no-trade quotes (open 0, volume 0) the projection excludes."""
    con = duckdb.connect()
    con.execute(
        "CREATE TABLE p (tradingDate DATE, securityID BIGINT, ticker_tk VARCHAR, todayTicker VARCHAR, dn BIGINT, "
        "open DOUBLE, high DOUBLE, low DOUBLE, close DOUBLE, closePr DOUBLE, closeUnadjPr DOUBLE, volume BIGINT, "
        "shares BIGINT, returnFactor DOUBLE, totalReturn DOUBLE, cumulReturnFactor DOUBLE)"
    )
    rows = []
    for vendor_id, closes in _CLOSES.items():
        price = (close or {}).get(vendor_id, closes[index])
        previous = closes[max(index - 1, 0)]
        symbol = f"S{vendor_id}"
        opened, volume = (0.0, 0) if vendor_id in invalid else (price, 1_000)
        rows.append((day or _DAYS[index], vendor_id, symbol, symbol, dn or index + 1, opened, price, price, price,
                     price, previous, volume, 50_000, (rf or {}).get(vendor_id, 1.0), 0.0, 1.0))
    con.executemany("INSERT INTO p VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
    con.execute(f"COPY p TO '{path.as_posix()}' (FORMAT parquet)")
    con.close()
    return path


def _ingest(store, path, received_at, **overrides):
    return ingest_ticker_history_partition(store, PartitionIngestOptions(
        partition_path=path, received_at=received_at, minimum_partition_lines=1, **overrides))


def _bars(store):
    return store.con.execute(
        "SELECT security_id, trade_date, close, adjusted_close, available_at, run_id, shares_outstanding "
        "FROM equity_daily_bars ORDER BY security_id, trade_date"
    ).fetchall()


def _asof(store, cutoff, **kwargs):
    rows = store.con.execute(bars_asof_sql(store, **kwargs), [cutoff]).fetchall()
    return {(row[1], row[4]): (row[8], row[9]) for row in rows}  # (security_id, date) -> (close, adjusted)


def _wal_bytes(store):
    wal = Path(f"{store.path}.wal")
    return wal.stat().st_size if wal.exists() else 0


def test_partitions_continue_the_stored_factor_and_never_rebase_history(tmp_store, tmp_path):
    parts = [_write_partition(tmp_path / f"p{i + 1}.parquet", i, rf={103: 0.98} if i == 2 else None)
             for i in range(3)]
    first = _ingest(tmp_store, parts[0], _RECEIPTS[0])
    assert (first.status, first.appended_rows, first.new_lines) == ("applied", 3, 3)
    after_first = _bars(tmp_store)
    assert {row[6] for row in after_first} == {50_000_000}  # A8: parquet thousands stored in shares

    second = _ingest(tmp_store, parts[1], _RECEIPTS[1])
    assert (second.appended_rows, second.restated_rows, second.rebased_lines) == (3, 0, 0)
    after_second = _bars(tmp_store)
    assert [row for row in after_second if row[1] == _DAYS[0]] == after_first
    assert {row[4] for row in after_second if row[1] == _DAYS[1]} == {_RECEIPTS[1]}  # receipt clock
    # C4: a new bar continues its line's series, so P8 / R1d read one series per line.
    assert tmp_store.con.execute(
        "SELECT max(n) FROM (SELECT count(DISTINCT run_id) AS n FROM equity_daily_bars GROUP BY security_id)"
    ).fetchone() == (1,)
    again = _ingest(tmp_store, parts[1], _RECEIPTS[1])
    assert (again.status, again.changed_rows) == ("unchanged", 0) and _bars(tmp_store) == after_second

    # Below migration 0328 the ledger is refused and nothing is written.
    tmp_store.con.execute(f"DROP TABLE {REBASES_TABLE}")
    tmp_store.con.execute(f"DROP TABLE {REVISIONS_TABLE}")
    ledger_sql = ("SELECT (SELECT count(*) FROM data_quality_checks), (SELECT count(*) FROM dataset_runs), "
                  "(SELECT count(*) FROM duckdb_tables() WHERE table_name IN (?, ?))")
    ledger = tmp_store.con.execute(ledger_sql, [REBASES_TABLE, REVISIONS_TABLE]).fetchone()
    with pytest.raises(RevisionsTableNotMigratedError):
        _ingest(tmp_store, parts[2], _RECEIPTS[2])
    assert _bars(tmp_store) == after_second
    assert tmp_store.con.execute(ledger_sql, [REBASES_TABLE, REVISIONS_TABLE]).fetchone() == ledger

    third = _ingest(tmp_store, parts[2], _RECEIPTS[2], allow_unmigrated_revisions_table=True)
    assert (third.appended_rows, third.rebased_lines, third.restated_rows) == (3, 1, 0)
    assert _wal_bytes(tmp_store) == 0  # checkpointed after the commit
    after_third = _bars(tmp_store)
    assert [row for row in after_third if row[1] <= _DAYS[1]] == after_second  # history untouched
    by_key = {(row[0], row[1]): row for row in after_third}
    ex_div = _LINES[103]
    new_bar = by_key[(ex_div, _DAYS[2])]
    assert new_bar[3] == pytest.approx(30.2 / 0.98) and new_bar[5] == by_key[(ex_div, _DAYS[0])][5]
    assert tmp_store.con.execute(
        f"SELECT security_id, partition_date, available_at, multiplier, first_affected_date, last_affected_date "
        f"FROM {REBASES_TABLE}").fetchall() == [(ex_div, _DAYS[2], _RECEIPTS[2], pytest.approx(0.98), *_DAYS[:2])]

    # Point in time: the rebase multiplier applies from its receipt only.
    before, after = _RECEIPTS[1] + dt.timedelta(hours=2), _RECEIPTS[2] + dt.timedelta(hours=2)
    known = _asof(tmp_store, before)
    assert [known[(ex_div, day)][1] for day in _DAYS[:2]] == pytest.approx([30.0, 31.0])
    assert (ex_div, _DAYS[2]) not in known
    known = _asof(tmp_store, after)
    assert [known[(ex_div, day)][1] for day in _DAYS[:3]] == pytest.approx([29.4, 30.38, 30.2])
    assert known[(_LINES[101], _DAYS[1])][1] == pytest.approx(10.5)
    stored = _asof(tmp_store, after, basis=BASIS_STORED)
    assert [stored[(ex_div, day)][1] for day in _DAYS[:3]] == pytest.approx([30.0, 31.0, 30.2 / 0.98])

    # A re-delivered third partition: 101's close corrected, 103's returnFactor corrected to 0.97.
    redelivered = _write_partition(tmp_path / "p3b.parquet", 2, rf={103: 0.97}, close={101: 11.1})
    receipt = _RECEIPTS[2] + dt.timedelta(hours=30)
    fourth = _ingest(tmp_store, redelivered, receipt, allow_unmigrated_revisions_table=True)
    assert (fourth.appended_rows, fourth.restated_rows, fourth.rebased_lines) == (0, 2, 1)
    assert _wal_bytes(tmp_store) == 0
    assert tmp_store.con.execute(
        f"SELECT security_id, trade_date, adjusted_close, available_at FROM {REVISIONS_TABLE} ORDER BY ALL"
    ).fetchall() == [(_LINES[101], _DAYS[2], pytest.approx(11.0), _RECEIPTS[2]),
                     (ex_div, _DAYS[2], pytest.approx(30.2 / 0.98), _RECEIPTS[2])]
    restated = {(row[0], row[1]): row for row in _bars(tmp_store)}
    assert restated[(_LINES[101], _DAYS[2])][2:5] == (11.1, pytest.approx(11.1), receipt)
    assert restated[(ex_div, _DAYS[2])][3] == pytest.approx(30.2 / 0.97)
    assert [row for row in _bars(tmp_store) if row[1] <= _DAYS[1]] == after_second
    assert tmp_store.con.execute(
        "SELECT count(*) FROM (SELECT 1 FROM equity_daily_bars GROUP BY source, security_id, trade_date "
        "HAVING count(*) > 1)").fetchone() == (0,)
    known = _asof(tmp_store, after)  # the superseded versions until the re-delivery's receipt
    assert known[(_LINES[101], _DAYS[2])] == pytest.approx((11.0, 11.0))
    assert [known[(ex_div, day)][1] for day in _DAYS[:3]] == pytest.approx([29.4, 30.38, 30.2])
    known = _asof(tmp_store, receipt + dt.timedelta(hours=1))
    assert known[(_LINES[101], _DAYS[2])] == pytest.approx((11.1, 11.1))
    assert [known[(ex_div, day)][1] for day in _DAYS[:3]] == pytest.approx([29.1, 30.07, 30.2])


_KILL = """
import datetime as dt, os, sys
from pathlib import Path
from atx_db import ticker_history_incremental as p14
from atx_db.connection import DuckDBStore
store = DuckDBStore(Path(sys.argv[1]))
store.reopen()
store.con.execute("SET memory_limit = '256MB'")
store._initialized = True
if sys.argv[4] == "before_checkpoint":
    p14._checkpoint = lambda _store: os._exit(9)
else:
    publish = p14._publish_new_lines
    def die(*args, **kwargs):
        publish(*args, **kwargs)
        os._exit(9)
    p14._publish_new_lines = die
p14.ingest_ticker_history_partition(store, p14.PartitionIngestOptions(
    partition_path=Path(sys.argv[2]), received_at=dt.datetime.fromisoformat(sys.argv[3]), minimum_partition_lines=1))
"""


def _killed(store, path, received_at, mode):
    store.close()
    done = subprocess.run(
        [sys.executable, "-c", _KILL, str(store.path), str(path), received_at.isoformat(), mode],
        env={**os.environ, "PYTHONPATH": _SRC, "OPENBLAS_NUM_THREADS": "1"}, capture_output=True, text=True,
        timeout=300,
    )
    assert done.returncode == 9, done.stderr
    wal = _wal_bytes(store)
    store.reopen()  # replays the WAL, if any
    return wal


def _state(store):
    bars = store.con.execute(
        "SELECT security_id, trade_date, close, adjusted_close, available_at FROM equity_daily_bars ORDER BY ALL"
    ).fetchall()
    ledger = store.con.execute(
        f"SELECT security_id, partition_date, available_at, multiplier FROM {REBASES_TABLE} ORDER BY ALL").fetchall()
    series = store.con.execute(
        "SELECT max(n) FROM (SELECT count(DISTINCT run_id) AS n FROM equity_daily_bars GROUP BY security_id)"
    ).fetchone()
    return bars, ledger, series


def test_a_killed_partition_load_resumes_to_the_uninterrupted_result(tmp_store, fresh_store, tmp_path):
    parts = [_write_partition(tmp_path / f"p{i + 1}.parquet", i, rf={103: 0.98} if i == 2 else None)
             for i in range(5)]
    for store in (tmp_store, fresh_store):
        for index in range(2):
            _ingest(store, parts[index], _RECEIPTS[index])
    applied = "SELECT count(*) FROM data_quality_checks WHERE check_name = 'incremental_partition_applied'"
    runs = tmp_store.con.execute(applied).fetchone()

    # Killed inside the transaction: nothing of the partition is left (only the A8 unit check's own
    # quality row, written before the transaction, replays); it resumes.
    _killed(tmp_store, parts[2], _RECEIPTS[2], "before_commit")
    assert tmp_store.con.execute(
        "SELECT count(*) FROM equity_daily_bars WHERE trade_date = ?", [_DAYS[2]]).fetchone() == (0,)
    assert tmp_store.con.execute(f"SELECT count(*) FROM {REBASES_TABLE}").fetchone() == (0,)
    assert tmp_store.con.execute(applied).fetchone() == runs
    assert _ingest(tmp_store, parts[2], _RECEIPTS[2]).status == "applied"

    # Killed after COMMIT, before CHECKPOINT: the partition lives only in the WAL and replays on open.
    assert _killed(tmp_store, parts[3], _RECEIPTS[3], "before_checkpoint") > 0
    assert tmp_store.con.execute(
        "SELECT count(*) FROM equity_daily_bars WHERE trade_date = ?", [_DAYS[3]]).fetchone() == (3,)
    assert _ingest(tmp_store, parts[3], _RECEIPTS[3]).status == "unchanged"
    # The next partition writes every indexed ledger table the replayed transaction wrote.
    assert _ingest(tmp_store, parts[4], _RECEIPTS[4]).status == "applied"
    assert _wal_bytes(tmp_store) == 0

    for index in range(2, 5):
        _ingest(fresh_store, parts[index], _RECEIPTS[index])
    assert _state(tmp_store) == _state(fresh_store)
    assert _state(tmp_store)[2] == (1,) and len(_state(tmp_store)[1]) == 1


def _latest_session(store):
    row = store.con.execute(
        "SELECT watermark_value FROM dataset_watermarks WHERE watermark_name = 'latest_partition:tbltickerhistory3_10y'"
    ).fetchone()
    return json.loads(row[0])


def test_every_session_and_every_excluded_step_stays_in_the_factor_chain(tmp_store, tmp_path):
    for index in range(2):
        _ingest(tmp_store, _write_partition(tmp_path / f"p{index}.parquet", index), _RECEIPTS[index])
    bars = _bars(tmp_store)

    # A skipped session (the 09-23 file never arrived) is refused before any write, also under an override:
    # its previous closes are the 09-23 closes, not the stored 09-22 ones (the vendor did publish 09-23).
    skipped = _write_partition(tmp_path / "p3.parquet", 3)
    with pytest.raises(SessionGapError, match="session 4"):
        _ingest(tmp_store, skipped, _RECEIPTS[3])
    with pytest.raises(SessionGapError, match="override is refused"):
        _ingest(tmp_store, skipped, _RECEIPTS[3], session_gap_reason="assumed holiday")
    assert _bars(tmp_store) == bars and _latest_session(tmp_store)["dn"] == 2

    # 09-23 arrives; line 103's row is a no-trade quote (excluded) carrying a 2 % distribution.
    third = _ingest(tmp_store, _write_partition(tmp_path / "p2.parquet", 2, invalid={103}, rf={103: 0.98}),
                    _RECEIPTS[2])
    ex_div = _LINES[103]
    assert (third.appended_rows, third.rebased_lines) == (2, 1)
    assert (third.detail["excluded_event_rows"], third.detail["excluded_event_rows_carried"]) == (1, 1)
    assert tmp_store.con.execute(
        "SELECT status FROM data_quality_checks WHERE check_name = 'incremental_partition_applied' "
        "ORDER BY checked_at DESC LIMIT 1").fetchone() == ("warning",)
    assert tmp_store.con.execute(
        f"SELECT security_id, partition_date, available_at, multiplier, last_affected_date, detection_basis "
        f"FROM {REBASES_TABLE}").fetchall() == [
        (ex_div, _DAYS[2], _RECEIPTS[2], pytest.approx(0.98), _DAYS[1], "vendor_crf_return_factor_excluded_row")]
    # The vendor's own file of 09-23 restates 103's history by 0.98: the vendor-basis read applies the carried
    # step from its receipt although no 09-23 bar of 103 is stored.
    before, between = _RECEIPTS[1] + dt.timedelta(hours=2), _RECEIPTS[2] + dt.timedelta(hours=2)
    assert [_asof(tmp_store, before)[(ex_div, day)][1] for day in _DAYS[:2]] == pytest.approx([30.0, 31.0])
    assert [_asof(tmp_store, between)[(ex_div, day)][1] for day in _DAYS[:2]] == pytest.approx([29.4, 30.38])

    # The next bar continues through the carried step: 103's factor is 1 / 0.98, so the stored return from
    # 09-22 to 09-24 includes the distribution.
    fourth = _ingest(tmp_store, _write_partition(tmp_path / "p3.parquet", 3), _RECEIPTS[3])
    assert fourth.detail["session"]["basis"] == "dn" and fourth.detail["session"]["dn"] == 4
    stored = {(row[0], row[1]): row for row in _bars(tmp_store)}
    assert stored[(ex_div, _DAYS[3])][3] == pytest.approx(30.5 / 0.98)
    after = _RECEIPTS[3] + dt.timedelta(hours=2)
    known = _asof(tmp_store, after)
    assert [known[(ex_div, day)][1] for day in (_DAYS[0], _DAYS[1], _DAYS[3])] == pytest.approx([29.4, 30.38, 30.5])
    _ingest(tmp_store, _write_partition(tmp_path / "p4.parquet", 4), _RECEIPTS[4])

    # 09-28: the vendor counted a closed session (dn 7 after 5, previous closes are the 09-25 ones).
    closure = _write_partition(tmp_path / "p5.parquet", 5, dn=7)
    with pytest.raises(SessionGapError, match="session 7"):
        _ingest(tmp_store, closure, _RECEIPTS[5])
    closed = _ingest(tmp_store, closure, _RECEIPTS[5], session_gap_reason="vendor session 6: market closed")
    assert closed.detail["session"]["status"] == "gap_overridden"
    assert closed.detail["session"]["reason"] == "vendor session 6: market closed"
    assert _latest_session(tmp_store)["dn"] == 7

    # A late file of the skipped number is stale: refused and recorded, and the next session still applies.
    stale = _write_partition(tmp_path / "late.parquet", 5, dn=6, day=dt.date(2026, 9, 26))
    refresh = refresh_daily_prices(tmp_store, DailyRefreshRequest(
        partitions=(PartitionFile(stale, _RECEIPTS[5]),
                    PartitionFile(_write_partition(tmp_path / "p6.parquet", 6, dn=8), _RECEIPTS[6])),
        minimum_partition_lines=1))
    assert len(refresh.refused) == 1 and "late.parquet" in refresh.refused[0]["path"]
    assert [(result.partition_date, result.status) for result in refresh.partitions] == [(_DAYS[6], "applied")]
    assert _wal_bytes(tmp_store) == 0
