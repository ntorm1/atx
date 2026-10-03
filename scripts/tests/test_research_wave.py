"""research_cycle.py wave (platform v8 lane YINFRA): one research wave as a resumable chain of receipted stages.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_wave.py

Every stage that touches data is driven here by wave_fixture.FakeCycle on a synthetic git root (sessions 2020-2021).
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_rules as WR  # noqa: E402
import wave_steps as WS  # noqa: E402
import wave_readers  # noqa: E402

KEPT_ALL = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 0), "alpha_c": ("reject_redundant", 0)}
ONE_DROPPED = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", 1)}


def wave(root: Path, fake, *args: str) -> tuple[int, list[str]]:
    lines: list[str] = []
    code = research_wave.main([args[0], F.MANIFEST, "--root", str(root), *args[1:]], executor=fake,
                              log=lines.append)
    return code, lines


def receipts(root: Path) -> list[str]:
    return sorted(p.name for p in (root / "out/waves/w1/receipts").glob("*.json"))


# ------------------------------------------------------------------ the manifest and the rules
def test_manifest_validation_lists_every_problem(tmp_path):
    m = F.manifest()
    m["fields"]["manifest_sha256"] = "0" * 64
    assert WM.validate(m) == []
    bad = json.loads(json.dumps(m))
    bad["candidates"][0]["dsl_sha256"] = "1" * 64                 # the frozen string's pin
    bad["candidates"][1]["hypothesis"] = "h-alpha_c"              # a second variant of one hypothesis
    bad["acceptance"]["rule"] = "looks-good"
    bad["rule_cell"] = {"template": "t.json", "template_sha256": "2" * 64}
    problems = " | ".join(WM.validate(bad))
    for needle in ("not the SHA-256 of the frozen DSL", "duplicate hypothesis", "acceptance.rule 'looks-good'",
                   "exactly one of candidates"):
        assert needle in problems
    rep = F.candidate("alpha_r", kind="replace")
    assert any("kind replace needs replaces" in p for p in WM.candidate_problems(rep, "c"))
    assert WM.candidate_problems(F.candidate("alpha_r", kind="replace", replaces=["m1"], rescreen=True), "c") == []
    assert any("rescreen" in p for p in WM.candidate_problems(
        F.candidate("alpha_r", kind="replace", replaces=["m1", "m2"], rescreen=True), "c"))


def test_sign_rule_pm7_35_as_ruled():
    cases = [("add", ("admitted", 1), 1, "keep"), ("add", ("admitted", 0), 1, "keep"),
             ("add", ("admitted", -1), 1, "drop"), ("add", ("reject_redundant", -1), 1, "keep"),
             ("add", ("admitted", 1), -1, "drop"), ("replace", ("admitted", 1), 1, "keep"),
             ("replace", ("admitted", 0), 1, "drop"), ("replace", ("reject_redundant", 1), 1, "drop")]
    for kind, (status, sign), prior, want in cases:
        got, _ = WR.sign_pm7_35({"status": status, "runner_sign": sign}, kind, prior)
        assert got == want, (kind, status, sign, prior)
    with pytest.raises(WR.RuleError, match="no admission row"):                    # a stop, never a drop
        WR.sign_pm7_35(None, "add", 1)
    dec = WR.screen_decision("pm7-35", [F.candidate(c) for c in ONE_DROPPED],
                             [{"id": k, "status": s, "runner_sign": g} for k, (s, g) in ONE_DROPPED.items()])
    assert dec["kept"] == ["alpha_a", "alpha_c"] and dec["dropped"] == ["alpha_b"]


def test_gross_match_reproduces_the_logged_corrections():
    """X-3, X-5 and X-6 of the integration log (L, G_parent, G calibration -> L')."""
    assert WR.matched_leverage("1.1474", 0.9862108210, 0.9913686083) == "1.1414"
    assert WR.matched_leverage("1.1414", 0.9861733264, 0.9604183310) == "1.1720"
    assert WR.matched_leverage("1.1720", 0.9862260459, 1.0099371711) == "1.1445"
    assert WR.gross_matches("pm6-6", 0.9859903463, 0.9862108210)                 # X-2: |diff| .00022 stands
    assert not WR.gross_matches("pm6-6", 0.9862108210, 0.9913686083)             # X-3: .00516 corrects
    assert WR.gross_matches("none", 1.0, 0.5)


def test_gm_doc_of_a_plain_spec_and_of_a_template():
    plain = F.cell_spec("v8x3b", "out/mega-nav-loc-L1.1474-v8x3b")
    plain["ref"] = {"output": "out/ref", "combined": "reference_combined"}
    doc = WS.gm_doc(plain, plain, "1.1414", "note.")
    assert doc["name"] == "v8x3b-gm" and doc["nav"]["leverage"] == "1.1414"
    assert doc["nav"]["output"] == "out/mega-nav-loc-L1.1414-v8x3b" and doc["ref"]["leverage"] == "1.1474"
    tpl = {"schema": "atx.research-cycle-template/v1", "name": "x-erc", "parent": "p.json",
           "change": {"set": {"nav.output": "out/nav-x-erc"}}}
    doc = WS.gm_doc(tpl, dict(plain, nav=dict(plain["nav"], output="out/nav-x-erc", leverage="1.1414")), "1.1720", "n.")
    assert doc["change"]["set"] == {"nav.output": "out/nav-x-erc-L1.1720", "nav.leverage": "1.1720"}
    assert WS.gm_path("scripts/specs/v8/lib-w1b.json") == "scripts/specs/v8/lib-w1b-gm.json"


def test_mechanics_rule_and_criteria():
    m = {"mean_gross_leverage_all_rows": 0.986, "mean_net_leverage_all_rows": 0.004, "tau_gmv_mean": 0.024,
         "tau_gmv_p95": 0.03, "accounting": {"max_return_identity_error": 1e-15, "max_cash_book_relative_error": 1e-14,
                                             "tolerance": 1e-9}}
    assert WR.mechanics_check(m)["pass"]
    assert not WR.mechanics_check(dict(m, mean_gross_leverage_all_rows=1.06))["pass"]
    assert not WR.mechanics_check(dict(m, accounting=dict(m["accounting"], tolerance=None)))["pass"]
    rows = WR.criteria_rows(["turnover-per-gross-not-higher", "capacity-4x-higher"],
                            {"tau_gmv_mean": 0.027, "mean_gross_leverage_all_rows": 0.986, "x4_net_sharpe": 1.65},
                            {"tau_gmv_mean": 0.023, "mean_gross_leverage_all_rows": 0.986, "x4_net_sharpe": 1.32})
    assert [r["printed"] for r in rows] == ["unmet", "met"]                         # X-5's capacity criterion
    v = WR.judge("pm7-34", 0.349, True, rows)
    assert v["accepted"] and v["decided_by"] == ["dsr_positive", "mechanics"]
    assert not WR.judge("v8-prereg-5", 0.349, True, rows)["accepted"]
    assert not WR.judge("pm7-34", -0.058, True, rows)["accepted"]                   # X-4


def test_readers_write_keys_only(tmp_path):
    F.write_nav(tmp_path, "nav", 0.98, x4=1.3)
    out = tmp_path / "mech.json"
    assert wave_readers.main(["mechanics", "--nav", f"cell={tmp_path / 'nav'}", "--output", str(out)]) == 0
    row = json.loads(out.read_text())["navs"]["cell"]
    leaked = {"net_sharpe", "gross_sharpe", "ann_mean", "net_annual", "cagr", "max_drawdown", "x4_net_sharpe"}
    assert not leaked & set(row) and "net_return" not in json.dumps(row)
    assert abs(row["mean_gross_leverage_all_rows"] - 0.98) < 0.01 and row["accounting"]["tolerance"] == 1e-9
    with pytest.raises(FileExistsError):                                            # never overwritten
        wave_readers.main(["mechanics", "--nav", f"cell={tmp_path / 'nav'}", "--output", str(out)])
    book = tmp_path / "book.json"
    wave_readers.main(["book", "--nav", f"cell={tmp_path / 'nav'}", "--output", str(book)])
    b = json.loads(book.read_text())["navs"]["cell"]
    assert b["x4_net_sharpe"] == 1.3 and b["net_annual"] == 0.05
    assert b["gross_annual"] == pytest.approx(0.05 + 252 * (0.01 + 0.005 + 0.002) / 299)


def test_add_alpha_save_plan_is_opt_in(tmp_path):
    """--save-plan writes the validated K1 plan of record; without it add-alpha writes exactly the files it wrote
    before (the same set, the same bytes)."""
    import test_research_cycle as T
    trees = {}
    for tag, extra in (("without", []), ("with", ["--save-plan", "plans/ftd_fail.json"])):
        root = T.add_alpha_root(tmp_path / tag)
        s = root / "atx-impl" / "strategies"
        ids = [c["id"] for c in json.loads((s / "fund_industry_ic_v70.json").read_text())["candidates"]]
        plan = T.k1_plan(tmp_path / tag, ids + ["ftd_fail"])
        assert RC.main(T.add_argv(root, "ftd_fail") + ["--plan-json", str(plan)] + extra) == RC.EXIT_OK
        trees[tag] = {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}
        if extra:
            assert json.loads((root / "plans" / "ftd_fail.json").read_text()) == json.loads(plan.read_text())
    saved = trees["with"].pop("plans/ftd_fail.json")
    assert saved and trees["with"] == trees["without"]


# ------------------------------------------------------------------ the wave, end to end on fakes
def test_plan_prints_every_stage_and_runs_nothing(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    code, lines = wave(root, fake, "plan")
    assert code == 0 and fake.calls == [] and not (root / "out/waves/w1").exists()
    text = "\n".join(lines)
    assert sum(1 for x in lines if x.startswith("# stage ")) == 9
    assert text.count(" add-alpha --id ") == 6                                      # register + the b library
    assert "--save-plan out/waves/w1/plans/w1/alpha_a.json" in text
    assert f"{WS.RCY} run scripts/specs/v8/lib-w1.json --screen" in text
    assert '--bundle "<parent NAV>" "<the cell NAV>" --bundle-json out/waves/w1/bundle.json' in text
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root), "--dry-run"], executor=fake,
                              log=lambda s: None) == 0 and fake.calls == []


def test_a_library_wave_with_every_string_kept_runs_to_its_result(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    code, lines = wave(root, fake, "run")
    assert code == 0, lines
    assert receipts(root) == ["01-preflight.json", "02-register.json", "03-screen.json", "04-spec.json", "05-run.json",
                              "06-match.json", "07-verify.json", "08-judge.json", "09-record.json"]
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["cell"]["spec"] == "scripts/specs/v8/lib-w1.json" and res["cell"]["kind"] == "screen-library"
    assert res["cell"]["corrected"] is False and res["verdict"]["accepted"] is True
    assert res["ledger"]["n_before"] == 2 and res["ledger"]["n_after"] == 3
    assert res["ledger"]["admission_lines"] == ["alpha_a", "alpha_b", "alpha_c"]
    assert res["next_parent"] == {"spec": "scripts/specs/v8/lib-w1.json", "library": "w1"}
    assert res["stats"]["cell"]["x4_net_sharpe"] == 1.1 and res["bundle"]["p_one_sided"] == 0.15
    assert [c["printed"] for c in res["verdict"]["criteria"]] == ["met", "unmet"]
    assert (root / "sprint/waves/w1/wave-result.json").read_bytes() == (root / "out/waves/w1/wave-result.json").read_bytes()
    log = (root / "out/waves/w1/wave-log.md").read_text()
    assert "### Wave w1 (library wave): ACCEPTED, N 3" in log and "Mechanics (S2, read before any return): PASS" in log
    adds = [c for c in fake.calls if "add-alpha" in c]
    assert len(adds) == 3 and all(c[c.index("--name") + 1] == "w1" for c in adds)
    runs = [F.unrooted(c[2:]) for c in fake.calls if Path(c[1]).name == "research_cycle.py" and c[2] == "run"]
    assert runs == [["run", "scripts/specs/v8/lib-w1.json", "--screen"],
                    ["run", "scripts/specs/v8/lib-w1.json", "--stop-after", "nav"],
                    ["run", "scripts/specs/v8/lib-w1.json"]]
    assert F.git(root, "log", "--format=%s").splitlines()[0].startswith("wave w1: register library w1")
    assert F.git(root, "status", "--porcelain", "--", "scripts", "atx-impl") == ""
    calls = len(fake.calls)                                                          # a second run: every stage done
    code, lines = wave(root, fake, "run")
    assert code == 0 and len(fake.calls) == calls and all(x.endswith(".json)") for x in lines if x.startswith("== "))


def test_a_dropped_string_makes_the_b_library_and_gross_is_matched(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, ONE_DROPPED, gross_per_l={"w1b": 0.9604 / 1.1474}, dsr=-0.05)
    code, lines = wave(root, fake, "run")
    assert code == 0, lines
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["screen"]["kept"] == ["alpha_a", "alpha_c"] and res["screen"]["dropped"] == ["alpha_b"]
    cell = res["cell"]
    assert cell["kind"] == "b-library" and cell["library"] == "w1b" and cell["corrected"] is True
    assert cell["spec"] == "scripts/specs/v8/lib-w1b-gm.json" and cell["leverage"] != "1.1474"
    assert abs(cell["gross"] - cell["gross_parent"]) <= 0.005 < abs(cell["gross_calibration"] - cell["gross_parent"])
    gm = json.loads((root / cell["spec"]).read_text())
    assert gm["nav"]["output"] == f"out/nav-w1b-L{cell['leverage']}" and gm["name"] == "w1b-gm"
    b_adds = [c for c in fake.calls if "add-alpha" in c and c[c.index("--name") + 1] == "w1b"]
    assert [c[c.index("--id") + 1] for c in b_adds] == ["alpha_a", "alpha_c"]
    assert res["verdict"]["accepted"] is False and res["next_parent"]["spec"] == F.PARENT    # dSR < 0: parent stays
    assert "NOT ACCEPTED" in (root / "out/waves/w1/wave-log.md").read_text()
    subjects = F.git(root, "log", "--format=%s").splitlines()
    assert subjects[0].startswith("wave w1: lib-w1b-gm.json at L") and subjects[1].startswith("wave w1: cell library w1b")


def test_the_gate_stopping_the_wave_records_no_cell(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, {"alpha_a": ("admitted", -1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", -1)},
                       gate_exit=RC.EXIT_GATE)
    code, lines = wave(root, fake, "run")
    assert code == 0, lines
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["cell"] is None and res["ledger"]["n_after"] == 2 and res["verdict"]["accepted"] is False
    assert not any("--stop-after" in c for c in fake.calls)
    assert "NO CELL" in (root / "out/waves/w1/wave-log.md").read_text()


def test_preflight_refusals(tmp_path):
    root = F.build(tmp_path / "a", expect={"n_before": 5})
    code, _ = wave(root, F.FakeCycle(root, KEPT_ALL), "run")
    assert code == 3 and (root / "out/waves/w1/receipts/01-preflight.failed-1.json").is_file()
    assert "planned on N 5" in json.loads((root / "out/waves/w1/receipts/01-preflight.failed-1.json").read_text())["error"]
    root = F.build(tmp_path / "b", budget={"id": "x", "admission_cap": 2, "admission_cycle_prefix": "w"})
    code, _ = wave(root, F.FakeCycle(root, KEPT_ALL), "run")
    err = json.loads((root / "out/waves/w1/receipts/01-preflight.failed-1.json").read_text())["error"]
    assert code == 3 and "0 admission trials used + 3 new > cap 2" in err
    root = F.build(tmp_path / "c")
    (root / F.MANIFEST).write_text((root / F.MANIFEST).read_text().replace("synthetic wave", "edited after commit"))
    code, _ = wave(root, F.FakeCycle(root, KEPT_ALL), "run")
    err = json.loads((root / "out/waves/w1/receipts/01-preflight.failed-1.json").read_text())["error"]
    assert code == 3 and "is not committed as it is" in err and "code pathspec is dirty" in err
    root = F.build(tmp_path / "d")
    F.write_json(root, f"{F.FIELDS}/manifest.json", {"status": "complete", "seal": {"exclusive_end": "2025-01-01"},
                                                     "fields": []})
    code, _ = wave(root, F.FakeCycle(root, KEPT_ALL), "run")
    err = json.loads((root / "out/waves/w1/receipts/01-preflight.failed-1.json").read_text())["error"]
    assert code == 3 and "fields-v1/manifest.json is" in err


def test_resume_after_a_failed_stage_and_refusal_of_a_changed_input(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL, fail={"--stop-after": 4})
    code, _ = wave(root, fake, "run")
    assert code == 4 and receipts(root)[-1] == "05-run.failed-1.json"
    fake.fail = {}
    adds = sum(1 for c in fake.calls if "add-alpha" in c)
    code, lines = wave(root, fake, "run")
    assert code == 0 and sum(1 for c in fake.calls if "add-alpha" in c) == adds         # register not re-run
    assert any(x == "== register: done (02-register.json)" for x in lines)
    fields = root / F.FIELDS / "manifest.json"                                        # a pinned input moves
    fields.write_text(fields.read_text().replace('"a"', '"z"'))
    code, lines = wave(root, fake, "status")
    assert code == 0 and lines[0].startswith("preflight  stale") and "fields_manifest_sha256" in lines[0]
    code, _ = wave(root, fake, "run")
    assert code == 3


def test_mechanics_failure_stops_before_any_return(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL, accounting=1e-6)
    code, _ = wave(root, fake, "run")
    assert code == 4 and receipts(root)[-1] == "07-verify.failed-1.json"
    err = json.loads((root / "out/waves/w1/receipts/07-verify.failed-1.json").read_text())["error"]
    assert "mechanics FAIL (max_return_identity_error)" in err and "no ledger line" in err
    assert not any(len(c) == 4 and c[2] == "run" and c[3].endswith("lib-w1.json") for c in fake.calls)  # no summ run
    assert not (root / "out/waves/w1/readers/book.json").exists()


def test_a_rule_wave_writes_its_cell_on_the_parent(tmp_path):
    root = tmp_path / "r"
    root.mkdir()
    tpl = {"schema": "atx.research-cycle-template/v1", "name": "x-rule", "description": "rule cell",
           "parent": None, "nominal_parent": "lib-p0.json",
           "change": {"set": {"nav.output": "out/nav-x-rule", "fit.output": "out/fit-x-rule"},
                      "flags": {"fit": {"--rule": None}}}}
    F.write_json(root, "scripts/specs/v8/x-rule.json", tpl)
    sha = F.sha((root / "scripts/specs/v8/x-rule.json").read_bytes())
    rule = {"template": "scripts/specs/v8/x-rule.json", "template_sha256": sha, "name": "x-rule-w1",
            "constants": {"flags": {"fit": {"--rule": "erc-v1"}}}}
    F.build(root, drop=("candidates", "sign_rule", "library"), rule_cell=rule)
    fake = F.FakeCycle(root, {})
    code, lines = wave(root, fake, "run")
    assert code == 0, lines
    cell = json.loads((root / "scripts/specs/v8/x-rule-w1.json").read_text())
    assert cell["parent"] == "lib-p0.json" and cell["name"] == "x-rule-w1"
    assert cell["change"]["flags"]["fit"] == {"--rule": "erc-v1"}
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["kind"] == "rule" and res["screen"] is None and res["cell"]["kind"] == "rule"
    assert not any("add-alpha" in c for c in fake.calls)
    assert ["lock", "scripts/specs/v8/x-rule-w1.json", "--write"] in [F.unrooted(c[2:]) for c in fake.calls]
