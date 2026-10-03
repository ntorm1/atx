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
import wave_manifest as WM  # noqa: E402
import wave_seal  # noqa: E402
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


# ------------------------------------------------------------------ MAJOR 2: the sign rule reads every string's row
MIXED = [F.candidate("alpha_a"), F.candidate("alpha_r", kind="replace", replaces=["m2"], rescreen=True)]


def test_a_mixed_wave_keeps_its_rescreen_admitted_with_the_prior_sign(tmp_path):
    """add-alpha lists only alpha_a in gate.admitted (the re-screen is gate.report), so cycle_verdict.json has no row
    for alpha_r; the fit's admission.json has it, and PM7-35 keeps it."""
    root = F.build(tmp_path / "r", candidates=MIXED)
    fake = F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_r": ("admitted", 1)}, listed={"alpha_a"})
    code, out = wave(root, fake, "run", "--until", "spec")
    assert code == 0, out
    sc = receipt(root, "03-screen.json")["outputs"]
    assert sc["decision"]["kept"] == ["alpha_a", "alpha_r"] and sc["decision"]["dropped"] == []
    assert [r["id"] for r in sc["rows"]] == ["alpha_a", "alpha_r"]                  # the parent's m1 row is not a string
    assert receipt(root, "04-spec.json")["outputs"]["kind"] == "screen-library"     # no b library


def test_a_string_without_an_admission_row_stops_the_screen(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1)})
    code, _ = wave(root, fake, "run")
    err = failed(root, "03-screen.failed-1.json")
    assert code == 4 and "no admission row for ['alpha_c']" in err and "not a drop" in err


# ------------------------------------------------------------------ MAJOR 3: a stage commits only what it writes
ADD_ALPHA_W1 = {"atx-impl/strategies/alphas/registry.json", "atx-impl/strategies/libraries/w1.json",
                "atx-impl/strategies/libraries/w1.prereg.md", "atx-impl/strategies/ic_w1.json",
                "atx-impl/strategies/ic_w1.recipe.v2.json", "scripts/specs/v8/lib-w1.json"}


def test_a_local_edit_between_stages_is_refused_not_committed(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    assert wave(root, fake, "run", "--until", "preflight")[0] == 0
    F.write(root, "scripts/research_add_alpha_fix.py", "# a local fix\n")          # untracked, inside the pathspec
    code, _ = wave(root, fake, "run")
    assert code == 3 and "dirty before the stage writes anything (scripts/research_add_alpha_fix.py)" in \
        failed(root, "02-register.failed-1.json")
    assert not any("add-alpha" in c for c in fake.calls)


class Sneaky(F.FakeCycle):
    def add_alpha(self, args):
        F.write(self.root, "scripts/sneaky/new_dir/tool.py", "x = 1\n")              # an untracked dir: one file
        return super().add_alpha(args)


def test_a_file_the_stage_does_not_write_blocks_its_commit(tmp_path):
    root = F.build(tmp_path / "r")
    code, _ = wave(root, Sneaky(root, KEPT_ALL), "run")
    err = failed(root, "02-register.failed-1.json")
    assert code == 3 and "dirty paths this stage does not write (scripts/sneaky/new_dir/tool.py)" in err
    assert F.git(root, "log", "--format=%s").splitlines()[0] == "synthetic root"     # nothing committed


def test_a_retry_after_the_stage_failed_commits_exactly_its_files(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL, fail={"alpha_b": 4})                          # add-alpha alpha_b fails
    assert wave(root, fake, "run")[0] == 4
    assert set(F.git(root, "status", "--porcelain", "-uall").split()) >= {"scripts/specs/v8/lib-w1.json"}
    fake.fail = {}
    code, out = wave(root, fake, "run", "--until", "register")
    assert code == 0, out
    head = F.git(root, "show", "--name-only", "--format=%s", "HEAD").split("\n")
    assert head[0].startswith("wave w1: register library w1") and set(filter(None, head[1:])) == ADD_ALPHA_W1


# ------------------------------------------------------------------ MAJOR 4: the seal scan covers every log of the wave
ONE_DROPPED = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", 1)}


def test_the_screen_librarys_logs_are_scanned_in_a_b_library_wave(tmp_path):
    root = F.build(tmp_path / "r", speed={"screen_first": False})                  # the b library screens nothing
    fake = F.FakeCycle(root, ONE_DROPPED, screen_log="u pass through 2024-02-01\n")
    code, _ = wave(root, fake, "run")
    err = failed(root, "07-verify.failed-1.json")
    assert code == 4 and "1 date token(s) at or after the seal 2024-01-01" in err and "out/u-w1-run/stdout.log" in err
    assert receipt(root, "04-spec.json")["outputs"]["cell_spec"].endswith("lib-w1b.json")   # not the cell's own dir


class LoudAddAlpha(F.FakeCycle):
    def add_alpha(self, args):
        done = super().add_alpha(args)
        return F.subprocess.CompletedProcess(done.args, 0, "K1 plan: 3 rows, last 2024-06-28\n", "")


def test_every_command_console_is_kept_and_scanned(tmp_path):
    root = F.build(tmp_path / "r")
    code, _ = wave(root, LoudAddAlpha(root, KEPT_ALL), "run")
    err = failed(root, "07-verify.failed-1.json")
    assert code == 4 and "out/waves/w1/consoles/001-add-alpha-alpha-a.log" in err
    consoles = sorted(p.name for p in (root / STATE / "consoles").glob("*.log"))
    assert consoles[:3] == ["001-add-alpha-alpha-a.log", "002-add-alpha-alpha-b.log", "003-add-alpha-alpha-c.log"]


class LoudBundle(F.FakeCycle):
    def bounded(self, argv):
        done = super().bounded(argv)
        run_dir = argv[argv.index("--output") + 1]
        if "bundle-run" in run_dir:
            F.write(self.root, f"{run_dir}/stdout.log", "paired sessions 2020-01-02 .. 2024-01-05\n")
        return done


def test_the_record_stage_rescans_after_the_judge_before_the_hidden_data_line(tmp_path):
    root = F.build(tmp_path / "r")
    code, _ = wave(root, LoudBundle(root, KEPT_ALL), "run")
    err = failed(root, "09-record.failed-1.json")
    assert code == 4 and "out/waves/w1/bundle-run1/stdout.log" in err and "no wave result is written" in err
    assert receipt(root, "07-verify.json")["outputs"]["seal_scan"]["tokens_at_or_after_seal"] == 0
    assert not (root / STATE / "wave-result.json").exists()


def test_the_hidden_data_line_counts_the_records_scan(tmp_path):
    root = F.build(tmp_path / "r")
    assert wave(root, F.FakeCycle(root, KEPT_ALL), "run")[0] == 0
    res = json.loads((root / STATE / "wave-result.json").read_text())
    seal = res["seal_scan"]
    verified = receipt(root, "07-verify.json")["outputs"]["seal_scan"]["files"]
    assert seal["tokens_at_or_after_seal"] == 0 and seal["files"] > verified                # the judge's logs too
    assert f"seal scan of {seal['files']} log(s)" in (root / STATE / "wave-log.md").read_text()


# ------------------------------------------------------------------ MAJOR 5: every stage re-checks its predecessor
def test_a_library_spec_edited_after_register_is_never_screened(tmp_path, capsys):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    assert wave(root, fake, "run", "--until", "register")[0] == 0
    spec = root / "scripts/specs/v8/lib-w1.json"
    spec.write_text(spec.read_text().replace("synthetic cell w1", "edited and committed"))
    F.git(root, "commit", "-q", "-am", "edit the library spec")
    code, _ = wave(root, fake, "run")
    assert code == 3 and "STALE: register recorded spec_sha256" in capsys.readouterr().err
    assert not any("--screen" in c for c in fake.calls) and not (root / STATE / "receipts/03-screen.json").exists()


def test_a_file_a_done_stage_recorded_that_moves_is_stale_on_resume(tmp_path, capsys):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, KEPT_ALL)
    assert wave(root, fake, "run", "--until", "match")[0] == 0
    nav = receipt(root, "05-run.json")["outputs"]["nav"]
    summary = root / nav / "summary.json"
    summary.write_text(summary.read_text().replace('"complete"', '"rewritten"'))
    code, _ = wave(root, fake, "run")
    assert code == 3 and "STALE: run recorded summary_sha256" in capsys.readouterr().err
    code, lines = wave(root, fake, "status")
    assert [x.split()[:2] for x in lines][5] == ["match", "stale"]


class OtherSpecBinding(F.FakeCycle):
    def cycle(self, args):
        done = super().cycle(args)
        if "--stop-after" in args:
            spec, c = self.spec_outputs(args[1])
            p = self.root / f"{c.out(spec['nav']['output'])}-run/cycle_binding.json"
            p.write_text(json.dumps(dict(json.loads(p.read_text()), spec_sha256="0" * 64)))
        return done


def test_verify_compares_the_navs_binding_with_the_cell_spec(tmp_path):
    root = F.build(tmp_path / "r")
    code, _ = wave(root, OtherSpecBinding(root, KEPT_ALL), "run")
    err = failed(root, "07-verify.failed-1.json")
    assert code == 4 and f"names spec {'0' * 64} (spec-digest-v1), not the cell spec" in err


# ------------------------------------------------------------------ MAJOR 6: carried marginal rows
def result(root: Path) -> dict:
    return json.loads((root / STATE / "wave-result.json").read_text())


def test_carried_rows_hold_only_the_per_row_fields(tmp_path):
    root = F.build(tmp_path / "r")
    assert wave(root, F.FakeCycle(root, ONE_DROPPED), "run")[0] == 0
    assert receipt(root, "04-spec.json")["outputs"]["marginal"] == {"mode": "themes", "screen_mode": "themes",
                                                                   "reuse": True}
    mg = result(root)["marginal"]
    assert mg["mode"] == "themes" and [r["id"] for r in mg["rows"]] == ["alpha_a", "alpha_c"]
    assert mg["rows"][0] == {"id": "alpha_a", "ic21": 0.01, "ic21_hac_t": 2.0, "marginal_ic21": 0.005,
                             "marginal_hac_t": 1.5, "max_abs_rho": None, "max_rho_member": None}


def test_reuse_is_off_when_the_screens_marginal_mode_differs(tmp_path):
    """A replacing wave screens pool-only; its replacement dropped, the b library adds only and runs themes: the
    screen's rows are not the b library's, so it runs its own marginal (reuse on in the manifest)."""
    cands = [F.candidate("alpha_a"), F.candidate("alpha_r", kind="replace", replaces=["m1"])]
    root = F.build(tmp_path / "r", candidates=cands)
    assert wave(root, F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_r": ("admitted", -1)}), "run")[0] == 0
    assert receipt(root, "04-spec.json")["outputs"]["marginal"] == {"mode": "themes", "screen_mode": "pool-only",
                                                                   "reuse": False}
    b = json.loads((root / "scripts/specs/v8/lib-w1b.json").read_text())
    assert b["marginal"]["themes"] == "reference_weights"                             # its own pass, themes
    mg = result(root)["marginal"]
    assert mg["source"].startswith("the cell's own marginal (scripts/specs/v8/lib-w1b.json, themes)")
    assert [r["id"] for r in mg["rows"]] == ["alpha_a"] and mg["rows"][0]["max_abs_rho"] == 0.3


# ------------------------------------------------------------------ MAJOR 7: no state or copies in the code pathspec
def test_out_dir_and_copy_to_inside_the_code_pathspec_are_refused():
    m = F.manifest()
    m["fields"]["manifest_sha256"] = "0" * 64
    assert WM.validate(m) == []
    for out_dir, copy_to in (("scripts/waves/w1", "sprint/waves"), ("./atx-impl/out", "sprint/waves"),
                             ("out/waves/w1", "Scripts/specs/v8/waves"), ("out/waves/w1", "CMakeLists.txt"),
                             ("out/waves/w1", "cmake/waves")):
        bad = dict(m, out_dir=out_dir, record={"copy_to": copy_to})
        problems = " | ".join(WM.validate(bad))
        assert "inside the code pathspec" in problems, (out_dir, copy_to)
    assert WM.validate(dict(m, out_dir="build-equity/waves/w1", record={"copy_to": ".superpowers/sdd/x"})) == []


def test_a_manifest_with_its_state_in_the_code_pathspec_never_runs(tmp_path):
    root = F.build(tmp_path / "r", out_dir="atx-impl/waves/w1")
    code, _ = wave(root, F.FakeCycle(root, KEPT_ALL), "run")
    assert code == 2 and not (root / "atx-impl/waves").exists()


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
