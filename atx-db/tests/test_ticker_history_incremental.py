"""P14: incremental TickerHistory3 date partitions.

One fixture, three daily partitions of three vendor lines (the vendor's
``cumulReturnFactor`` is 1.0 on each partition's own date, as on the real feed):

* the second partition appends only its own date; the first date's rows are untouched;
* re-applying a partition changes nothing;
* an ex-dividend on one line in the third partition (``returnFactor`` 0.98)
  moves that line's stored adjusted history to the new basis (x 0.98) and
  records one ledger row (``equity_adjustment_rebases``) at the third
  partition's receipt; raw fields and every bar's clock stay as first
  published; a point-in-time read before that receipt still returns the
  pre-rebase adjusted history; the other lines are untouched;
* the rebase is refused, with nothing written, until the ledger is allowed
  (migration 0328 formalizes it).
"""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db.ticker_history_incremental import (
    REBASES_TABLE,
    REVISIONS_TABLE,
    PartitionIngestOptions,
    RevisionsTableNotMigratedError,
    bars_asof_sql,
    ingest_ticker_history_partition,
)

_DAYS = (dt.date(2026, 9, 21), dt.date(2026, 9, 22), dt.date(2026, 9, 23))
_RECEIPTS = tuple(dt.datetime.combine(day + dt.timedelta(days=1), dt.time(10)) for day in _DAYS)
_CLOSES = {101: (10.0, 10.5, 11.0), 102: (20.0, 20.2, 20.4), 103: (30.0, 31.0, 30.2)}


def _write_partition(path, index, *, ex_dividend=None):
    con = duckdb.connect()
    con.execute(
        "CREATE TABLE p (tradingDate DATE, securityID BIGINT, ticker_tk VARCHAR, todayTicker VARCHAR, dn BIGINT, "
        "open DOUBLE, high DOUBLE, low DOUBLE, close DOUBLE, closePr DOUBLE, closeUnadjPr DOUBLE, volume BIGINT, "
        "shares BIGINT, returnFactor DOUBLE, totalReturn DOUBLE, cumulReturnFactor DOUBLE)"
    )
    rows = []
    for vendor_id, closes in _CLOSES.items():
        close = closes[index]
        rf = 0.98 if vendor_id == ex_dividend else 1.0
        symbol = f"S{vendor_id}"
        rows.append((_DAYS[index], vendor_id, symbol, symbol, index + 1, close, close, close, close, close, close,
                     1_000, 50_000, rf, 0.0, 1.0))
    con.executemany("INSERT INTO p VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
    con.execute(f"COPY p TO '{path.as_posix()}' (FORMAT parquet)")
    con.close()
    return path


def _ingest(store, path, index, **overrides):
    return ingest_ticker_history_partition(store, PartitionIngestOptions(
        partition_path=path, received_at=_RECEIPTS[index], minimum_partition_lines=1, **overrides))


def _bars(store):
    return store.con.execute(
        "SELECT security_id, trade_date, close, adjusted_close, available_at, run_id, shares_outstanding "
        "FROM equity_daily_bars ORDER BY security_id, trade_date"
    ).fetchall()


def test_partitions_append_idempotently_and_a_factor_rebase_revises_only_its_line(tmp_store, tmp_path):
    p1 = _write_partition(tmp_path / "p1.parquet", 0)
    p2 = _write_partition(tmp_path / "p2.parquet", 1)
    p3 = _write_partition(tmp_path / "p3.parquet", 2, ex_dividend=103)

    first = _ingest(tmp_store, p1, 0)
    assert (first.status, first.appended_rows, first.new_lines) == ("applied", 3, 3)
    after_first = _bars(tmp_store)
    # A8 units: parquet shares are thousands (format evidence + median), stored in shares.
    assert {row[6] for row in after_first} == {50_000_000}

    second = _ingest(tmp_store, p2, 1)
    assert (second.appended_rows, second.restated_rows, second.rebased_lines) == (3, 0, 0)
    after_second = _bars(tmp_store)
    assert [row for row in after_second if row[1] == _DAYS[0]] == after_first  # only the new date was appended
    assert {row[4] for row in after_second if row[1] == _DAYS[1]} == {_RECEIPTS[1]}  # receipt clock

    again = _ingest(tmp_store, p2, 1)
    assert (again.status, again.changed_rows) == ("unchanged", 0) and _bars(tmp_store) == after_second

    ledger_sql = "SELECT (SELECT count(*) FROM data_quality_checks), (SELECT count(*) FROM dataset_runs)"
    ledger = tmp_store.con.execute(ledger_sql).fetchone()
    with pytest.raises(RevisionsTableNotMigratedError):
        _ingest(tmp_store, p3, 2)
    assert _bars(tmp_store) == after_second and tmp_store.con.execute(ledger_sql).fetchone() == ledger

    def tables():
        return {row[0] for row in tmp_store.con.execute(
            "SELECT table_name FROM duckdb_tables() WHERE table_name IN (?, ?)",
            [REBASES_TABLE, REVISIONS_TABLE]).fetchall()}

    assert tables() == set()

    third = _ingest(tmp_store, p3, 2, allow_unmigrated_revisions_table=True)
    assert (third.appended_rows, third.rebased_lines, third.rebased_rows, third.restated_rows) == (3, 1, 2, 0)
    rebased = "TBLTICKERHISTORY-103"
    by_key = {(row[0], row[1]): row for row in _bars(tmp_store)}
    for row in after_second:
        now = by_key[(row[0], row[1])]
        if row[0] == rebased:  # adjusted history x 0.98; raw close, clock, run and shares as first published
            assert now[3] == pytest.approx(row[3] * 0.98) and now[:3] + now[4:] == row[:3] + row[4:]
        else:
            assert now == row
    # A factor-only revision is one ledger row per rebase, never a copy of history.
    assert tables() == {REBASES_TABLE}
    ledger_rows = tmp_store.con.execute(
        f"SELECT security_id, partition_date, available_at, multiplier, first_affected_date, last_affected_date "
        f"FROM {REBASES_TABLE}"
    ).fetchall()
    assert ledger_rows == [(rebased, _DAYS[2], _RECEIPTS[2], pytest.approx(0.98), _DAYS[0], _DAYS[1])]
    assert not tmp_store.con.execute(
        "SELECT count(*) FROM (SELECT source, security_id, trade_date FROM equity_daily_bars GROUP BY ALL "
        "HAVING count(*) > 1)").fetchone()[0]

    # Point in time: before the third receipt the pre-rebase history is what was known.
    sql = bars_asof_sql(tmp_store)
    for cutoff, scale in ((_RECEIPTS[1] + dt.timedelta(hours=2), 1.0), (_RECEIPTS[2] + dt.timedelta(hours=2), 0.98)):
        known = {(row[1], row[4]): row[9] for row in tmp_store.con.execute(sql, [cutoff]).fetchall()}
        assert [known[(rebased, day)] for day in _DAYS[:2]] == pytest.approx([30.0 * scale, 31.0 * scale])
        assert known[("TBLTICKERHISTORY-101", _DAYS[1])] == 10.5
        assert ((rebased, _DAYS[2]) in known) == (cutoff > _RECEIPTS[2])
