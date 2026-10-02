"""The adversarial review of lane YINFRA (review-yinfra.md): one test per finding, on wave_fixture's synthetic roots.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_review.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import research_wave  # noqa: E402
import wave_context  # noqa: E402
import wave_stages  # noqa: E402

KEPT_ALL = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 0), "alpha_c": ("reject_redundant", 0)}
STATE = "out/waves/w1"


def wave(root: Path, fake, *args: str) -> tuple[int, list[str]]:
    lines: list[str] = []
    code = research_wave.main([args[0], F.MANIFEST, "--root", str(root), *args[1:]], executor=fake,
                              log=lines.append)
    return code, lines


def receipt(root: Path, name: str, state: str = STATE) -> dict:
    return json.loads((root / state / "receipts" / name).read_text())


def failed(root: Path, name: str, state: str = STATE) -> str:
    return json.loads((root / state / "receipts" / name).read_text())["error"]


# ------------------------------------------------------------------ MAJOR 1: the admission budget is the hand count
X_IDS = {"v8x2": ["bac_vq", "ind_adj_rev_5_nx", "iv_rv_spread_xe", "q5_eg_f49g", "ins_opp_buy"],
         "v8x3": ["stmom", "earn_season", "k8_intensity", "inst_persist", "nt_late", "div_season", "vol_beta",
                  "season_y2_5"],
         "v8x4": ["value_composite_v49", "bm_v49", "ep_v49", "cfp_v49", "fcfp_v49", "ebit_ev_v49", "net_payout_v49",
                  "sp_v49", "rd_me_v49"]}
X_LINES = [("v80", f"v80_{k}") for k in range(7)] + \
          [("v8x2", c) for c in X_IDS["v8x2"]] + [("v8x2b", c) for c in X_IDS["v8x2"][3:]] + \
          [("v8x3", c) for c in X_IDS["v8x3"]] + [("v8x3b", c) for c in X_IDS["v8x3"][2:]] + \
          [("v8x3b-gm", c) for c in X_IDS["v8x3"][2:]] + \
          [("v8x4", c) for c in X_IDS["v8x4"]] + [("v8x4b", c) for c in ("value_composite_v49", "bm_v49",
                                                                          "net_payout_v49")]
X7 = [F.candidate(f"x7_{k}") for k in range(12)]
X_BUDGET = {"id": "v8-x", "admission_cap": 33, "admission_cycle_prefix": "v8x", "construction_cap": 99}


def x_root(path: Path, cap: int = 33) -> Path:
    """The X-7 situation: the ledger as X-2 .. X-4b left it (real line layout: X-2's 5 and X-3's 8 admission trials,
    their b / -gm cells re-ledgering the same trial_ids, X-4's 9 re-screens and X-4b's 3, R-2's v80 lines on another
    prefix), the libraries naming the re-screens, a 12-string wave."""
    root = F.build(path, candidates=X7, budget=dict(X_BUDGET, admission_cap=cap))
    F.write_json(root, "atx-impl/strategies/libraries/v8x4.json", {"members": [], "rescreens": X_IDS["v8x4"]})
    F.write_json(root, "atx-impl/strategies/libraries/v8x4b.json",
                 {"members": [], "rescreens": ["value_composite_v49", "bm_v49", "net_payout_v49"]})
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "x libraries")
    F.BI.ledger_append(root / F.LEDGER, [F.admission_line(cyc, cid, role_sha="e1c67101" * 8) for cyc, cid in X_LINES],
                       chain=True)
    return root


def test_x7_passes_preflight_on_the_real_ledger_format_25_of_33(tmp_path):
    root = x_root(tmp_path / "r")
    lines = F.BI.ledger_read(root / F.LEDGER)
    # the b / -gm cells' lines carry ledgered trial_ids and are skipped: 5 + 8 + 9 re-screens = 22 lines on v8x, the
    # review's "22 used + 12 new = 34 > 33" of a count that keeps the re-screens
    assert sum(1 for r in lines if r.get("kind") == "admission" and r["cycle"].startswith("v8x")) == 22
    assert all(set(r) >= {"schema", "kind", "count", "candidate", "cycle", "status", "origin", "window_id", "window",
                          "pins", "dsl_sha256", "trial_id", "prev_sha256"} for r in lines if r["kind"] == "admission")
    code, out = wave(root, F.FakeCycle(root, {}), "run", "--until", "preflight")
    assert code == 0, out
    b = receipt(root, "01-preflight.json")["outputs"]["budget"]
    assert (b["admission_used"], b["admission_new"], b["admission_cap"]) == (13, 12, 33)      # the hand count: 25
    assert b["admission_new_ids"] == [c["id"] for c in X7]


def test_x7_over_its_cap_is_refused_with_the_hand_numbers(tmp_path):
    root = x_root(tmp_path / "r", cap=24)
    code, _ = wave(root, F.FakeCycle(root, {}), "run", "--until", "preflight")
    assert code == 3 and "13 admission trials used + 12 new > cap 24" in failed(root, "01-preflight.failed-1.json")


def test_new_trials_leave_out_rescreens_other_origins_and_trials_already_ledgered(tmp_path):
    root = F.build(tmp_path / "r", budget={"id": "b", "admission_cap": 10, "admission_cycle_prefix": "w",
                                           "admission_origin": "prior"},
                   candidates=[F.candidate("alpha_a"), F.candidate("alpha_r", kind="replace", replaces=["m1"],
                                                                   rescreen=True),
                               F.candidate("alpha_g", origin="grid")])
    w = wave_context.Wave(root / F.MANIFEST, root)
    assert wave_stages.admission_new(w, F.BI.ledger_read(root / F.LEDGER), w.manifest["budget"]) == ["alpha_a"]
    F.BI.ledger_append(root / F.LEDGER, [F.admission_line("w0", "alpha_a", role_sha=F.role_sha(root)),
                                         F.admission_line("w0", "alpha_q", role_sha=F.role_sha(root), origin="grid")],
                       chain=True)
    records = F.BI.ledger_read(root / F.LEDGER)
    assert wave_stages.admission_new(w, records, w.manifest["budget"]) == []                # alpha_a is ledgered
    assert wave_stages.admission_used(w, records, w.manifest["budget"]) == 1                # the grid line is not prior


def test_a_fresh_state_dir_after_the_screen_counts_no_string_twice(tmp_path):
    root = F.build(tmp_path / "r")
    code, out = wave(root, F.FakeCycle(root, KEPT_ALL), "run", "--until", "screen")
    assert code == 0, out
    w = wave_context.Wave(root / F.MANIFEST, root)
    b, problems = wave_stages.budget_check(w, F.BI.ledger_read(root / F.LEDGER), 2)
    assert problems == [] and (b["admission_used"], b["admission_new"]) == (3, 0)
