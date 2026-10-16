"""GATE step 3, COMPOSE-1a (ledger "Ruling COMPOSE-1a", "Ruling COMPOSE-1", "Ruling COMPOSE-PRE"): the store-backed
parse check of `mv-aim-v1` + `info-clock-rate-v1` together on the tiny world (synthetic data only; no product code).

usage (cwd C:/atx-wt/pool-2; interpreter C:/Program Files/Python312/python.exe -B):
  python compose_1a.py world     build the tiny world (scripts/tests/fixtures/tiny_world.py, seed 7, Release bin) into
                                 build-equity/p9-gate-compose-tw
  python compose_1a.py store     the step-1 Release `atx-equity-strategy-risk risk` on the tiny world's role and fields
                                 (`--emit-exposures all --emit-container --bias-families extended --recipe atx-cov-v1`,
                                 the P9-MV store's recipe flags) into build-equity/p9-gate-compose-store
  python compose_1a.py nav       the Release NAV exe on the tiny world's NAV argv (tiny.json's nav block on the tiny
                                 role and fields; --combined names a file that does not exist: the parse, the store
                                 open and the run checks come before the blend load) with COMPOSE-1's flags
                                 `--rate info-clock-rate-v1 --risk-model <store> --risk-model-sha256 <manifest>
                                 --target-rule mv-aim-v1`, then two controls (mv-aim-v1 alone, the rate alone) and a
                                 two-store control (COMPOSE-1's argv plus a second --risk-model); each into its own
                                 not-yet-existing --output under build-equity/p9-gate-compose-nav-<k>
Every step appends lines to gate-logs/compose-1a.txt: exit code, the stderr / stdout lines (refusal text), exe SHA-256.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
LOGS = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "gate-logs"
BIN = REPO / "build-equity-rel" / "bin"
RISK, NAV = BIN / "atx-equity-strategy-risk.exe", BIN / "atx-equity-strategy-targets.exe"
TW = REPO / "build-equity" / "p9-gate-compose-tw"
STORE = REPO / "build-equity" / "p9-gate-compose-store"


def sha_file(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def log(line: str) -> None:
    print(line)
    with (LOGS / "compose-1a.txt").open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")


def run(tag: str, cmd: list[str]) -> int:
    done = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, encoding="utf-8", errors="replace")
    log(f"{tag}: exit {done.returncode}; argv {' '.join(str(c) for c in cmd[1:])}")
    for stream, text in (("stdout", done.stdout), ("stderr", done.stderr)):
        lines = [x for x in text.splitlines() if x.strip()]
        for x in lines[-12:]:
            log(f"  {stream}: {x[:400]}")
    return done.returncode


def world() -> int:
    sys.path[:0] = [str(REPO / "scripts" / "tests" / "fixtures")]
    import tiny_world as tw  # noqa: E402
    pins = tw.build(TW, bin_dir=BIN, build_type="Release")
    log(f"world {TW.relative_to(REPO).as_posix()}: seed {pins['seed']} role {pins['role_manifest_sha256']} fields "
        f"{pins['fields_manifest_sha256']} (synthetic)")
    return 0


def pins() -> tuple[str, str]:
    return sha_file(TW / "role" / "manifest.json"), sha_file(TW / "fields" / "manifest.json")


def store() -> int:
    role_sha, fields_sha = pins()
    log(f"store exe {RISK.relative_to(REPO).as_posix()} sha256 {sha_file(RISK)}")
    rc = run("store", [str(RISK), "risk", "--role", "build-equity/p9-gate-compose-tw/role/manifest.json",
                       "--role-sha256", role_sha, "--fields", "build-equity/p9-gate-compose-tw/fields/manifest.json",
                       "--fields-sha256", fields_sha, "--output", "build-equity/p9-gate-compose-store",
                       "--emit-exposures", "all", "--emit-container", "--bias-families", "extended",
                       "--recipe", "atx-cov-v1"])
    if (STORE / "manifest.json").is_file():
        log(f"store manifest sha256 {sha_file(STORE / 'manifest.json')}; files "
            f"{sorted(p.name for p in STORE.iterdir())}")
    return rc


def nav() -> int:
    role_sha, fields_sha = pins()
    spec = json.loads((TW / "tiny.json").read_text(encoding="utf-8"))["nav"]
    flags = [f.replace("{leverage}", spec["leverage"]) for f in spec["flags"]]
    base = [str(NAV), "nav", "--combined", "build-equity/p9-gate-compose-tw/no-combined.json", "--combined-sha256",
            "0" * 64, "--role", "build-equity/p9-gate-compose-tw/role/manifest.json", "--role-sha256", role_sha,
            "--fields", "build-equity/p9-gate-compose-tw/fields/manifest.json", "--fields-sha256", fields_sha,
            "--rule", spec["rule"], *flags]
    store_flags = []
    if (STORE / "manifest.json").is_file():
        store_flags = ["--risk-model", "build-equity/p9-gate-compose-store", "--risk-model-sha256",
                       sha_file(STORE / "manifest.json")]
    log(f"nav exe {NAV.relative_to(REPO).as_posix()} sha256 {sha_file(NAV)}; store present {bool(store_flags)}")
    rate, mv = ["--rate", "info-clock-rate-v1"], [*store_flags, "--target-rule", "mv-aim-v1"]
    cases = [("compose-1", rate + mv), ("control-mv-only", mv), ("control-rate-only", rate)]
    if store_flags:
        cases.append(("control-two-stores", rate + mv + ["--risk-model", "build-equity/p9-gate-compose-nostore",
                                                          "--risk-model-sha256", "1" * 64]))
    for k, (tag, extra) in enumerate(cases, 1):
        out = REPO / "build-equity" / f"p9-gate-compose-nav-{k}"
        assert not out.exists(), out
        run(f"nav {tag}", base + ["--output", f"build-equity/p9-gate-compose-nav-{k}"] + extra)
        log(f"  output dir created: {out.exists()}")
    return 0


if __name__ == "__main__":
    verb = sys.argv[1]
    raise SystemExit({"world": world, "store": store, "nav": nav}[verb]())
