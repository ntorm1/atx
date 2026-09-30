"""Governance and capital-market event tables (events stage, S6.6 / S6.2) on a synthetic lake."""

from __future__ import annotations

import datetime as dt
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import events_sources as ES

T0 = dt.datetime(2024, 3, 1, 21, 0)


def _write(path: Path, rows: list[dict], schema: pa.Schema | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)


def _filing(cik: int, acc: str, form: str, day: dt.date, items: str | None = None) -> dict:
    at = dt.datetime.combine(day, dt.time(21, 0))
    return {"cik": cik, "accession": acc, "form": form, "filing_date": day, "acceptance_utc": at,
            "acceptance_clock": "file_utc", "vintage_risk": None, "report_date": day, "items": items,
            "is_amendment": form.endswith("/A"), "available_at": at}


def _hit(qid: str, adsh: str, file_type: str, form: str, cik: int, day: dt.date) -> dict:
    return {"qid": qid, "adsh": adsh, "file": "d.htm", "file_type": file_type, "file_description": None,
            "form": form, "root_form": form, "items": [], "ciks": [cik], "file_date": day, "period_ending": None,
            "display_names": [], "sics": [], "window_start": day, "window_end": day}


@pytest.fixture()
def lake(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    d = dt.date
    filings = [
        _filing(1, "0000000001-24-000001", "8-K", d(2024, 3, 1), "5.02,9.01"),
        _filing(1, "0000000001-24-000002", "8-K", d(2024, 5, 1), "5.02"),
        _filing(2, "0000000002-24-000001", "8-K", d(2024, 4, 1), "4.01,9.01"),
        _filing(2, "0000000002-24-000002", "8-K", d(2024, 6, 1), "4.02"),
        _filing(3, "0000000003-24-000001", "10-K", d(2024, 3, 15)),
        _filing(3, "0000000003-23-000001", "10-K", d(2023, 3, 15)),
        # IPO: S-1 then first 424B4, first session two days later; then a follow-on 424B4
        _filing(10, "0000000010-23-000001", "S-1", d(2023, 11, 1)),
        _filing(10, "0000000010-24-000001", "424B4", d(2024, 2, 1)),
        _filing(10, "0000000010-24-000002", "424B4", d(2024, 9, 1)),
        # seasoned issuer: 424B4 is a follow-on even with an S-1 on file; two 424B5 within 5 days = one event
        _filing(11, "0000000011-24-000001", "S-1", d(2024, 1, 5)),
        _filing(11, "0000000011-24-000002", "424B4", d(2024, 2, 1)),
        _filing(11, "0000000011-24-000003", "424B5", d(2024, 5, 1)),
        _filing(11, "0000000011-24-000004", "424B5", d(2024, 5, 2)),
        _filing(11, "0000000011-24-000005", "424B5", d(2024, 8, 1)),
        _filing(11, "0000000011-24-000006", "8-K", d(2024, 7, 1), "8.01,9.01"),
        # spin-off: first Form 10-12B, first session months later
        _filing(12, "0000000012-24-000001", "10-12B", d(2024, 1, 10)),
    ]
    root = tmp_path
    _write(root / "sec_filings" / "filings.parquet", filings)
    items = []
    for f in filings:
        if f["form"].startswith("8-K"):
            for it in f["items"].split(","):
                items.append({k: f[k] for k in ("cik", "accession", "form", "filing_date", "acceptance_utc",
                                                "available_at", "report_date", "is_amendment", "acceptance_clock",
                                                "vintage_risk")} | {"item": it})
    _write(root / "sec_filings" / "eight_k_items.parquet", items)
    _write(root / "sec_filings" / "issuer_profile.parquet",
           [{"cik": c, "sic": "6770" if c == 10 else "3571"} for c in (1, 2, 3, 10, 11, 12)])
    lt = pa.schema([("security_id", pa.int64()), ("cik", pa.int64()), ("valid_from", pa.date32()),
                    ("valid_to", pa.date32()), ("sessions", pa.int64()), ("link_tier", pa.string()),
                    ("is_issuer_primary", pa.bool_()), ("ever_member", pa.bool_())])
    _write(root / "identity" / "link_table.parquet", [
        {"security_id": 100 + c, "cik": c, "valid_from": vf, "valid_to": d(2026, 9, 18), "sessions": 500,
         "link_tier": "strict", "is_issuer_primary": True, "ever_member": c != 3}
        for c, vf in ((1, d(2018, 1, 2)), (2, d(2018, 1, 2)), (3, d(2018, 1, 2)), (10, d(2024, 2, 5)),
                      (11, d(2018, 1, 2)), (12, d(2024, 7, 1)))], lt)
    _write(root / "security_master" / "lines.parquet", [
        {"security_id": 100 + c, "first_session": fs, "left_censored": lc}
        for c, fs, lc in ((1, d(2018, 1, 2), True), (2, d(2018, 1, 2), True), (3, d(2018, 1, 2), True),
                          (10, d(2024, 2, 5), False), (11, d(2018, 1, 2), True), (12, d(2024, 7, 1), False))])
    hits = root / "_tmp" / "events"
    _write(hits / "fts_ceo_depart_0.parquet",
           [_hit("ceo_depart_0", "0000000001-24-000001", "EX-99.1", "8-K", 1, d(2024, 3, 1)),
            # an EX-10 agreement is not evidence
            _hit("ceo_depart_0", "0000000001-24-000002", "EX-10.1", "8-K", 1, d(2024, 5, 1))], ES.HIT_SCHEMA)
    _write(hits / "fts_going_concern.parquet",
           [_hit("going_concern", "0000000003-24-000001", "10-K", "10-K", 3, d(2024, 3, 15))], ES.HIT_SCHEMA)
    _write(hits / "fts_convert_0.parquet",
           [_hit("convert_0", "0000000011-24-000006", "EX-99.1", "8-K", 11, d(2024, 7, 1))], ES.HIT_SCHEMA)
    _write(hits / "fts_spinoff.parquet",
           [_hit("spinoff", "0000000012-24-000001", "EX-99.1", "10-12B", 12, d(2024, 1, 10))], ES.HIT_SCHEMA)
    return root


def test_governance_events(lake: Path) -> None:
    from atx_db.alpha_panel import events_governance as G

    G.build()
    rows = pq.read_table(lake / "events" / "governance.parquet").to_pylist()
    got = {(r["accession"], r["event_type"]): r for r in rows}
    assert set(got) == {("0000000001-24-000001", "officer_director_change"),
                        ("0000000001-24-000002", "officer_director_change"),
                        ("0000000002-24-000001", "auditor_change"), ("0000000002-24-000002", "non_reliance"),
                        ("0000000003-24-000001", "going_concern")}
    first = got[("0000000001-24-000001", "officer_director_change")]
    assert first["ceo_departure"] is True and first["cfo_departure"] is False
    assert first["departure_basis"] == "fts_phrase" and first["security_id"] == 101
    assert first["link_basis"] == "on_date" and first["is_member_issuer"] is True
    assert got[("0000000001-24-000002", "officer_director_change")]["ceo_departure"] is False
    gc = got[("0000000003-24-000001", "going_concern")]
    assert gc["available_at"] == dt.datetime(2024, 3, 15, 21, 0) and gc["is_member_issuer"] is False
    assert got[("0000000002-24-000001", "auditor_change")]["ceo_departure"] is None


def test_capital_events(lake: Path) -> None:
    from atx_db.alpha_panel import events_capital as K

    K.build()
    rows = pq.read_table(lake / "events" / "capital.parquet").to_pylist()
    got = sorted((r["cik"], r["event_type"], r["accession"], r["n_filings"]) for r in rows)
    assert got == [
        (10, "follow_on", "0000000010-24-000002", 1),
        (10, "ipo", "0000000010-24-000001", 1),
        (11, "convert_pricing", "0000000011-24-000006", 1),
        (11, "follow_on", "0000000011-24-000002", 1),
        (11, "shelf_takedown", "0000000011-24-000003", 2),
        (11, "shelf_takedown", "0000000011-24-000005", 1),
        (12, "spin_off", "0000000012-24-000001", 1),
    ]
    by = {(r["cik"], r["event_type"], r["accession"]): r for r in rows}
    ipo = by[(10, "ipo", "0000000010-24-000001")]
    assert ipo["first_session"] == dt.date(2024, 2, 5) and ipo["listed"] and ipo["is_spac"]
    assert ipo["security_id"] == 110 and ipo["link_basis"] == "nearest_30d"
    take = by[(11, "shelf_takedown", "0000000011-24-000003")]
    assert take["last_accession"] == "0000000011-24-000004"
    assert take["available_at"] == dt.datetime(2024, 5, 1, 21, 0)
    assert by[(12, "spin_off", "0000000012-24-000001")]["first_session"] == dt.date(2024, 7, 1)
