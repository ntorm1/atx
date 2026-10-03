"""The adversarial review of lane YINFRA (review-yinfra.md), MINOR findings 8-18 (MAJOR 1-7: test_wave_review.py).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_review_minor.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
from test_wave_review import KEPT_ALL, ONE_DROPPED, STATE, failed, receipt, result, wave  # noqa: E402
import wave_seal  # noqa: E402
import wave_steps as WS  # noqa: E402


# ------------------------------------------------------------------ MINOR 8, 9: date forms, allow list, guard lines
class Logs:
    def __init__(self, root: Path):
        self.root = root

    def path(self, rel: str) -> Path:
        return self.root / rel


def test_the_scan_reads_compact_year_and_quarter_forms():
    got = {raw: (iso, form) for raw, iso, form in wave_seal.tokens(
        "a 20240315 b 0.20240316 c a20240317 d 20240318.5 e 20231399 f year=2024 g 2024Q2 h 2024-q3 i 2023Q4 "
        "j 2025-02-03")}
    assert got == {"20240315": ("2024-03-15", "compact"), "year=2024": ("2024-01-01", "year"),
                   "2024Q2": ("2024-04-01", "quarter"), "2024-q3": ("2024-07-01", "quarter"),
                   "2023Q4": ("2023-10-01", "quarter"), "2025-02-03": ("2025-02-03", "iso")}


def test_seeds_and_guard_refusals_are_classified_not_hits(tmp_path):
    F.write(tmp_path, "a.log", "nav_summ --protocol v8 seed 20260929 draws 4999\n"
                               ".superpowers/sdd/platform-v8-20260929/x\n"
                               "fit: a session at or after the research seal 2024-01-01 (w1); refusing (sealed)\n")
    F.write(tmp_path, "b.log", "session 2024-03-05 at or after the research seal 2024-01-01; refusing (sealed)\n")
    seal = wave_seal.scan(Logs(tmp_path), ["a.log", "b.log"])
    assert seal["tokens_at_or_after_seal"] == 1 and seal["where"] == [{"file": "b.log", "tokens": 1}]   # 2024-03-05
    assert seal["seal_references"] == {"tokens": 2, "files": ["a.log", "b.log"]}
    assert seal["allowed"] == [{"token": "20260929", "ruling": wave_seal.SEEDS["20260929"], "tokens": 2,
                                "files": ["a.log"]}]
    ruled = wave_seal.scan(Logs(tmp_path), ["b.log"], {"2024-03-05": "PM8-99: a calendar constant"})
    assert ruled["tokens_at_or_after_seal"] == 0 and ruled["rulings"] == {"2024-03-05": "PM8-99: a calendar constant"}


def test_a_seal_allow_ruling_passes_verify_and_is_carried_to_record(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL, screen_log="build tag 20241105\n")
    code, _ = wave(root, fake, "run")
    assert code == 4 and "--seal-allow TOKEN=RULING" in failed(root, "07-verify.failed-1.json")
    code, out = wave(root, fake, "run", "--until", "verify", "--seal-allow", "20241105=PM8-99 build tag")
    assert code == 0, out
    assert receipt(root, "07-verify.json")["outputs"]["seal_scan"]["rulings"] == {"20241105": "PM8-99 build tag"}
    assert wave(root, fake, "run")[0] == 0                                            # record: no flag, ruling kept
    seal = result(root)["seal_scan"]
    assert seal["allowed"][0]["token"] == "20241105" and seal["allowed"][0]["ruling"] == "PM8-99 build tag"
    assert "20241105 x1 allowed: PM8-99 build tag" in (root / STATE / "wave-log.md").read_text()
    assert wave(root, fake, "run", "--seal-allow", "x")[0] == 2


# ------------------------------------------------------------------ MINOR 10: the binding of the attempt that made it
class RetriedNav(F.FakeCycle):
    """The cell's first NAV attempt was killed (its dir holds a receipt and a stale binding); the retry is -run2."""

    def cycle(self, args):
        if "--stop-after" in args and not args[1].endswith("-gm.json"):
            spec, c = self.spec_outputs(args[1])
            first = f"{c.out(spec['nav']['output'])}-run"
            if not (self.root / first).exists():
                F.write_json(self.root, f"{first}/receipt.json", {"outcome": "killed", "exit_code": None,
                                                                  "wall_seconds": 180.0})
                F.write_json(self.root, f"{first}/cycle_binding.json", {"argv_sha256": "a" * 64,
                                                                        "spec_sha256": "b" * 64,
                                                                        "spec_rule": "spec-digest-v1"})
        return super().cycle(args)


def test_verify_reads_the_binding_of_the_last_completed_nav_attempt(tmp_path):
    root = F.build(tmp_path / "r")
    code, out = wave(root, RetriedNav(root, KEPT_ALL), "run")
    assert code == 0, out
    b = receipt(root, "07-verify.json")["outputs"]["binding"]
    assert b["path"].endswith("-run2/cycle_binding.json") and b["spec_sha256"] != "b" * 64


# ------------------------------------------------------------------ MINOR 12: a rule wave resumes after lock --write
def rule_root(path: Path) -> Path:
    path.mkdir(parents=True)
    tpl = {"schema": "atx.research-cycle-template/v1", "name": "x-rule", "description": "rule cell",
           "parent": None, "nominal_parent": "lib-p0.json",
           "change": {"set": {"nav.output": "out/nav-x-rule", "fit.output": "out/fit-x-rule"},
                      "flags": {"fit": {"--rule": None}}}}
    F.write_json(path, "scripts/specs/v8/x-rule.json", tpl)
    rule = {"template": "scripts/specs/v8/x-rule.json",
            "template_sha256": F.sha((path / "scripts/specs/v8/x-rule.json").read_bytes()), "name": "x-rule-w1",
            "constants": {"flags": {"fit": {"--rule": "erc-v1"}}}}
    return F.build(path, drop=("candidates", "sign_rule", "library"), rule_cell=rule)


def test_a_rule_wave_resumes_after_lock_write_rewrote_its_cell(tmp_path):
    root = rule_root(tmp_path / "r")
    fake = F.FakeCycle(root, {}, fail={"commit": 4})                                   # the commit after lock fails
    assert wave(root, fake, "run")[0] == 4
    cell = json.loads((root / "scripts/specs/v8/x-rule-w1.json").read_text())
    assert "locked" in cell                                                            # lock --write pinned it
    fake.fail = {}
    code, out = wave(root, fake, "run")
    assert code == 0, out
    assert F.git(root, "log", "-1", "--format=%s", "--", "scripts/specs/v8/x-rule-w1.json").startswith(
        "wave w1: rule cell x-rule-w1.json")
    assert receipt(root, "04-spec.json")["outputs"]["commit"] == F.git(root, "log", "-1", "--format=%H", "--",
                                                                       "scripts/specs/v8/x-rule-w1.json").strip()
    doc = json.loads((root / "scripts/specs/v8/x-rule-w1.json").read_text())
    edited = json.loads(json.dumps(doc))
    edited["change"]["flags"]["fit"]["--rule"] = "erc-v2"                              # a real edit still differs
    assert WS.unpinned(edited) != WS.unpinned(doc) and WS.unpinned(dict(doc, locked={"x": 1})) == WS.unpinned(doc)


# ------------------------------------------------------------------ MINOR 13: the wave's root reaches every tool
def test_research_cycle_and_add_alpha_get_the_waves_root(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    assert wave(root, fake, "run")[0] == 0
    cycles = [c for c in fake.calls if Path(c[1]).name == "research_cycle.py"]
    assert cycles and all(c[c.index("--root") + 1] == root.resolve().as_posix() for c in cycles)
    assert all(c[1] == (F.research_tree.REPO / WS.RCY).as_posix() for c in cycles)    # not under this root
    F.write(root, WS.RCY, "# the root's own copy\n")                                  # a checkout that has the tool
    assert WS.cycle_argv("py", "run", "s.json", root=root)[1] == WS.RCY
    assert WS.cycle_argv("py", "run", "s.json") == ["py", WS.RCY, "run", "s.json"]    # no root: as before
    readers = WS.reader_argv("py", "book", {"cell": "n"}, "o.json", "r1", root=tmp_path / "elsewhere")
    assert readers[1].endswith("scripts/run_bounded_research.py") and Path(readers[1]).is_absolute()


# ------------------------------------------------------------------ MINOR 14: the series the readers / bundle read
def binds(argv: list[str]) -> list[str]:
    return [argv[i + 1] for i in range(argv.index("--")) if argv[i] == "--bind"]


def test_readers_and_bundle_bind_the_daily_series_and_capacity_curve(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    assert wave(root, fake, "run")[0] == 0
    bounded = [c for c in fake.calls if Path(c[1]).name == "run_bounded_research.py"]
    nav = receipt(root, "05-run.json")["outputs"]["nav"]
    for c in bounded:                                                                  # mechanics x1, bundle, book
        for d in (nav, F.PARENT_NAV):
            assert {f"{d}/summary.json", f"{d}/daily_s2.csv", f"{d}/capacity_curve.csv"} <= set(binds(c))


def test_a_bundle_whose_run_did_not_bind_the_series_now_is_refused(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)                                 # a bundle made by a run that bound no series
    F.write_json(root, f"{STATE}/bundle.json", {"base": F.PARENT_NAV, "final": "out/nav-w1-L1.1474", "paired": {}})
    F.write_json(root, f"{STATE}/bundle-run1/receipt.json", {"outcome": "completed", "exit_code": 0,
                                                              "bindings": []})
    code, _ = wave(root, fake, "run")
    err = failed(root, "08-judge.failed-1.json")
    assert code == 3 and "did not bind the daily series" in err and "daily_s2.csv" in err


# ------------------------------------------------------------------ MINOR 17: the hand log's heading and budget line
def test_the_log_section_has_the_hand_heading_and_the_budget_line(tmp_path):
    root = F.build(tmp_path / "r")
    assert wave(root, F.FakeCycle(root, ONE_DROPPED, gross_per_l={"w1b": 0.9604 / 1.1474}), "run")[0] == 0
    log = (root / STATE / "wave-log.md").read_text()
    assert log.splitlines()[0] == "### Cell w1 (library wave; library w1 screen, then w1b on p0): N 3"
    assert "Budget syn: admission trials 0 + 3 new = 3 of 10 (cycles w*; re-screens left out); construction N 2 -> " \
           "3 of 20." in log
    assert result(root)["budget"]["admission_new_ids"] == ["alpha_a", "alpha_b", "alpha_c"]
    rule = rule_root(tmp_path / "rule")
    assert wave(rule, F.FakeCycle(rule, {}), "run")[0] == 0
    assert (rule / STATE / "wave-log.md").read_text().splitlines()[0] == \
        "### Cell w1 (rule wave; template `x-rule.json` on p0): N 3"
