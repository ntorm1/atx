"""Kill-and-resume of the CF-R extract stage (tier-1 v2 node 1.4, a ledgered stage).

A crash in either write window of a batch -- while its Parquet ``.tmp`` is being written, or after
the Parquet rename but before its manifest row -- must leave no duplicate member, resume only the
incomplete batches, and reproduce an uninterrupted run byte for byte.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import zipfile
from pathlib import Path

import pyarrow.parquet as pq
import pytest

from atx_db import companyfacts_stage as cf

CONCEPTS = ("Assets", "Revenues", "EntityCommonStockSharesOutstanding")


def _fact(val: object, end: str | None = "2020-12-31", filed: str | None = "2021-02-01", **extra: object) -> dict:
    return {"val": val, "end": end, "filed": filed, "fy": 2020, "fp": "FY", "form": "10-K",
            "accn": "0000000001-21-000001", **extra}


def _loaded(cik: int, rows: int) -> dict:
    return {"cik": cik, "facts": {
        "us-gaap": {"Assets": {"label": "Assets", "description": "Total assets.",
                               "units": {"USD": [_fact(1000 + i, end=f"2020-0{1 + i % 9}-28") for i in range(rows)]}},
                    "Unlisted": {"label": "x", "units": {"USD": [_fact(1)]}}},
        "ifrs-full": {"Assets": {"units": {"EUR": [_fact(5)]}}},
    }}


MEMBERS: dict[str, object] = {
    # 7 fact rows kept: None val -> NULL, int -> DOUBLE; missing filed and end > filed are dropped.
    "CIK0000000001.json": {"cik": 1, "facts": {
        "us-gaap": {"Assets": {"label": "Assets", "description": None, "units": {"USD": [
            _fact(10), _fact(11.5, start="2020-01-01"), _fact(None), _fact(12, filed=None),
            _fact(13, end="2021-03-01"), _fact(2**60, fy=None, frame="CY2020")]}},
            "Revenues": {"label": "Revenues", "units": {"USD": [_fact(7, end="2020-06-30")]}}},
        "dei": {"EntityCommonStockSharesOutstanding": {"units": {"shares": [_fact(3), _fact(4, end="2021-01-15")]}}},
    }},
    "CIK0000000002.json": b"{}",
    "CIK0000000003.json": {"cik": "3", "facts": {}},
    "CIK0000000004.json": {"facts": {"us-gaap": {"Unlisted": {"units": {"USD": [_fact(1)]}}}}},
    "CIK0000000005.json": {"cik": 5, "facts": {"us-gaap": {"Assets": {"units": {"USD": [_fact(1, end="2022-01-01")]}}}}},
    "CIK0000000006.json": {"cik": 7, "facts": {"us-gaap": {}}},
    "CIK0000000008.json": _loaded(8, 5),
    "CIK0000000009.json": _loaded(9, 4),
    "CIK0000000010.json": _loaded(10, 6),
    "notes/readme.txt": b"ignored",
}
EXPECTED = {
    "CIK0000000001.json": ("loaded", None, 7), "CIK0000000002.json": ("unavailable", "empty_archive_placeholder", 0),
    "CIK0000000003.json": ("empty", "unsupported_or_empty_taxonomy", 0),
    "CIK0000000004.json": ("empty", "allowlist_empty", 0), "CIK0000000005.json": ("empty", "no_valid_fact_rows", 0),
    "CIK0000000006.json": ("error", "ValueError: payload CIK does not match archive member CIK", 0),
    "CIK0000000008.json": ("loaded", None, 5), "CIK0000000009.json": ("loaded", None, 4),
    "CIK0000000010.json": ("loaded", None, 6),
}


@pytest.fixture()
def archive(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setattr(cf, "ROW_GROUP_ROWS", 4)  # several row groups per batch
    path = tmp_path / "companyfacts.zip"
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as handle:
        for name, body in MEMBERS.items():
            handle.writestr(name, body if isinstance(body, bytes) else json.dumps(body))
    return path


def _stage(zip_path: Path, root: Path) -> Path:
    plan = cf.write_companyfacts_plan(zip_path, root, concepts=CONCEPTS, concepts_source="test",
                                      target_uncompressed_bytes=700)
    return plan.parent


def _digests(out_dir: Path) -> dict[str, str]:
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(out_dir.iterdir()) if path.suffix in (".parquet", ".json")}


def test_kill_and_resume_reproduces_uninterrupted_run(archive: Path, tmp_path: Path,
                                                      monkeypatch: pytest.MonkeyPatch) -> None:
    clean = _stage(archive, tmp_path / "clean")
    _plan, batches = cf.load_companyfacts_plan(clean)
    assert len(batches) >= 4 and [m for b in batches for m in b.members] == sorted(EXPECTED)
    for batch in batches:
        cf.extract_companyfacts_batch(archive, batch, clean)
    manifest = cf.assemble_companyfacts_stage(archive, clean)

    killed = _stage(archive, tmp_path / "killed")
    cf.extract_companyfacts_batch(archive, batches[0], killed)
    first = cf.batch_paths(killed, 0)[0].stat().st_mtime_ns

    # Window 1: killed at the batch's second member, while its Parquet .tmp is open.
    extract_member = cf._extract_member
    calls: list[str] = []

    def die(*args: object) -> object:
        calls.append("member")
        if len(calls) == 2:
            raise KeyboardInterrupt("killed mid-batch")
        return extract_member(*args)  # type: ignore[arg-type]

    with monkeypatch.context() as patch:
        patch.setattr(cf, "_extract_member", die)
        with pytest.raises(KeyboardInterrupt):
            cf.extract_companyfacts_batch(archive, batches[1], killed)
    parquet_1, row_1 = cf.batch_paths(killed, 1)
    assert not parquet_1.exists() and not row_1.exists()
    assert parquet_1.with_name(parquet_1.name + ".tmp").exists()

    # Window 2: killed after the Parquet rename, before the batch's manifest row.
    def die_json(path: Path, value: object) -> None:
        raise KeyboardInterrupt("killed before the manifest row")

    with monkeypatch.context() as patch:
        patch.setattr(cf, "_write_json_atomic", die_json)
        with pytest.raises(KeyboardInterrupt):
            cf.extract_companyfacts_batch(archive, batches[2], killed)
    assert cf.batch_paths(killed, 2)[0].exists() and not cf.batch_paths(killed, 2)[1].exists()
    with pytest.raises(ValueError, match="incomplete"):
        cf.assemble_companyfacts_stage(archive, killed)

    pending = cf.pending_companyfacts_batches(killed)
    assert [b.batch_id for b in pending] == [b.batch_id for b in batches[1:]]
    for batch in pending:  # the resumed run
        cf.extract_companyfacts_batch(archive, batch, killed)
    assert cf.pending_companyfacts_batches(killed) == []
    assert cf.batch_paths(killed, 0)[0].stat().st_mtime_ns == first  # the complete batch was skipped
    assert cf.assemble_companyfacts_stage(archive, killed) == manifest
    assert _digests(killed) == _digests(clean)
    assert not list(killed.glob("*.tmp"))

    members = pq.read_table(killed / cf.MEMBERS_FILE).to_pylist()
    assert [m["member"] for m in members] == sorted(EXPECTED)  # each member exactly once, CIK order
    assert {m["member"]: (m["disposition"], m["reason"], m["rows"]) for m in members} == EXPECTED
    assert manifest["totals"]["rows"] == 22 and manifest["archive"]["ignored_members"] == ["notes/readme.txt"]
    facts = [row for path in sorted(killed.glob("batch-*.parquet")) for row in pq.read_table(path).to_pylist()]
    assert len(facts) == 22
    first_member = [f for f in facts if f["cik"] == "0000000001"]
    assert [f["value"] for f in first_member] == [10.0, 11.5, None, float(2**60), 7.0, 3.0, 4.0]
    assert {f["available_at"] for f in first_member} == {dt.datetime(2021, 2, 1, 22)}
    assert first_member[0]["security_id"] == "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001"
    assert first_member[0]["source_url"].endswith("companyfacts.zip#CIK0000000001.json")
    assert first_member[1]["period_start"] == dt.date(2020, 1, 1) and first_member[3]["fiscal_year"] is None
