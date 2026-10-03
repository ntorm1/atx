"""End-to-end research cycle on the synthetic tiny_world (platform v8 E-3, P9 lane T1): the canary of every build.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_e2e.py

Offline (always): the fixture is byte-deterministic and equals the pins locked in scripts/specs/tiny.json, its fields
manifest carries the research seal (P9 ruling P13), its planted structure holds, the numpy reference of the runner's
rank IC (fixtures/tiny_world_ic.py) is well conditioned, and the resolved spec plans through research_cycle with every
pin verified.

Live (only when ATX_EQUITY_BIN names the directory of atx-equity-strategy-ic.exe and atx-equity-strategy-targets.exe;
ATX_EQUITY_BUILD_TYPE names their build type, Debug (default) or Release; ATX_CANARY_REQUIRED=1 turns the skip into a
failure, as research-build.ps1 -Canary sets it): builds the world in a temporary root outside the repository, runs
    research_cycle.py run ROOT/tiny.json --root ROOT --no-git
and asserts
  - exit 0, copy_b rejected as redundant with planted_b, planted_a and planted_b admitted;
  - the build type's golden digests of the u, fit, w and NAV payloads (fixtures/tiny_world_goldens.json);
  - every member's rank IC at the orientation horizon (h 21) in the u pass's orientations.json equals the numpy
    reference: mean within 1e-12 (absolute), HAC standard error within 1e-9 (relative), HAC lag and valid dates
    exact. The tolerance is rounding, not a model: the reference sums the per-row correlation with numpy reductions,
    the executable with its own loops (fixtures/tiny_world_ic.py states the recipe it mirrors);
  - planted signal recovery (DS review section 6; PM ruling T1-SE): for planted_a and planted_b the executable's
    h-21 mean rank IC lies within PLANTED_BAND_SE = 2 HAC standard errors of the value the generator planted,
    |z| <= 2 with z = (mean - planted) / SE. The band is pre-registered at 2 SE, not 1: the runner's lag-42 HAC SE on
    250 overlapping dates under-covers the realised dispersion (at seed 7 the reference puts planted_a at z -0.82 and
    planted_b at z -1.14; the T1 report gives the seed scan). noise_c stays out of the band: its planted value is 0
    and the reference puts it at z +2.23 (a sampling draw of a persistent state);
  - the time budget (ATX_E2E_MAX_SECONDS, default 15 s for the first run), and that a second run finds every phase
    up to date and creates nothing.
It prints, and --record stores, the planted block (mean, HAC t, planted value, z, band, inside the band).

Output paths are found by payload name, not by run-dir name: today's IC attempt dirs <output>-<k>/, lane E1's attempt
sub-dirs <output>/attempt-<k>/ (K-P9-10) and single-attempt <output>/ all resolve (a first fresh run has one).

Goldens: root records one entry per build type after a build with
    python scripts/tests/test_cycle_e2e.py --record --bin DIR --build-type Debug|Release
(runs the same cycle once in a fresh temporary root; refuses unless copy_b is rejected as redundant, planted_a and
planted_b are admitted, the IC reference ties and both planted members lie inside the band; then writes the digests,
the planted block and the executables' SHA-256s into builds[<build type>]). An entry already recorded is replaced
only with --repin (a ruled re-pin, plan section 0.6), and the old and new digests are printed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE / "fixtures"))
sys.path.insert(0, str(HERE.parent))
import tiny_world as TW  # noqa: E402
import tiny_world_ic as TIC  # noqa: E402
import research_cycle as RC  # noqa: E402

GOLDENS = HERE / "fixtures" / "tiny_world_goldens.json"
GOLDENS_SCHEMA = "atx.tiny-world-goldens/v2"
CYCLE = HERE.parent / "research_cycle.py"
BIN = os.environ.get("ATX_EQUITY_BIN")
BUILD_TYPE = os.environ.get("ATX_EQUITY_BUILD_TYPE", "Debug")
REQUIRED = os.environ.get("ATX_CANARY_REQUIRED") == "1"
PHASES = ("fields", "u", "fit", "card", "gate", "w", "nav")
OUTPUT_PHASES = ("fields", "u", "fit", "card", "w", "nav")
OUTPUT_KEYS = ("u_orientations_json_sha256", "fit_admission_decisions_sha256", "fit_weights_sha256",
               "w_combined_f64_sha256", "nav_primary_daily_csv_sha256")
MEMBERS = tuple(m[0] for m in TW.MEMBERS)
PLANTED = ("planted_a", "planted_b")
ORIENTATION_HORIZON = 21
IC_MEAN_ABS_TOLERANCE = 1e-12
IC_SE_REL_TOLERANCE = 1e-9
PLANTED_BAND_SE = 2.0           # PM ruling T1-SE (P9 progress.md): planted recovery asserted within 2 HAC SE


# ------------------------------------------------------------------ outputs and digests
def find_output(root: Path, base: str, name: str) -> Path:
    """The one file NAME of the phase output BASE (relative to ROOT) in any attempt layout."""
    patterns = (f"{base}/{name}", f"{base}-[0-9]*/{name}", f"{base}/attempt-[0-9]*/{name}")
    hits = sorted({p for pattern in patterns for p in root.glob(pattern)})
    if len(hits) != 1:
        raise AssertionError(f"{base}: expected one {name} in an attempt layout, found {[str(h) for h in hits]}")
    return hits[0]


def output_paths(root: Path, spec: dict) -> dict:
    summary = find_output(root, spec["nav"]["output"], "summary.json")
    scenario = json.loads(summary.read_bytes())["primary_scenario"]
    return {"orientations": find_output(root, spec["ic"]["u_output"], "orientations.json"),
            "admission": find_output(root, spec["fit"]["output"], "admission.json"),
            "weights": find_output(root, spec["fit"]["output"], "composition_weights.json"),
            "combined": find_output(root, spec["ic"]["w_output"], "train_combined.f64"),
            "daily": summary.parent / f"daily_{scenario}.csv", "scenario": scenario}


def sha256_file(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical_sha256(doc) -> str:
    return hashlib.sha256(json.dumps(doc, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def admission_digest(doc: dict) -> str:
    """admission.json without its inputs block (run provenance: the u pass summary SHA carries wall_seconds and cache
    hit counts; the fitter's script SHA changes with any edit of the script)."""
    return canonical_sha256({k: v for k, v in doc.items() if k != "inputs"})


def weights_digest(doc: dict) -> str:
    """composition_weights.json's decision content: schema, signs and weights (provenance and inputs carry the u
    summary SHA and the fitter's script SHA)."""
    return canonical_sha256({k: doc[k] for k in ("schema", "signs", "weights")})


def digests(root: Path, spec: dict) -> dict:
    p = output_paths(root, spec)
    return {"u_orientations_json_sha256": sha256_file(p["orientations"]),
            "fit_admission_decisions_sha256": admission_digest(json.loads(p["admission"].read_bytes())),
            "fit_weights_sha256": weights_digest(json.loads(p["weights"].read_bytes())),
            "w_combined_f64_sha256": sha256_file(p["combined"]),
            "nav_primary_daily_csv_sha256": sha256_file(p["daily"]), "primary_scenario": p["scenario"]}


# ------------------------------------------------------------------ the IC reference and the planted report
def rank_estimate(candidate: dict, horizon: int) -> dict:
    """The u pass's rank-IC estimate of one orientations.json candidate at ``horizon``."""
    for row in candidate["ic"]["horizons"]:
        if row["horizon"] == horizon:
            return row["rank"]
    raise AssertionError(f"{candidate['id']}: no horizon {horizon} in orientations.json")


def orientation_candidates(root: Path, spec: dict) -> dict:
    doc = json.loads(output_paths(root, spec)["orientations"].read_bytes())
    return {c["id"]: c for c in doc["candidates"]}


def ic_reference_problems(candidates: dict, world: dict) -> list[str]:
    """Every member's h-21 rank IC of the executable against the numpy reference (empty when they tie)."""
    problems = []
    for member in MEMBERS:
        want = TIC.research_ic(world, member, ORIENTATION_HORIZON)
        got = rank_estimate(candidates[member], ORIENTATION_HORIZON)
        if not got.get("inference_defined") or got.get("standard_error") is None or got.get("mean") is None:
            problems.append(f"{member}: the executable's h-21 rank inference is undefined ({got})")
            continue
        if abs(got["mean"] - want["mean"]) > IC_MEAN_ABS_TOLERANCE:
            problems.append(f"{member}: mean rank IC {got['mean']!r} vs reference {want['mean']!r}")
        if abs(got["standard_error"] - want["standard_error"]) > IC_SE_REL_TOLERANCE * want["standard_error"]:
            problems.append(f"{member}: HAC SE {got['standard_error']!r} vs reference {want['standard_error']!r}")
        for key, ref in (("required_hac_lag", "hac_lag"), ("valid_dates", "valid_dates"),
                         ("calendar_dates", "calendar_dates")):
            if got.get(key) != want[ref]:
                problems.append(f"{member}: {key} {got.get(key)!r} vs reference {want[ref]!r}")
    return problems


def planted_block(candidates: dict) -> dict:
    """The planted members' executable estimates against their planted rank IC and the T1-SE band."""
    out = {}
    for member in PLANTED:
        est = rank_estimate(candidates[member], ORIENTATION_HORIZON)
        planted = TIC.planted_rank_ic(member, ORIENTATION_HORIZON)
        out[member] = TIC.planted_report(est["mean"], est["standard_error"], planted, PLANTED_BAND_SE)
    return out


def planted_problems(block: dict) -> list[str]:
    """Planted members outside the band (empty when the planted signal is recovered)."""
    return [f"{member}: mean rank IC {r['mean']!r} lies {r['z']:+.3f} SE from the planted {r['planted']!r} "
            f"(band {r['band_se']} SE, PM ruling T1-SE)" for member, r in block.items() if not r["within_band"]]


def admission_problems(rows: dict) -> list[str]:
    """The fixture's admission facts: copy_b redundant with planted_b, both planted members admitted."""
    problems = []
    copy_b = rows["copy_b"]
    if copy_b["status"] != "reject_redundant" or copy_b.get("redundant_with") != "planted_b":
        problems.append(f"copy_b is {copy_b['status']} (redundant with {copy_b.get('redundant_with')!r}), not "
                        "reject_redundant with planted_b")
    problems += [f"{member} is {rows[member]['status']}, not admitted" for member in PLANTED
                 if rows[member]["status"] != "admitted"]
    return problems


# ------------------------------------------------------------------ the cycle
def accepts_no_git() -> bool:
    done = subprocess.run([sys.executable, str(CYCLE), "--help"], capture_output=True, text=True)
    return "--no-git" in done.stdout


def run_cycle(root: Path) -> tuple[subprocess.CompletedProcess, float]:
    started = time.perf_counter()
    done = subprocess.run([sys.executable, str(CYCLE), "run", str(root / TW.SPEC), "--root", str(root), "--no-git"],
                          cwd=root, capture_output=True, text=True)
    return done, time.perf_counter() - started


def admission_rows(root: Path, spec: dict) -> dict:
    doc = json.loads(output_paths(root, spec)["admission"].read_bytes())
    return {row["id"]: row for row in doc["candidates"]}


def fresh_root(parent: Path | None = None) -> Path:
    """A new empty root outside the repository (--no-git is accepted only there)."""
    root = Path(tempfile.mkdtemp(prefix="tiny-world-", dir=parent))
    if root.resolve().is_relative_to(REPO.resolve()):
        raise RuntimeError(f"temporary root {root} lies inside the repository")
    return root


def load_goldens() -> dict:
    doc = json.loads(GOLDENS.read_bytes())
    if doc.get("schema") != GOLDENS_SCHEMA:
        raise AssertionError(f"{GOLDENS.name}: schema {doc.get('schema')!r} is not {GOLDENS_SCHEMA}")
    return doc


def template_fields_pin() -> str:
    return TW.template_pins(json.loads(TW.TEMPLATE.read_bytes()))["fields"]


# ------------------------------------------------------------------ offline
def test_tiny_world_is_deterministic_and_matches_the_locked_spec(tmp_path):
    a = TW.build(tmp_path / "a", bin_dir=tmp_path / "bin")
    b = TW.build(tmp_path / "b", bin_dir=tmp_path / "bin")
    keys = ("role_manifest_sha256", "fields_manifest_sha256", "library_sha256", "recipe_sha256", "registry_sha256")
    assert {k: a[k] for k in keys} == {k: b[k] for k in keys}
    locked = TW.template_pins(json.loads(TW.TEMPLATE.read_bytes()))
    assert locked == {"library": a["library_sha256"], "recipe": a["recipe_sha256"], "role": a["role_manifest_sha256"],
                      "fields": a["fields_manifest_sha256"]}
    for root in (tmp_path / "a", tmp_path / "b"):  # every payload byte, not only the manifests
        files = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() and p.name != TW.SPEC)
        assert files == sorted(p.relative_to(tmp_path / "a").as_posix() for p in (tmp_path / "a").rglob("*")
                               if p.is_file() and p.name != TW.SPEC)
        assert all(sha256_file(root / f) == sha256_file(tmp_path / "a" / f) for f in files)
    spec = json.loads(Path(a["spec"]).read_bytes())
    assert spec["python"] == Path(sys.executable).as_posix()
    assert spec["exes"] == {"ic": (tmp_path / "bin" / TW.EXES["ic"]).as_posix(),
                            "nav": (tmp_path / "bin" / TW.EXES["nav"]).as_posix()}
    for section in ("runner", "fit", "card"):
        assert Path(spec[section]["script"]).is_absolute() and Path(spec[section]["script"]).is_file(), section
    with pytest.raises(FileExistsError):
        TW.build(tmp_path / "a")


def test_fields_manifest_carries_the_research_seal(tmp_path):
    TW.build(tmp_path, bin_dir=tmp_path / "bin")
    manifest = json.loads((tmp_path / TW.FIELDS_DIR / "manifest.json").read_bytes())
    tools = REPO / "atx-engine" / "tools"
    sys.path.insert(0, str(tools))
    try:
        import research_window as RW
    finally:
        sys.path.remove(str(tools))
    assert manifest["seal"]["exclusive_end"] == RW.current()["SEAL_DATE"] == TW.research_seal()
    assert TW.weekdays_ending(TW.LAST_SESSION, TW.DATES)[-1].isoformat() < manifest["seal"]["exclusive_end"]


def test_release_spec_drops_the_debug_dll_directory(tmp_path):
    TW.build(tmp_path / "dbg", bin_dir=tmp_path / "bin")
    TW.build(tmp_path / "rel", bin_dir=tmp_path / "bin", build_type="Release")
    dbg = json.loads((tmp_path / "dbg" / TW.SPEC).read_bytes())["env_path_prepend"]
    rel = json.loads((tmp_path / "rel" / TW.SPEC).read_bytes())["env_path_prepend"]
    assert any(p.endswith(TW.DEBUG_DLL_SUFFIX) for p in dbg)
    assert rel == [p for p in dbg if not p.endswith(TW.DEBUG_DLL_SUFFIX)] and rel
    assert rel == RC.BUILDS["equity-rel"]["path"]            # the cycle's own Release DLL path
    with pytest.raises(ValueError):
        TW.build(tmp_path / "bad", bin_dir=tmp_path / "bin", build_type="RelWithDebInfo")


def spearman_rows(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    rx = np.argsort(np.argsort(x, axis=1), axis=1).astype(float)
    ry = np.argsort(np.argsort(y, axis=1), axis=1).astype(float)
    rx -= rx.mean(axis=1, keepdims=True)
    ry -= ry.mean(axis=1, keepdims=True)
    return (rx * ry).sum(axis=1) / np.sqrt((rx * rx).sum(axis=1) * (ry * ry).sum(axis=1))


def test_planted_structure():
    """The members mean what the fixture says: planted_a ranks as zA, copy_b ranks as planted_b, and both planted
    states carry a positive realized rank IC at the runner's h 21 label (the gate's sign check depends on it)."""
    w = TW.simulate(TW.DEFAULT_SEED)
    s, h = slice(TW.SCORE_BEGIN, TW.DATES - 22), 21
    ratio = -(w["si_shares"] / w["shares_out"])
    assert np.array_equal(np.argsort(ratio, axis=1), np.argsort(w["za"], axis=1))
    assert np.array_equal(np.argsort(2 * w["tiny_signal"], axis=1), np.argsort(w["tiny_signal"], axis=1))
    close = w["close"]
    label = close[TW.SCORE_BEGIN + 1 + h:TW.DATES] / close[TW.SCORE_BEGIN + 1:TW.DATES - h] - 1
    for state in ("za", "zb"):
        ic = spearman_rows(w[state][s], label)
        assert ic.mean() > 0, state
    assert int(w["member"][:TW.MEMBER_WARMUP].sum()) == 0 and bool(w["member"][TW.MEMBER_WARMUP:].all())


def test_ic_reference_follows_the_runner_recipe_and_is_well_conditioned():
    """The reference the live canary ties the executable to: 250 mature rows at h 21 (656 - 384 - 22), the lag rule
    max(2h, rule of thumb) = 42, copy_b identical to planted_b (the same ranks), and no two names closer than 1e-9 of
    a row's range on any member (so the executable's arithmetic cannot reorder ranks the reference relies on)."""
    w = TW.simulate(TW.DEFAULT_SEED)
    rows = {m: TIC.research_ic(w, m, ORIENTATION_HORIZON) for m in MEMBERS}
    for member, est in rows.items():
        assert est["valid_dates"] == est["calendar_dates"] == TW.DATES - TW.SCORE_BEGIN - 1 - ORIENTATION_HORIZON
        assert est["hac_lag"] == 2 * ORIENTATION_HORIZON and est["standard_error"] > 0, member
        assert TIC.min_relative_gap(TIC.member_signal(w, member), ORIENTATION_HORIZON) > 1e-9, member
    assert rows["copy_b"] == rows["planted_b"]
    # the reference is the same statistic as the plain spearman of the planted states (planted_b is zB itself)
    s, h = slice(TW.SCORE_BEGIN, TW.DATES - 22), 21
    label = w["close"][TW.SCORE_BEGIN + 1 + h:TW.DATES] / w["close"][TW.SCORE_BEGIN + 1:TW.DATES - h] - 1
    assert math.isclose(rows["planted_b"]["mean"], float(spearman_rows(w["zb"][s], label).mean()), abs_tol=1e-14)


def test_hac_rule_matches_its_closed_form():
    """hac_se on a series whose autocovariances are known by hand: x = (1, -1, 1, -1), lag 1."""
    x = [1.0, -1.0, 1.0, -1.0]                  # mean 0; S = 4 + 2 (1 - 1/2) (-3) = 1; var = 1/16 * 4/3
    assert math.isclose(TIC.hac_se(x, 1), math.sqrt(1.0 / 16.0 * 4.0 / 3.0), rel_tol=0, abs_tol=1e-16)
    assert math.isclose(TIC.hac_se(x, 0), math.sqrt(4.0 / 16.0 * 4.0 / 3.0), rel_tol=0, abs_tol=1e-16)
    assert TIC.rule_of_thumb_lag(250) == 4 and TIC.rule_of_thumb_lag(1) == 0
    assert list(TIC.average_ranks(np.array([3.0, 1.0, 3.0, 2.0]))) == [3.5, 1.0, 3.5, 2.0]


def test_planted_values_follow_the_generator():
    """The planted rank IC from tiny_world's constants: about .045 at h 21 (the module doc's '.05 to first order, x .95
    for the decay'), lower for planted_a (its 21-session decay averages an older state), 0 for the noise member."""
    a, b = TIC.planted_rank_ic("planted_a"), TIC.planted_rank_ic("planted_b")
    assert 0.040 < a < b < 0.050
    assert TIC.planted_rank_ic("copy_b") == b and TIC.planted_rank_ic("noise_c") == 0.0
    assert TIC.planted_rank_ic("not_a_member") is None
    report = TIC.planted_report(0.03, 0.02, b, PLANTED_BAND_SE)
    assert report["within_band"] is (abs(0.03 - b) <= 2 * 0.02) and math.isclose(report["t"], 1.5)
    assert report["band_se"] == PLANTED_BAND_SE == 2.0


def test_reference_planted_members_sit_inside_the_band_at_seed_7():
    """PM ruling T1-SE checked without an executable: at seed 7 the numpy reference of the runner's estimate puts
    planted_a at z -0.82 and planted_b at z -1.14 (inside 2 SE, so a correct executable passes the live band), while
    noise_c sits at z +2.23 from its planted 0 (outside, which is why the band covers the planted members only)."""
    w = TW.simulate(TW.DEFAULT_SEED)
    block = {}
    for member in MEMBERS:
        est = TIC.research_ic(w, member, ORIENTATION_HORIZON)
        block[member] = TIC.planted_report(est["mean"], est["standard_error"],
                                           TIC.planted_rank_ic(member, ORIENTATION_HORIZON), PLANTED_BAND_SE)
    assert planted_problems({m: block[m] for m in PLANTED}) == []
    assert math.isclose(block["planted_a"]["z"], -0.82, abs_tol=0.005)
    assert math.isclose(block["planted_b"]["z"], -1.14, abs_tol=0.005)
    assert math.isclose(block["noise_c"]["z"], 2.23, abs_tol=0.005) and not block["noise_c"]["within_band"]
    moved = dict(block["planted_b"], z=-2.5, within_band=False)          # a collapsed planted signal is refused
    assert planted_problems({"planted_b": moved}) and "planted_b" in planted_problems({"planted_b": moved})[0]


def test_record_refuses_to_overwrite_a_recorded_entry_without_repin(tmp_path, monkeypatch):
    """A recorded build type is replaced only by a ruled re-pin (--repin); the refusal comes before any run."""
    doc = load_goldens()
    doc["builds"]["Debug"] = {"outputs": {k: "0" * 64 for k in OUTPUT_KEYS}, "primary_scenario": "s",
                              "planted": {m: {} for m in PLANTED}, "recorded": {}}
    goldens = tmp_path / "goldens.json"
    goldens.write_bytes((json.dumps(doc, indent=2) + "\n").encode("utf-8"))
    monkeypatch.setattr(sys.modules[__name__], "GOLDENS", goldens)
    monkeypatch.setattr(sys.modules[__name__], "accepts_no_git", lambda: True)
    with pytest.raises(SystemExit, match="ruled re-pin"):
        record(tmp_path / "bin", "Debug")
    assert json.loads(goldens.read_bytes()) == doc


def test_admission_problems_name_every_broken_fact():
    good = {"copy_b": {"status": "reject_redundant", "redundant_with": "planted_b"},
            "planted_a": {"status": "admitted"}, "planted_b": {"status": "admitted"}}
    assert admission_problems(good) == []
    bad = dict(good, copy_b={"status": "admitted"}, planted_a={"status": "reject_veto"})
    assert len(admission_problems(bad)) == 2


def test_find_output_resolves_every_attempt_layout(tmp_path):
    for k, rel in enumerate(("out-1/x.json", "out/attempt-1/x.json", "out/x.json")):
        root = tmp_path / str(k)
        (root / rel).parent.mkdir(parents=True)
        (root / rel).write_text("{}")
        (root / "out-run1").mkdir()                        # a receipt dir never matches
        assert find_output(root, "out", "x.json") == root / rel
    both = tmp_path / "both"
    for rel in ("out-1/x.json", "out-2/x.json"):
        (both / rel).parent.mkdir(parents=True)
        (both / rel).write_text("{}")
    with pytest.raises(AssertionError):
        find_output(both, "out", "x.json")


def test_goldens_belong_to_the_current_world():
    """Relocking the world (a new fields manifest SHA in tiny.json) must come with new goldens."""
    doc = load_goldens()
    assert doc["fields_manifest_sha256"] == template_fields_pin()
    assert set(doc["builds"]) == set(TW.BUILD_TYPES)
    for build_type, entry in doc["builds"].items():
        if entry is not None:
            assert set(entry["outputs"]) == set(OUTPUT_KEYS), build_type
            assert set(entry["planted"]) == set(PLANTED), build_type


def test_tiny_spec_plans_through_research_cycle(tmp_path):
    TW.build(tmp_path, bin_dir=tmp_path / "bin")
    spec_path = tmp_path / TW.SPEC
    cycle = RC.Cycle(RC.load_spec(spec_path), RC.Resolver(tmp_path), spec_path=spec_path)
    assert all(how == "locked, verified" for _, _, how in cycle.pins.values())
    steps = cycle.steps()
    assert [st.phase for st in steps] == list(PHASES)
    assert steps[0].state == "done" and all(st.state in ("pending", "always") for st in steps[1:])
    u = next(st for st in steps if st.phase == "u")
    assert u.argv[1] == (TW.REPO / "scripts" / "run_bounded_research.py").as_posix()
    k = u.argv.index("--train-fields")
    assert u.argv[k + 1:k + 4] == ["fields", "--train-fields-sha256", cycle.spec["fields"]["manifest_sha256"]]
    lines = RC.plan_lines(cycle)
    assert sum(1 for line in lines if line.startswith("# phase ")) == len(PHASES)


def test_tiny_world_inside_the_research_window():
    tools = REPO / "atx-engine" / "tools"
    if not (tools / "research_window.py").is_file():
        pytest.skip("atx-engine/tools/research_window.py (task W0-1) is not on this branch yet")
    sys.path.insert(0, str(tools))
    try:
        import research_window as RW
    finally:
        sys.path.remove(str(tools))
    sessions = [TW.session_ns(d) for d in TW.weekdays_ending(TW.LAST_SESSION, TW.DATES)]
    assert sessions[-1] < RW.TRAIN_END_NS and not RW.is_sealed(sessions[-1])
    assert sessions[TW.SCORE_BEGIN] >= RW.TRAIN_BEGIN_NS


# ------------------------------------------------------------------ live
@pytest.mark.skipif(not BIN and not REQUIRED, reason="set ATX_EQUITY_BIN to the directory of the equity executables")
def test_cycle_e2e_goldens_ic_reference_redundant_copy_and_idempotent_rerun():
    if not BIN:
        pytest.fail("ATX_CANARY_REQUIRED=1 but ATX_EQUITY_BIN is not set")
    if BUILD_TYPE not in TW.BUILD_TYPES:
        pytest.fail(f"ATX_EQUITY_BUILD_TYPE {BUILD_TYPE!r} is not one of {TW.BUILD_TYPES}")
    if not accepts_no_git():
        if REQUIRED:
            pytest.fail("research_cycle.py does not accept --no-git (contract K3)")
        pytest.skip("research_cycle.py does not accept --no-git yet (contract K3, lane A)")
    # A short root directly under the system temp dir, not pytest's tmp_path: the candidate cache nests
    # <role sha>/fp_*/ic1_*/<id>.<key>.json (about 150 characters), and under tmp_path the IC runner's cache
    # publish crosses Windows MAX_PATH (260) and fails with "The system cannot find the path specified".
    root = fresh_root()
    try:
        _live_cycle(root)
    finally:
        import shutil
        shutil.rmtree(root, ignore_errors=True)


def _live_cycle(root: Path) -> None:
    goldens = load_goldens()
    assert goldens["fields_manifest_sha256"] == template_fields_pin(), "goldens belong to another world"
    TW.build(root, bin_dir=Path(BIN), build_type=BUILD_TYPE)
    spec = json.loads((root / TW.SPEC).read_bytes())
    done, seconds = run_cycle(root)
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-4000:]
    admission = admission_problems(admission_rows(root, spec))
    assert not admission, "admission facts of the fixture broken:\n" + "\n".join(admission)
    candidates = orientation_candidates(root, spec)
    problems = ic_reference_problems(candidates, TW.simulate(TW.DEFAULT_SEED))
    assert not problems, "the u pass's rank IC disagrees with the numpy reference:\n" + "\n".join(problems)
    planted = planted_block(candidates)
    print(f"\ntiny_world canary ({BUILD_TYPE}): planted members at h {ORIENTATION_HORIZON}, band {PLANTED_BAND_SE} SE "
          "(PM ruling T1-SE): " + json.dumps(planted, sort_keys=True))
    outside = planted_problems(planted)
    assert not outside, "planted signal not recovered:\n" + "\n".join(outside)
    got = digests(root, spec)
    entry = goldens["builds"][BUILD_TYPE]
    assert entry is not None, (f"{BUILD_TYPE} goldens not recorded: python scripts/tests/test_cycle_e2e.py --record "
                               f"--bin {BIN} --build-type {BUILD_TYPE} (got {got})")
    assert got == dict(entry["outputs"], primary_scenario=entry["primary_scenario"])
    limit = float(os.environ.get("ATX_E2E_MAX_SECONDS", "15"))
    assert seconds < limit, f"first run took {seconds:.1f} s (limit {limit} s)"
    before = sorted(p.name for p in root.iterdir())
    again, _ = run_cycle(root)
    assert again.returncode == 0, again.stdout[-4000:] + again.stderr[-4000:]
    lines = again.stdout.splitlines()
    for phase in OUTPUT_PHASES:
        assert any(line.startswith(f"== {phase}: done") for line in lines), phase
        assert not any(line == f"== {phase}" or line.startswith(f"== {phase} (attempt") for line in lines), phase
    assert sorted(p.name for p in root.iterdir()) == before
    assert digests(root, spec) == got


# ------------------------------------------------------------------ --record
def record(bin_dir: Path, build_type: str, keep: bool = False, repin: bool = False) -> dict:
    if build_type not in TW.BUILD_TYPES:
        raise SystemExit(f"--build-type {build_type!r} is not one of {TW.BUILD_TYPES}")
    if not accepts_no_git():
        raise SystemExit("research_cycle.py does not accept --no-git yet (contract K3): nothing recorded")
    doc = load_goldens()
    if doc["fields_manifest_sha256"] != template_fields_pin():
        raise SystemExit("goldens name another world than scripts/specs/tiny.json: reset them first")
    old = doc["builds"][build_type]
    if old is not None and not repin:
        raise SystemExit(f"{build_type} goldens are recorded already: a change is a ruled re-pin (plan section 0.6); "
                         "pass --repin with the ruling, nothing recorded")
    root = fresh_root()
    TW.build(root, bin_dir=bin_dir, build_type=build_type)
    spec = json.loads((root / TW.SPEC).read_bytes())
    done, seconds = run_cycle(root)
    if done.returncode != 0:
        raise SystemExit(f"cycle failed (exit {done.returncode}) in {root}:\n{done.stdout[-4000:]}\n{done.stderr[-4000:]}")
    refusals = admission_problems(admission_rows(root, spec))
    candidates = orientation_candidates(root, spec)
    refusals += ic_reference_problems(candidates, TW.simulate(TW.DEFAULT_SEED))
    planted = planted_block(candidates)
    refusals += planted_problems(planted)
    if refusals:
        raise SystemExit(f"nothing recorded ({root}):\n" + "\n".join(refusals))
    got = digests(root, spec)
    if old is not None:
        for key in OUTPUT_KEYS + ("primary_scenario",):
            before = old["outputs"].get(key) if key in OUTPUT_KEYS else old.get(key)
            print(f"re-pin {build_type} {key}: {before} -> {got[key]}", file=sys.stderr)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip()
    doc["builds"][build_type] = {
        "outputs": {k: got[k] for k in OUTPUT_KEYS}, "primary_scenario": got["primary_scenario"],
        "planted": planted,
        "recorded": {"git_head": head or None, "bin": Path(bin_dir).as_posix(),
                     "exe_sha256": {k: sha256_file(Path(spec["exes"][k])) for k in sorted(spec["exes"])},
                     "python": sys.version.split()[0], "numpy": np.__version__, "first_run_seconds": round(seconds, 1)}}
    GOLDENS.write_bytes((json.dumps(doc, indent=2) + "\n").encode("utf-8"))
    if not keep:
        import shutil
        shutil.rmtree(root, ignore_errors=True)
    return doc["builds"][build_type]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="record the tiny_world golden digests of one build type (root)")
    ap.add_argument("--record", action="store_true", required=True)
    ap.add_argument("--bin", type=Path, default=Path(BIN) if BIN else None,
                    help="directory of atx-equity-strategy-ic.exe / atx-equity-strategy-targets.exe (ATX_EQUITY_BIN)")
    ap.add_argument("--build-type", choices=TW.BUILD_TYPES, default=BUILD_TYPE,
                    help="the executables' build type (ATX_EQUITY_BUILD_TYPE, default Debug)")
    ap.add_argument("--keep", action="store_true", help="keep the temporary root")
    ap.add_argument("--repin", action="store_true",
                    help="replace an already recorded entry (a ruled re-pin; old and new digests are printed)")
    a = ap.parse_args(argv)
    if a.bin is None:
        ap.error("--bin (or ATX_EQUITY_BIN) is required")
    print(json.dumps(record(a.bin, a.build_type, a.keep, a.repin), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
