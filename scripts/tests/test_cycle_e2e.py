"""End-to-end research cycle on the synthetic tiny_world (platform v8, task E-3): the canary of every platform change.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_e2e.py

Offline (always): the fixture is byte-deterministic and equals the pins locked in scripts/specs/tiny.json, its planted
structure holds, and the resolved spec plans through research_cycle with every pin verified.

Live (only when ATX_EQUITY_BIN names the directory of atx-equity-strategy-ic.exe and atx-equity-strategy-targets.exe):
builds the world in a temporary root outside the repository, runs
    research_cycle.py run ROOT/tiny.json --root ROOT --no-git
and asserts exit 0, the golden digests of orientations.json, admission.json (its decision table) and the primary daily
CSV, that copy_b is rejected as redundant with planted_b, the time budget (ATX_E2E_MAX_SECONDS, default 15 s for the
first run), and that a second run finds every phase up to date and creates nothing. It skips with a message while
research_cycle.py does not accept --no-git (contract K3, lane A).

Goldens: scripts/tests/fixtures/tiny_world_goldens.json. Root records them after a build with
    python scripts/tests/test_cycle_e2e.py --record --bin DIR
(runs the same cycle once in a fresh temporary root and writes the three digests plus the executables' SHA-256s).
"""
from __future__ import annotations

import argparse
import hashlib
import json
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
import research_cycle as RC  # noqa: E402

GOLDENS = HERE / "fixtures" / "tiny_world_goldens.json"
CYCLE = HERE.parent / "research_cycle.py"
BIN = os.environ.get("ATX_EQUITY_BIN")
PHASES = ("fields", "u", "fit", "card", "gate", "w", "nav")
OUTPUT_PHASES = ("fields", "u", "fit", "card", "w", "nav")


# ------------------------------------------------------------------ digests
def sha256_file(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def admission_digest(doc: dict) -> str:
    """SHA-256 of admission.json's canonical JSON without its inputs block (run provenance: the u pass summary SHA
    carries wall_seconds and cache hit counts; the fitter's script SHA changes with any edit of the script)."""
    body = {k: v for k, v in doc.items() if k != "inputs"}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def output_paths(root: Path, spec: dict) -> dict:
    nav = root / spec["nav"]["output"]
    summary = json.loads((nav / "summary.json").read_bytes())
    scenario = summary["primary_scenario"]
    return {"orientations": root / f"{spec['ic']['u_output']}-1" / "orientations.json",
            "admission": root / spec["fit"]["output"] / "admission.json",
            "daily": nav / f"daily_{scenario}.csv", "scenario": scenario}


def digests(root: Path, spec: dict) -> dict:
    p = output_paths(root, spec)
    return {"orientations_json_sha256": sha256_file(p["orientations"]),
            "admission_decisions_sha256": admission_digest(json.loads(p["admission"].read_bytes())),
            "primary_daily_csv_sha256": sha256_file(p["daily"]), "primary_scenario": p["scenario"]}


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
@pytest.mark.skipif(not BIN, reason="set ATX_EQUITY_BIN to the directory of the equity executables")
def test_cycle_e2e_goldens_redundant_copy_and_idempotent_rerun(tmp_path):
    if not accepts_no_git():
        pytest.skip("research_cycle.py does not accept --no-git yet (contract K3, lane A)")
    goldens = json.loads(GOLDENS.read_bytes())
    root = fresh_root(tmp_path)
    TW.build(root, bin_dir=Path(BIN))
    spec = json.loads((root / TW.SPEC).read_bytes())
    done, seconds = run_cycle(root)
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-4000:]
    rows = admission_rows(root, spec)
    assert rows["copy_b"]["status"] == "reject_redundant" and rows["copy_b"]["redundant_with"] == "planted_b"
    assert rows["planted_a"]["status"] == rows["planted_b"]["status"] == "admitted"
    got = digests(root, spec)
    want = dict(goldens["outputs"], primary_scenario=goldens["primary_scenario"])
    assert all(v is not None for v in want.values()), (
        f"goldens not recorded: python scripts/tests/test_cycle_e2e.py --record --bin {BIN} (got {got})")
    assert got == want
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
def record(bin_dir: Path, keep: bool = False) -> dict:
    if not accepts_no_git():
        raise SystemExit("research_cycle.py does not accept --no-git yet (contract K3): nothing recorded")
    root = fresh_root()
    TW.build(root, bin_dir=bin_dir)
    spec = json.loads((root / TW.SPEC).read_bytes())
    done, seconds = run_cycle(root)
    if done.returncode != 0:
        raise SystemExit(f"cycle failed (exit {done.returncode}) in {root}:\n{done.stdout[-4000:]}\n{done.stderr[-4000:]}")
    rows = admission_rows(root, spec)
    if rows["copy_b"]["status"] != "reject_redundant":
        raise SystemExit(f"copy_b is {rows['copy_b']['status']}, not reject_redundant: nothing recorded ({root})")
    got = digests(root, spec)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip()
    doc = json.loads(GOLDENS.read_bytes())
    doc["outputs"] = {k: got[k] for k in doc["outputs"]}
    doc["primary_scenario"] = got["primary_scenario"]
    doc["recorded"] = {"git_head": head or None, "bin": Path(bin_dir).as_posix(),
                       "exe_sha256": {k: sha256_file(Path(spec["exes"][k])) for k in sorted(spec["exes"])},
                       "python": sys.version.split()[0], "numpy": np.__version__,
                       "first_run_seconds": round(seconds, 1)}
    GOLDENS.write_bytes((json.dumps(doc, indent=2) + "\n").encode("utf-8"))
    if not keep:
        import shutil
        shutil.rmtree(root, ignore_errors=True)
    return doc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="record the tiny_world golden digests (root, after a build)")
    ap.add_argument("--record", action="store_true", required=True)
    ap.add_argument("--bin", type=Path, default=Path(BIN) if BIN else None,
                    help="directory of atx-equity-strategy-ic.exe / atx-equity-strategy-targets.exe (ATX_EQUITY_BIN)")
    ap.add_argument("--keep", action="store_true", help="keep the temporary root")
    a = ap.parse_args(argv)
    if a.bin is None:
        ap.error("--bin (or ATX_EQUITY_BIN) is required")
    print(json.dumps(record(a.bin, a.keep), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
