"""RI2: the ``identity_reconstruction`` activation stage writes RI1 evidence into the warehouse.

One compact end-to-end check on a scratch 0327 warehouse (the schema template at
migration head) and tiny retained files: the stage writes the accepted link with
its point-in-time tier history and both sides of a two-CIK conflict, the bridge's
evidence reader sees them, a rerun on the same files changes nothing, a
missing input or one received after the cutoff day fails before anything is
written, and other inputs append a new revision that supersedes the held one
through the governed swap (and the older inputs restore it the same way).
"""

from __future__ import annotations

import calendar
import datetime as dt
import json
import os
import zipfile
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db import historical_identity as hi
from atx_db import identity_reconstruction as ir
from atx_db.activation import IDENTITY_RECONSTRUCTION_SCRATCH, ActivationOptions, stage_identity_reconstruction
from atx_db.market_owner_bridge import _read_reconstructed_evidence
from atx_db.symbol_directory import SnapshotAfterCutoffError

D = dt.date
RECEIVED = dt.datetime(2026, 9, 19, 12, 0)
OLD_CIK, TWIN_A, TWIN_B = "0000000501", "0000000601", "0000000602"


def _weekdays(first: dt.date, last: dt.date) -> list[dt.date]:
    return [first + dt.timedelta(days=n) for n in range((last - first).days + 1)
            if (first + dt.timedelta(days=n)).weekday() < 5]


def _quarters(first: dt.date, count: int) -> list[dt.date]:
    return [D(first.year + (first.month - 1 + 3 * i) // 12, (first.month - 1 + 3 * i) % 12 + 1, first.day)
            for i in range(count)]


def _facts(base: int) -> list[tuple[dt.date, int]]:
    """Thirteen quarterly cover counts that change every quarter (as of, shares)."""
    return [(as_of, base + i * 111_111) for i, as_of in enumerate(_quarters(D(2014, 1, 20), 13))]


def _bars(vendor_id: int, symbol: str, first: dt.date, last: dt.date, facts: list[tuple[dt.date, int]]):
    """Daily bars whose vendor count (thousands) switches on each cover ``as of`` date."""
    for day in _weekdays(first, last):
        known = [shares for as_of, shares in facts if as_of <= day]
        yield (vendor_id, day, 10.0, symbol, symbol, known[-1] // 1000 if known else None)


def _retained_files(root: Path) -> tuple[Path, Path]:
    cache, staging = root / "cache", root / "staging"
    cache.mkdir()
    staging.mkdir()
    old, twin = _facts(50_123_456), _facts(30_000_001)
    rows = [
        *_bars(901, "OLDCO", D(2014, 1, 2), D(2017, 6, 30), old),  # delisted; no current ticker
        *_bars(903, "TWIN", D(2014, 1, 2), D(2017, 6, 30), twin),  # two co-registrants report its counts
        *_bars(902, "LIVE", D(2012, 3, 26), D(2020, 12, 31), []),  # sets the price file's span
    ]
    columns = ("securityID", "tradingDate", "close", "ticker_tk", "todayTicker", "shares")
    parquet = staging / "TickerHistory3.parquet"
    pq.write_table(pa.table({name: [row[i] for row in rows] for i, name in enumerate(columns)}), parquet)

    def companyfacts(facts: list[tuple[dt.date, int]], tag: str) -> str:
        return json.dumps({"facts": {"dei": {"EntityCommonStockSharesOutstanding": {"units": {"shares": [
            {"end": as_of.isoformat(), "val": shares, "accn": f"{tag}-{as_of:%Y%m%d}", "form": "10-Q",
             "filed": (as_of + dt.timedelta(days=10)).isoformat()}
            for as_of, shares in facts
        ]}}}}})

    with zipfile.ZipFile(cache / "companyfacts.zip", "w") as archive:
        archive.writestr(f"CIK{OLD_CIK}.json", companyfacts(old, "old"))
        archive.writestr(f"CIK{TWIN_A}.json", companyfacts(twin, "twa"))
        archive.writestr(f"CIK{TWIN_B}.json", companyfacts(twin, "twb"))
    # A Form 25 four days before the last trade: raises the tier from its own clock, never the link's.
    notice = {"form": ["25-NSE"], "filingDate": ["2017-06-26"], "accessionNumber": ["0000000501-17-000001"],
              "acceptanceDateTime": ["2017-06-26T16:05:00.000Z"]}
    with zipfile.ZipFile(cache / "submissions.zip", "w") as archive:
        archive.writestr(f"CIK{OLD_CIK}.json", json.dumps({"cik": "501", "filings": {"recent": notice, "files": []}}))
    (cache / "company_tickers.json").write_text(
        json.dumps({"0": {"cik_str": 999, "ticker": "LIVE", "title": "Live Co"}}), encoding="utf-8")
    stamp = calendar.timegm(RECEIVED.timetuple())
    for path in (parquet, cache / "companyfacts.zip", cache / "submissions.zip", cache / "company_tickers.json"):
        os.utime(path, (stamp, stamp))
    return parquet, root


def _held(store) -> list[tuple[object, ...]]:
    return store.con.execute(
        "SELECT native_key, cik, evidence_status, rejection_reason, available_at, is_latest_revision, value_json "
        "FROM security_identity_evidence ORDER BY native_key, cik"
    ).fetchall()


def test_stage_writes_ri1_evidence_idempotently_and_refuses_late_or_missing_inputs(tmp_store, tmp_path):
    parquet, root = _retained_files(tmp_path)
    options = ActivationOptions(
        as_of_date=D(2026, 9, 20), ticker_history_source_path=parquet, cache_dir=root / "cache",
        staging_dir=root / "staging", run_id="ri2-test",
    )
    first = stage_identity_reconstruction(tmp_store, options)

    assert first.rows == 3
    assert first.detail["evidence_rows_by_status"] == {"conflicting": 2, "reconstructed": 1}
    assert (first.detail["links_by_tier.high"], first.detail["unresolved_conflict_lines"]) == (1, 1)
    assert first.detail["network_requests"] == 0
    assert first.detail["inputs"]["companyfacts"]["receipt_basis"] == "file_mtime"
    held = _held(tmp_store)
    link, side_a, side_b = held
    payload = json.loads(link[6])
    # The accepted link keeps RI1's evidence clock (third distinct count, filed + 46h) and its PIT tier history.
    assert link[:6] == ("901", OLD_CIK, "reconstructed", None, dt.datetime(2014, 7, 31, 22, 0), True)
    assert (payload["tier_at_available_at"], payload["tier"], [tier for tier, _ in payload["tier_history"]]) == (
        ir.TIER_MEDIUM, ir.TIER_HIGH, [ir.TIER_MEDIUM, ir.TIER_HIGH])
    assert payload["tier_attained_at"] > link[4].isoformat()
    # Both sides of the unresolved conflict are kept, never linkable.
    assert [row[:5] for row in (side_a, side_b)] == [
        ("903", TWIN_A, "conflicting", ir.REJECT_CONFLICTING, None),
        ("903", TWIN_B, "conflicting", ir.REJECT_CONFLICTING, None),
    ]
    assert set(hi.audit_identity_evidence(tmp_store.con).values()) == {0}
    # The owner bridge (rule 5) reads exactly these rows from the warehouse.
    evidence = sorted((item.evidence_status, item.cik, len(item.tier_history)) for item in
                      _read_reconstructed_evidence(tmp_store))
    assert evidence == [("conflicting", TWIN_A, 0), ("conflicting", TWIN_B, 0), ("reconstructed", OLD_CIK, 2)]
    assert not (root / "staging" / IDENTITY_RECONSTRUCTION_SCRATCH / "ri2_stage.duckdb").exists()

    # Same files: nothing written, superseded or restored.
    again = stage_identity_reconstruction(tmp_store, options)
    assert (again.rows, again.detail["rows_unchanged"], again.detail["rows_superseded"],
            again.detail["rows_restored"]) == (0, 3, 0, 0)
    assert _held(tmp_store) == held

    # Inputs received after the cutoff day, or missing, fail before anything is written.
    with pytest.raises(SnapshotAfterCutoffError):
        stage_identity_reconstruction(tmp_store, ActivationOptions(**{**options.as_dict(), "as_of_date": D(2026, 9, 18)}))
    with pytest.raises(FileNotFoundError):
        stage_identity_reconstruction(tmp_store, ActivationOptions(**{**options.as_dict(),
                                                                      "ticker_history_source_path": None}))
    assert _held(tmp_store) == held

    # Other inputs (a later receipt clock on one file, still before the cutoff) are a new revision: appended,
    # and the held one superseded -- never deleted -- by the governed swap (no in-place UPDATE).
    tickers = root / "cache" / "company_tickers.json"
    later = calendar.timegm((RECEIVED + dt.timedelta(hours=6)).timetuple())
    os.utime(tickers, (later, later))
    second = stage_identity_reconstruction(tmp_store, options)
    assert (second.rows, second.detail["rows_superseded"], second.detail["rows_restored"],
            second.detail["latest_flags_rebuilt_by_swap"]) == (3, 3, 0, True)
    assert _flags(tmp_store) == [(first.detail["revision"], False, 3), (second.detail["revision"], True, 3)]
    assert set(hi.audit_identity_evidence(tmp_store.con).values()) == {0}
    assert {item.evidence_id for item in _read_reconstructed_evidence(tmp_store)} == {
        row[0] for row in tmp_store.con.execute(
            "SELECT evidence_id FROM security_identity_evidence WHERE source_revision_id = ?",
            [second.detail["revision"]]).fetchall()}
    # The older inputs again: nothing appended; their revision is latest again, by the same swap.
    stamp = calendar.timegm(RECEIVED.timetuple())
    os.utime(tickers, (stamp, stamp))
    back = stage_identity_reconstruction(tmp_store, options)
    assert (back.rows, back.detail["rows_unchanged"], back.detail["rows_superseded"], back.detail["rows_restored"],
            back.detail["latest_flags_rebuilt_by_swap"]) == (0, 3, 3, 3, True)
    assert _flags(tmp_store) == [(second.detail["revision"], False, 3), (first.detail["revision"], True, 3)]
    assert [row for row in _held(tmp_store) if row[5]] == held


def _flags(store) -> list[tuple[object, ...]]:
    return store.con.execute(
        "SELECT source_revision_id, is_latest_revision, count(*) FROM security_identity_evidence "
        "GROUP BY ALL ORDER BY is_latest_revision, source_revision_id"
    ).fetchall()
