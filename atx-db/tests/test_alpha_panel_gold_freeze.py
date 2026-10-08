"""A synthetic frozen gold lake exercises authorized OOS labels and IC end to end."""

import datetime as dt

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import gold as G
from atx_db.alpha_panel import gold_freeze as F
from atx_db.alpha_panel import holdout_eval as O
from atx_db.alpha_panel import ic_eval as E
from atx_db.alpha_panel import labels_holdout as L


def parquet(root, stage, rows):
    path = root / stage / "year=2024" / "data.parquet"
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows), path)


@pytest.fixture()
def frozen_lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setattr(E, "MIN_NAMES", 10)
    first = dt.date(2024, 1, 2)
    calendar = [first + dt.timedelta(days=i) for i in range(140)]
    calendar = [d for d in calendar if d.weekday() < 5][:95]
    pq.write_table(pa.table({"session_date": calendar}), tmp_path / "calendar.parquet")
    panel, prices, char, gold = [], [], [], []
    x = np.linspace(-1, 1, 30)
    y = np.sin(np.arange(30) / 3)
    sx, sy = G.standardize(x), G.standardize(y)
    composite = G.standardize((sx + sy) / 2)
    for i, date in enumerate(calendar):
        for j in range(30):
            base = {"session_date": date, "security_id": j + 1}
            panel.append({**base, "member_equity": True, "grp_ff49": j % 3, "cik": j + 1,
                          "link_tier": "strict", "is_issuer_primary": True, "is_common": True,
                          "raw_close": 30., "ret_guarded": False,
                          "ret": .005 * x[j] + .003 * np.sin(i * 7 + j * 11)})
            prices.append({**base, "sid0_repaired": False})
            char.append({**base, "beta_252": float(np.cos(j)), "vol_63": float(.1 + j / 1000),
                         "log_adv63": float(12. + np.sin(j))})
            gold.append({**base, "cik": j + 1, "grp_ff49": j % 3, "grp_ff12": j % 2,
                         "g_x": float(sx[j]), "g_y": float(sy[j]), "c_momentum": float(composite[j])})
    for stage, rows in (("panel_goldready", panel), ("prices", prices), ("characteristics", char), ("gold", gold)):
        parquet(tmp_path, stage, rows)
    for stage in ("panel_goldready", "characteristics", "labels_goldready"):
        C.write_stage_manifest(stage, "fixture", ("common",), {})
    # Test the legacy canonical prices receipt adapter, without mutating the receipt.
    C.write_json_atomic(tmp_path / "prices" / "manifest.json", {"stage": "prices", "years": {"2024": len(prices)}})
    selected = [{"name": name, "family": "momentum", "fill_zero": False, "horizon": 21, "variant": "raw"}
                for name in ("x", "y")]
    selection = {"selected": selected, "controls": [], "label_cutoff": "2023-12-29",
                 "rules": {"validation_used_for_selection": False}}
    evidence = tmp_path / "validation" / "gold_pipeline" / "characteristics_strict_ic.json"
    C.write_json_atomic(evidence, {"schema": E.SCHEMA, "label_cutoff": "2023-12-29"})
    evidence_sha = C.sha256_file(evidence)
    selection["ic_sha256"] = evidence_sha
    recipe = G.frozen_recipe(selection)
    recipe["ic_sha256"] = evidence_sha
    C.write_json_atomic(tmp_path / "gold" / "selection.json", selection)
    C.write_json_atomic(tmp_path / "gold" / "frozen_recipe.json", recipe)
    sources = {stage: C.sha256_file(tmp_path / stage / "manifest.json")
               for stage in ("panel_goldready", "characteristics", "labels_goldready")}
    C.write_stage_manifest("gold", G.SCHEMA, ("gold", "common"), {
        "frozen_recipe_sha256": C.sha256_file(tmp_path / "gold" / "frozen_recipe.json"),
        "selection_sha256": C.sha256_file(tmp_path / "gold" / "selection.json"), "input_manifests_sha256": sources,
        "ic_sha256": evidence_sha})
    return tmp_path, calendar


def test_unauthorized_oos_rejected_before_any_receipt_read(tmp_path):
    with pytest.raises(ValueError, match="explicit"):
        F.verify(tmp_path / "does-not-exist.json")
    with pytest.raises(ValueError, match="explicit"):
        O.run(tmp_path / "does-not-exist.json", tmp_path / "out.json")
    assert not (tmp_path / "out.json").exists()


def test_freeze_detects_corruption_and_recipe_reinterpretation(frozen_lake):
    root, _calendar = frozen_lake
    path = root / "freeze.json"
    receipt = F.create(path)
    assert receipt["features"] == ["g_x", "g_y", "c_momentum"]
    altered = {**receipt, "features": ["g_y"]}
    C.write_json_atomic(path, altered)
    with pytest.raises(ValueError, match="choices differ"):
        F.verify(path, authorized_oos=True)
    C.write_json_atomic(path, receipt)
    artifact = root / "gold" / "year=2024" / "data.parquet"
    with artifact.open("ab") as fh:
        fh.write(b"changed")
    with pytest.raises(ValueError, match="artifact changed"):
        F.verify(path, authorized_oos=True)


def test_frozen_oos_end_to_end_never_rewrites_recipe_or_training_cutoff(frozen_lake):
    root, calendar = frozen_lake
    freeze = root / "freeze.json"
    F.create(freeze)
    before = {name: C.sha256_file(root / "gold" / name) for name in ("selection.json", "frozen_recipe.json", "manifest.json")}
    old_cutoff, old_horizons = E.LABEL_CUTOFF, E.HORIZONS
    L.build(freeze, authorized_oos=True, memory="200MB", threads=1)
    out, report = root / "oos.json", root / "oos.md"
    result = O.run(freeze, out, authorized_oos=True, memory="200MB", report=report)
    assert result["label_cutoff"] == str(calendar[-1])
    assert result["frozen_columns"] == ["g_x", "g_y", "c_momentum"]
    assert result["horizons"] == [5, 21, 63]
    stats = result["features"]["gold:g_x"]["ic"]["h5"]["member_equity"]
    assert stats["FROZEN_OOS"]["sessions"] > 60
    assert stats["FROZEN_OOS"]["ic_mean"] > .5
    assert "2024" in stats["years"] and "SELECTION" not in stats
    assert old_cutoff == E.LABEL_CUTOFF and old_horizons == E.HORIZONS
    assert before == {name: C.sha256_file(root / "gold" / name) for name in before}
    assert "never change admission" in report.read_text()


def test_freeze_rejects_undeclared_or_omitted_physical_source_files(frozen_lake):
    root, _calendar = frozen_lake
    path = root / "freeze.json"
    receipt = F.create(path)
    omitted = dict(receipt["artifact_sha256"])
    omitted.pop("gold/year=2024/data.parquet")
    C.write_json_atomic(path, {**receipt, "artifact_sha256": omitted})
    with pytest.raises(ValueError, match="declared physical"):
        F.verify(path, authorized_oos=True)
    C.write_json_atomic(path, receipt)
    pq.write_table(pa.table({"session_date": [dt.date(2024, 1, 2)], "security_id": [999]}),
                   root / "gold" / "year=2024" / "unmanifested.parquet")
    with pytest.raises(ValueError, match="unmanifested"):
        F.verify(path, authorized_oos=True)


def test_oos_rejects_label_corruption_before_computing_ic(frozen_lake):
    root, _calendar = frozen_lake
    freeze = root / "freeze.json"
    F.create(freeze)
    L.build(freeze, authorized_oos=True, memory="200MB", threads=1)
    labels = root / "labels_holdout" / "year=2024" / "labels.parquet"
    with labels.open("ab") as fh:
        fh.write(b"changed")
    with pytest.raises(ValueError, match="label physical"):
        O.run(freeze, root / "oos.json", authorized_oos=True)
    assert not (root / "oos.json").exists()
