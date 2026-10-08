"""Gold admission, same-session scoring and consumer alignment invariants."""
import copy
import datetime as dt
import json

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import gold as G
from atx_db.alpha_panel.char_registry import Feature
from atx_db.alpha_panel import common as C


def feature(name="x", family="momentum", **kwargs):
    return Feature(name, family, name, 1, "fixture", ("panel.ret",), 1, **kwargs)


def report(names=("x",)):
    stat = {"ic_mean": .02, "nw_t": 3., "ind_ic_mean": .03, "ind_nw_t": 4.}
    return {"label_cutoff": "2023-12-29", "windows": {"SELECTION": ["2020-01-02", "2021-12-30"]},
            "validation_protocol": {"selection_decision_ends": {f"h{h}": "2021-09-01" for h in G.HORIZONS},
                                    "purge_sessions": {f"h{h}": h + 1 for h in G.HORIZONS}},
            "pairwise_redundancy": {"window": "SELECTION", "pairs": {}},
            "features": {f"characteristics:{n}": {"coverage": {str(y): .5 for y in range(2019, 2027)},
                "coverage_sessions": {str(y): 250 for y in range(2019, 2027)},
                "ic": {"h21": {"member_equity": {"SELECTION": stat.copy(), "DISCOVERY": stat.copy(),
                                                    "VALIDATION": {"ic_mean": -.5}}}}} for n in names}}


def test_selection_never_reads_validation_and_freezes_variant():
    r = report()
    first = G.select(r, [feature()])
    r["features"]["characteristics:x"]["ic"]["h21"]["member_equity"]["VALIDATION"] = {"ic_mean": 1, "nw_t": 999}
    assert first == G.select(r, [feature()])
    assert first["selected"][0]["variant"] == "industry"


@pytest.mark.parametrize("field,value", [("ic_mean", 0), ("nw_t", 2.499), ("ic_mean", None)])
def test_selection_signal_boundaries(field, value):
    r = report()
    e = r["features"]["characteristics:x"]["ic"]["h21"]["member_equity"]
    e["SELECTION"][field] = value
    e["SELECTION"]["ind_ic_mean"] = None
    assert not G.select(r, [feature()])["selected"]


def test_coverage_discovery_and_missing_pair_fail_closed():
    r = report(("x", "y"))
    assert len(G.select(r, [feature("x"), feature("y")])["selected"]) == 1
    r["pairwise_redundancy"]["pairs"]["characteristics:x|characteristics:y"] = .899
    assert len(G.select(r, [feature("x"), feature("y")])["selected"]) == 2
    r["pairwise_redundancy"]["pairs"]["characteristics:x|characteristics:y"] = .9
    assert len(G.select(r, [feature("x"), feature("y")])["selected"]) == 1
    r = report()
    r["features"]["characteristics:x"]["coverage"]["2026"] = .299
    assert G.select(r, [feature()])["selected"]
    r["features"]["characteristics:x"]["coverage"]["2021"] = .299
    assert not G.select(r, [feature()])["selected"]
    r = report()
    r["features"]["characteristics:x"]["ic"]["h21"]["member_equity"]["DISCOVERY"] = {}
    assert not G.select(r, [feature()])["selected"]


def test_selection_seal_and_validation_redundancy_guard():
    r = report()
    r["pairwise_redundancy"]["window"] = "TRAIN"
    with pytest.raises(ValueError, match="SELECTION"):
        G.select(r)
    r = report()
    r["windows"]["SELECTION"][1] = "2022-01-01"
    with pytest.raises(ValueError, match="validation"):
        G.select(r)


def test_normalization_ties_zero_fill_and_small_groups():
    x = np.array([1., 1., 2., 3., np.nan, 0., 4.])
    groups = np.array(["a", "a", "a", "b", "b", "c", "c"], dtype=object)
    score = G.normalize(x, groups, fill_zero=True)
    assert score[0] == score[1] and score[4] == score[5]
    assert abs(score.mean()) < 1e-12 and score.std() == pytest.approx(1)
    assert np.isnan(G.normalize(x, groups)[4])
    ind = G.normalize(x, groups, industry=True)
    # Group a has 3 members and is demeaned; b/c fewer than3 retain global relative information.
    assert np.isfinite(ind[[0, 1, 2, 3, 5, 6]]).all()
    assert np.isnan(G.normalize(np.ones(4), groups[:4])).all()


def test_composite_threshold_and_nonmember_exclusion():
    cols = [np.array([1., np.nan, 2., 5.]), np.array([2., 1., np.nan, 8.])]
    assert np.isnan(G.composite(cols, 2)[1:3]).all()
    table = pa.table({"session_date": [dt.date(2020, 1, 2)] * 4, "security_id": [1, 2, 3, 4],
                      "cik": [1, 2, 3, 4], "grp_ff49": [1, 1, 1, 1], "grp_ff12": [1, 1, 1, 1],
                      "member_equity": [True, False, True, True], "x": [1., 1000., 2., 3.]})
    selection = {"selected": [{"name": "x", "family": "momentum", "fill_zero": False, "variant": "raw"}],
                 "controls": []}
    scored = G.score_session(table, selection)
    assert scored["security_id"].to_pylist() == [1, 3, 4]
    assert scored["g_x"].type == pa.float32()
    assert np.mean(scored["g_x"].to_numpy()) == pytest.approx(0, abs=1e-7)


def test_run_binds_strict_evidence_and_publishes_atomic_member_rows(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setenv("ATX_ALPHA_PANEL_STAGE", "panel_goldready")
    selection = G.select(report(), [feature()])
    monkeypatch.setattr(G, "select", lambda _: selection)
    r = report()
    r.update(label_stage="labels_goldready", label_policy="strict")
    r["input_manifests_sha256"] = {}
    for stage in ("characteristics", "panel_goldready", "labels_goldready"):
        folder = tmp_path / stage
        folder.mkdir()
        C.write_json_atomic(folder / "manifest.json", {"status": "complete", "stage": stage})
        r["input_manifests_sha256"][f"{stage}/manifest.json"] = C.sha256_file(folder / "manifest.json")
    day = dt.date(2020, 1, 2)
    data = {"session_date": [day] * 4, "security_id": [1, 2, 3, 4], "grp_ff49": [1] * 4,
            "grp_ff12": [1] * 4, "member_equity": [True, False, True, True], "x": [1., 999., 2., 3.]}
    for stage in ("characteristics", "panel_goldready"):
        folder = tmp_path / stage / "year=2020"
        folder.mkdir()
        rows = pa.table(data if stage == "characteristics" else {"session_date": [day] * 4,
                                                               "security_id": [1, 2, 3, 4], "cik": [11, 22, 33, 44]})
        pq.write_table(rows, folder / "p.parquet")
    C.write_json_atomic(tmp_path / "ic.json", r)
    assert G.run(tmp_path / "ic.json", [2020], "100MB")["2020"]["rows"] == 3
    values = pq.read_table(tmp_path / "gold" / "year=2020" / "gold.parquet")
    assert values["security_id"].to_pylist() == [1, 3, 4]
    manifest = C.read_json(tmp_path / "gold" / "manifest.json")
    assert manifest["label_policy"] == "strict" and not manifest["predictive_power_certified"]
    assert not list((tmp_path / "gold").rglob("*.partial"))
    r["input_manifests_sha256"]["characteristics/manifest.json"] = "stale"
    C.write_json_atomic(tmp_path / "ic.json", r)
    with pytest.raises(ValueError, match="not bound"):
        G.run(tmp_path / "ic.json", [2020])


def test_frozen_recipe_predeclares_columns_horizons_and_equal_weights():
    selection = {"controls": ["beta_252"], "selected": [
        {"name": f"x{k}", "family": f"family{k//2}", "horizon": G.HORIZONS[k//2],
         "variant": "raw", "fill_zero": False} for k in range(6)]}
    recipe = G.frozen_recipe(selection)
    assert recipe["evaluation_columns"] == ["g_x0", "g_x1", "g_x2", "g_x3", "g_x4", "g_x5",
                                             "c_family0", "c_family1", "c_family2", "c_all"]
    assert recipe["features"]["g_x4"]["selected_horizon"] == 63
    assert recipe["controls"] == ["ctl_beta_252"]
    assert all(sum(c["weights"].values()) == pytest.approx(1) for c in recipe["composites"].values())
    assert recipe["composites"]["c_all"]["minimum_present"] == 3
    assert not recipe["out_of_sample_used_for_selection"]
