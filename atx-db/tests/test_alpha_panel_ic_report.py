"""Validation is reported at choices frozen solely from selection evidence."""

import copy
import json

from atx_db.alpha_panel import ic_report as R


def evidence():
    return {"label_cutoff": "2023-12-29", "validation_protocol": {"selection_window": "SELECTION"},
            "features": {"characteristics:x": {"ic": {
                "h5": {"member_equity": {"SELECTION": {"ic_mean": .04, "nw_t": 4.},
                                          "VALIDATION": {"ic_mean": -.01, "nw_t": -1., "sessions": 300}}},
                "h21": {"member_equity": {"SELECTION": {"ic_mean": .02, "nw_t": 3.},
                                           "VALIDATION": {"ic_mean": .4, "nw_t": 20., "sessions": 300}}}}}}}


def test_validation_cannot_choose_horizon_or_variant():
    e = evidence()
    assert R.frozen_ranking(e)[0]["horizon"] == 5
    changed = copy.deepcopy(e)
    changed["features"]["characteristics:x"]["ic"]["h21"]["member_equity"]["VALIDATION"]["nw_t"] = 1000
    assert R.frozen_ranking(e) == R.frozen_ranking(changed)


def test_report_keeps_negative_validation_and_evidence_hash(tmp_path):
    source = tmp_path / "eval.json"
    source.write_text(json.dumps(evidence()))
    out = tmp_path / "report.md"
    result = R.write_report(evidence(), source, out)
    assert result["ranked_features"][0]["validation_ic"] == -.01
    assert len(result["evaluation_sha256"]) == 64
    assert "return outcomes remain sealed" in out.read_text()
    assert "Holm" in out.read_text()
    assert out.with_suffix(".json").exists()
