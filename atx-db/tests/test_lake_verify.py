"""``lake verify`` on fixture lakes: one test per failure kind (FAIL / STALE / WARN) plus the clean baseline."""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

from atx_db.stagelake.contract import Output, Stage, validate
from atx_db.stagelake.coverage import coverage_report
from atx_db.stagelake.testing import bind, publish, write_table
from atx_db.stagelake.verify import StageResult, verify

NOW = dt.datetime(2026, 9, 29, 12, tzinfo=dt.UTC)
TS = [dt.datetime(2024, 1, 2, 21), dt.datetime(2024, 1, 3, 21)]


def _stages() -> list[Stage]:
    return validate([
        Stage("alpha", "PLAT", "atx.test/v1", None, doc="fixture", outputs=(Output("alpha/year=*/data.parquet"),)),
        Stage("beta", "PLAT", "atx.test/v1", None, doc="fixture", inputs=("alpha",),
              outputs=(Output("beta/events.parquet"),
                       Output("beta/lookup.parquet", view="beta_lookup", clock=None))),
        Stage("gamma", "PLAT", "atx.test/v1", None, doc="fixture", planned=True,
              outputs=(Output("gamma/g.parquet"),)),
    ])


def _lake(tmp_path: Path) -> Path:
    root = tmp_path / "lake"
    write_table(root, "alpha/year=2024/data.parquet", {"security_id": [1, 2], "available_at": TS})
    publish(root, "alpha/manifest.json")
    write_table(root, "beta/events.parquet", {"cik": [10, 11], "available_at": TS})
    write_table(root, "beta/lookup.parquet", {"cusip": ["A", "B"], "security_id": [1, 2]})
    publish(root, "beta/manifest.json", inputs=bind(root, "alpha/manifest.json"))
    return root


def _run(root: Path, name: str, **kw) -> StageResult:
    return verify(root, _stages(), [name], NOW, **kw)[0]


def _codes(res: StageResult, kind: str) -> set[str]:
    return {f.code for f in res.findings if f.kind == kind}


def test_clean_lake_is_ok_and_planned_stage_is_reported(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    res = verify(root, _stages(), None, NOW)
    assert {r.stage: r.status for r in res} == {"alpha": "OK", "beta": "OK", "gamma": "PLANNED"}
    assert res[1].binding_basis == "input_manifests_sha256" and res[1].files_checked == 2
    assert coverage_report(root, _stages()) == ""


def test_missing_manifest_fails(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    (root / "alpha" / "manifest.json").unlink()
    assert "missing_manifest" in _codes(_run(root, "alpha"), "FAIL")
    # its consumer cannot prove freshness either
    assert "input_missing" in _codes(_run(root, "beta"), "STALE")


def test_incomplete_manifest_fails(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    m = json.loads((root / "alpha" / "manifest.json").read_text())
    del m["code"]
    m["status"] = "running"
    (root / "alpha" / "manifest.json").write_text(json.dumps(m))
    res = _run(root, "alpha")
    assert res.status == "FAIL"
    details = [f.detail for f in res.findings if f.code == "incomplete_manifest"]
    assert "no code" in details and "status 'running'" in details


def test_schema_mismatch_fails(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    publish(root, "alpha/manifest.json", schema="atx.test/v2")
    assert "schema_mismatch" in _codes(_run(root, "alpha"), "FAIL")


def test_sha_mismatch_and_size_mismatch_fail(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    p = root / "alpha" / "year=2024" / "data.parquet"
    blob = bytearray(p.read_bytes())
    blob[len(blob) // 2] ^= 0xFF  # same size, different bytes
    p.write_bytes(bytes(blob))
    assert "sha_mismatch" in _codes(_run(root, "alpha"), "FAIL")
    assert "sha_mismatch" not in _codes(_run(root, "alpha", hash_files=False), "FAIL")  # --no-hash: sizes only
    with p.open("ab") as fh:
        fh.write(b"x")
    assert "size_mismatch" in _codes(_run(root, "alpha"), "FAIL")


def test_file_missing_output_missing_and_unbound_file_fail(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    write_table(root, "alpha/year=2025/data.parquet", {"security_id": [3], "available_at": TS[:1]})
    assert "unbound_file" in _codes(_run(root, "alpha"), "FAIL")
    (root / "alpha" / "year=2025" / "data.parquet").unlink()
    (root / "alpha" / "year=2024" / "data.parquet").unlink()
    assert {"file_missing", "output_missing"} <= _codes(_run(root, "alpha"), "FAIL")


def test_missing_clock_column_fails_and_static_output_needs_none(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    write_table(root, "beta/events.parquet", {"cik": [10, 11], "filed": TS})
    publish(root, "beta/manifest.json", inputs=bind(root, "alpha/manifest.json"))
    res = _run(root, "beta")
    assert _codes(res, "FAIL") == {"clock_missing"}
    assert all("lookup" not in f.detail for f in res.findings)  # clock None: not checked


def test_future_clock_fails(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    assert _run(root, "alpha").status == "OK"
    early = dt.datetime(2024, 1, 3, tzinfo=dt.UTC)
    assert "clock_future" in _codes(verify(root, _stages(), ["alpha"], early)[0], "FAIL")


def test_timestamptz_clock_is_compared_in_utc(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    tz = [t.replace(tzinfo=dt.UTC) for t in TS]
    write_table(root, "alpha/year=2024/data.parquet", {"security_id": [1, 2], "available_at": tz})
    publish(root, "alpha/manifest.json")
    assert verify(root, _stages(), ["alpha"], dt.datetime(2024, 1, 3, 21, 30, tzinfo=dt.UTC))[0].status == "OK"
    assert verify(root, _stages(), ["alpha"], dt.datetime(2024, 1, 3, 20, 30, tzinfo=dt.UTC))[0].status == "FAIL"


def test_partial_file_fails(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    (root / "alpha" / "year=2024" / "data.parquet.partial").write_bytes(b"half")
    assert "partial_file" in _codes(_run(root, "alpha"), "FAIL")
    (root / "alpha" / "_work").mkdir()
    (root / "alpha" / "year=2024" / "data.parquet.partial").unlink()
    (root / "alpha" / "_work" / "x.parquet.partial").write_bytes(b"scratch")  # stage scratch dir: ignored
    assert _run(root, "alpha").status == "OK"


def test_changed_input_manifest_is_stale(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    publish(root, "alpha/manifest.json", extra={"rebuilt": True})  # alpha rebuilt after beta
    res = _run(root, "beta")
    assert res.status == "STALE" and _codes(res, "STALE") == {"input_changed"}
    assert _run(root, "alpha").status == "OK"


def test_unbound_input_warns_and_legacy_binding_is_read(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    publish(root, "beta/manifest.json")  # no input_manifests_sha256
    res = _run(root, "beta")
    assert res.status == "WARN" and _codes(res, "WARN") == {"input_unbound"}
    sha = bind(root, "alpha/manifest.json")["alpha/manifest.json"]
    publish(root, "beta/manifest.json", extra={"sources": {"alpha_manifest": {"manifest.json": {"sha256": sha}}}})
    res = _run(root, "beta")
    assert res.status == "OK" and res.binding_basis == "legacy"


def test_legacy_clock_column_warns(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    stages = validate([Stage("alpha", "PLAT", "atx.test/v1", None, doc="fixture",
                             outputs=(Output("alpha/year=*/data.parquet", clock="clock_utc"),))])
    write_table(root, "alpha/year=2024/data.parquet", {"security_id": [1, 2], "clock_utc": TS})
    publish(root, "alpha/manifest.json")
    res = verify(root, stages, ["alpha"], NOW)[0]
    assert res.status == "WARN" and _codes(res, "WARN") == {"legacy_clock"}


def test_coverage_report_names_unregistered_manifest_and_files(tmp_path: Path) -> None:
    root = _lake(tmp_path)
    write_table(root, "delta/year=2024/d.parquet", {"security_id": [1], "available_at": TS[:1]})
    publish(root, "delta/manifest.json", schema="atx.test.delta/v1", extra={"stage": "delta"})
    write_table(root, "beta/new_output.parquet", {"x": [1]})
    text = coverage_report(root, _stages())
    assert "unregistered manifest delta/manifest.json" in text
    assert '"schema": "atx.test.delta/v1"' in text and '"glob": "delta/year=*/d.parquet"' in text
    assert '"clock": "available_at"' in text
    assert "not covered by any output glob of stage 'beta'" in text and "beta/new_output.parquet" in text
