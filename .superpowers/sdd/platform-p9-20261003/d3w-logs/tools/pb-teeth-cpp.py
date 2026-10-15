"""D3W-PB's one C++ teeth build (TEETH-RESTORE): python pb-teeth-cpp.py TAG LOG

Keeps fit.cpp's bytes, applies every mutation at once, builds atx-engine-research-composition-tests through
scripts/research-build.ps1 (Debug, tag TAG), runs the named filters, then restores the bytes in the same run (finally)
and asserts `git status --porcelain` is empty and the file's content equals its HEAD blob. Nothing built here is
evidence of the head (EXE-CUR): the binaries carry the mutations.

  T1 ew-theme-aim-v2's modifier refusal      -> AimV2.RefusalsBeforeAnyFit (the four modifier cases)
  T2 ew-theme-aim-v2's gain refusal           -> AimV2.RefusalsBeforeAnyFit (the four gain cases: the message)
  T3 walk-forward-v1 refused with aim-v2      -> AimV2.RefusalsBeforeAnyFit, AimV2.CliWritesGainsAndThemeWeightsOnlyForIt
  T4 the look-ahead inequality cut + lag <= j (review D3W S-10) -> WalkForward.NoRowUsesFutureDecisions
"""
import subprocess
import sys
import time
from pathlib import Path

REPO = Path("C:/atx-wt/pool-23")
FIT = "atx-engine/src/research/composition/fit.cpp"
MUTATIONS = [
    ("T1 aim-v2 modifiers",
     "  if (aim_v2 && (spec.theme_erc || spec.theme_tsmom || spec.theme_resid || spec.mom_volman))",
     "  if (false && aim_v2 && (spec.theme_erc || spec.theme_tsmom || spec.theme_resid || spec.mom_volman))"),
    ("T2 aim-v2 gains",
     "      if (!std::isfinite(gains[k]) || !(gains[k]>0.0))",
     "      if (false && (!std::isfinite(gains[k]) || !(gains[k]>0.0)))"),
    ("T3 walk-forward aim-v2",
     "  if (spec.composition==ew_theme_aim_v2_rule) // P9 lane D3W-PB",
     "  if (false && spec.composition==ew_theme_aim_v2_rule) // P9 lane D3W-PB"),
    ("T4 S-10 look-ahead", "    for (;cut+wf.lag<=j;++cut) {", "    for (;cut+wf.lag<=j+1U;++cut) {"),
]
FILTER = "AimV2.*:WalkForward.*"


def git(*args) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True, check=True).stdout


def main() -> int:
    tag, log_path = sys.argv[1], Path(sys.argv[2])
    log = log_path.open("w", encoding="utf-8")

    def say(text: str) -> None:
        print(text)
        log.write(text + "\n")
        log.flush()

    assert git("status", "--porcelain").strip() == "", "tree not clean before the teeth run"
    head = git("rev-parse", "HEAD").strip()
    path = REPO / FIT
    original = path.read_bytes()
    say(f"# D3W-PB C++ teeth on HEAD {head}; tag {tag}; file {FIT}; filter {FILTER}")
    try:
        text = original.decode("utf-8")
        for name, old, new in MUTATIONS:
            assert text.count(old) == 1, (name, text.count(old))
            text = text.replace(old, new)
            say(f"applied {name}: {old.strip()} -> {new.strip()}")
        path.write_bytes(text.encode("utf-8"))
        receipt = REPO / "build-equity" / f"mega-{tag}-receipt.json"
        deadline = time.time() + 20 * 60
        while True:
            build = subprocess.run(["powershell", "-NoProfile", "-File", "scripts\\research-build.ps1", "-Tag", tag,
                                    "-Targets", "atx-engine-research-composition-tests"], cwd=REPO,
                                   capture_output=True, text=True)
            say(f"research-build exit {build.returncode}")
            say(build.stdout[-3000:] + build.stderr[-2000:])
            if build.returncode == 0 or receipt.exists() or time.time() > deadline:
                break
            say("no receipt (admission refused): retry in 60 s")
            time.sleep(60)
        if build.returncode == 0:
            exe = REPO / "build-equity" / "bin" / "atx-engine-research-composition-tests.exe"
            run = subprocess.run([str(exe), f"--gtest_filter={FILTER}"], capture_output=True, text=True, cwd=REPO)
            say(f"gtest exit {run.returncode} (non-zero expected: the mutations must turn named tests red)")
            say(run.stdout[-20000:] + run.stderr[-2000:])
    finally:
        path.write_bytes(original)
        status = git("status", "--porcelain").strip()
        work = git("hash-object", FIT).strip()
        blob = git("rev-parse", f"HEAD:{FIT}").strip()
        say(f"restored {FIT}: git status --porcelain {'EMPTY' if not status else repr(status)}; "
            f"hash-object {work} vs HEAD blob {blob}: {'EQUAL' if work == blob else 'DIFFER'}")
        log.close()
        if status or work != blob:
            print("RESTORE FAILED: STOP", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
