"""M2e C2 root item (i): the X-5 gm-pair identity, engine (nav --calibrate-gross) vs Python (two-process PM6-6).

usage:
  c2_gm_pair.py gparent              G_parent = wave_readers.mechanics(parent NAV)["mean_gross_leverage_all_rows"]
                                     (the reader the match stage runs as mech-parent); writes m2e-logs/c2-gm-gparent.json
                                     (repr only; nothing printed but the file name)
  c2_gm_pair.py run NAME L [--engine] [--dry]
                                     the reference receipt's argv (p9-c1-s2-x5-nav-run: X-5 NAV, L 1.1720) with
                                     exactly --output -> build-equity/NAME and --aim-leverage -> L substituted; --engine
                                     appends --calibrate-gross repr(G_parent) --calibrate-tolerance 0.005 (what
                                     wave_stage_cell.calibrated_doc adds); exe = build-equity/bin (p9-2d); through
                                     run_bounded_research.py, Debug, --seconds 600 --max-rss-mib 2048 (Ruling RUN-1)
  c2_gm_pair.py compare              the comparison of the written list (root-wave2-merge-report.md, M2e C2), by file
                                     SHA-256 and JSON path; gross values compared for bit equality, never printed;
                                     writes m2e-logs/c2-gm-pair-cmp.txt and exits 1 on any difference outside the list
Reads no return / NAV / Sharpe / turnover statistic: only mean_gross_leverage_all_rows (mechanics) and the
calibration record.
"""
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\atx-wt\pool-2")
BE = REPO / "build-equity"
LOGS = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "m2e-logs"
PY = r"C:\Program Files\Python312\python.exe"
REF = "p9-c1-s2-x5-nav"
PARENT = "build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b"
PARENT_SUMMARY_SHA = "e342db4faee76302c6953bf949ad55f3586442ea96600bd9379600519beb5bec"   # locked in x-theme-erc*.json
EXE = str(BE / "bin" / "atx-equity-strategy-targets.exe")
CAL, GM, ENG = "p9-m2e-c2-x5-cal", "p9-m2e-c2-x5-gm", "p9-m2e-c2-x5-engine"
L_PARENT, L_GM = "1.1414", "1.1720"
TOLERANCE = 0.005

sys.path.insert(0, str(REPO / "scripts"))
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
sys.dont_write_bytecode = True


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def mech_g(d: str) -> float:
    import wave_readers as WRD  # noqa: PLC0415
    return WRD.mechanics(REPO / d)["mean_gross_leverage_all_rows"]


def gparent():
    assert sha(REPO / PARENT / "summary.json") == PARENT_SUMMARY_SHA, "parent summary.json is not the locked one"
    g = mech_g(PARENT)
    out = LOGS / "c2-gm-gparent.json"
    out.write_text(json.dumps({"parent": PARENT, "summary_sha256": PARENT_SUMMARY_SHA,
                               "reader": "scripts/wave_readers.py mechanics", "g_parent_repr": repr(g)}, indent=1)
                   + "\n", encoding="utf-8")
    print("written", out.name)


def g_parent() -> float:
    return float(json.loads((LOGS / "c2-gm-gparent.json").read_text(encoding="utf-8"))["g_parent_repr"])


def run(name: str, lev: str, engine: bool, dry: bool):
    rec = json.loads((BE / f"{REF}-run" / "receipt.json").read_text(encoding="utf-8"))
    cmd = list(rec["command"])

    def sub(flag, value):
        idx = [i for i, t in enumerate(cmd) if t == flag]
        assert len(idx) == 1, (flag, idx)
        cmd[idx[0] + 1] = value

    sub("--output", f"build-equity/{name}")
    sub("--aim-leverage", lev)
    assert os.path.normcase(cmd[0]) == os.path.normcase(EXE), cmd[0]
    changed = [(i, a, b) for i, (a, b) in enumerate(zip(rec["command"], cmd)) if a != b]
    appended = ["--calibrate-gross", repr(g_parent()), "--calibrate-tolerance", repr(TOLERANCE)] if engine else []
    cmd += appended
    print("substituted:", changed, "appended:", appended[:1] + ["<repr G_parent>"] + appended[2:] if engine else [])
    env = dict(os.environ)
    vc = r"C:\atx-cache\vcpkg_installed\x64-windows"
    env["PATH"] = vc + r"\debug\bin;" + vc + r"\bin;" + env.get("PATH", "")
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    runner = [PY, "scripts/run_bounded_research.py", "--output", f"build-equity/{name}-run", "--seconds", "600",
              "--max-rss-mib", "2048", "--min-free-mib", str(rec["limits"]["min_free_mib"]), "--build-type", "Debug",
              "--admission-wait-seconds", "3600", "--"] + cmd
    if dry:
        return 0
    return subprocess.run(runner, cwd=REPO, env=env).returncode


def files(d: Path) -> dict:
    return {p.relative_to(d).as_posix(): sha(p) for p in sorted(d.rglob("*")) if p.is_file()}


def leaves(x, path="") -> dict:
    if isinstance(x, dict):
        out = {}
        for k, v in x.items():
            out.update(leaves(v, f"{path}/{k}"))
        return out or {path: {}}
    if isinstance(x, list):
        out = {}
        for i, v in enumerate(x):
            out.update(leaves(v, f"{path}/{i}"))
        return out or {path: []}
    return {path: x}


def jdiff(a: Path, b: Path) -> list[str]:
    la = leaves(json.loads(a.read_text(encoding="utf-8")))
    lb = leaves(json.loads(b.read_text(encoding="utf-8")))
    out = []
    for k in sorted(set(la) | set(lb)):
        if k not in lb:
            out.append(f"REMOVED {k}")
        elif k not in la:
            out.append(f"ADDED {k}")
        elif la[k] != lb[k] or type(la[k]) is not type(lb[k]):
            out.append(f"CHANGED {k}")
    return out


def compare():
    import wave_rules as WR  # noqa: PLC0415
    lines, bad = [], []

    def say(s):
        lines.append(s)

    def check(ok, what):
        say(f"{'yes' if ok else 'NO '} {what}")
        if not ok:
            bad.append(what)

    ref, cal, gm, eng = BE / REF, BE / CAL, BE / GM, BE / ENG
    for d in (cal, gm, eng):
        r = json.loads((BE / f"{d.name}-run" / "receipt.json").read_text(encoding="utf-8"))
        say(f"run {d.name}: outcome {r.get('outcome')} exit {r.get('exit_code')} wall {r.get('wall_seconds')} s "
            f"peak {round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20)} MiB exe {r.get('executable_sha256', '')[:8]} "
            f"argv {r.get('argv_sha256', '')[:8]} source {r.get('source_sha', '')[:8]} git {r.get('git')}")
        check(r.get("outcome") == "completed" and r.get("exit_code") == 0, f"{d.name} completed, exit 0")
    # 1. flag-absent: P2 (the -gm run on the merged exe) vs the Debug X-5 reference since C1
    fr, fg = files(ref), files(gm)
    check(set(fr) == set(fg), f"1 {GM} vs {REF}: same file set ({len(fg)} files)")
    other = sorted(k for k in fr if k != "summary.json" and fr.get(k) != fg.get(k))
    check(not other, f"1 {GM} vs {REF}: every file but summary.json byte-identical ({len(fr) - 1}); differing: {other}")
    d1 = jdiff(ref / "summary.json", gm / "summary.json")
    say(f"  summary.json paths differing: {d1}")
    check(set(d1) <= {"CHANGED /producer/engine_git_sha"}, "1 summary.json differs only at /producer/engine_git_sha")
    # 2. the Python rule on P1 / P2
    gp = g_parent()
    gcal, g2 = mech_g(f"build-equity/{CAL}"), mech_g(f"build-equity/{GM}")
    l_py = WR.matched_leverage(L_PARENT, gp, gcal)
    check(not WR.gross_matches("pm6-6", gp, gcal), "2 Python: calibration at L 1.1414 is outside .005 (corrected)")
    check(l_py == L_GM, f"2 Python: L' = matched_leverage(1.1414, G_P, G_cal) = {l_py} (registered -gm L {L_GM})")
    check(WR.gross_matches("pm6-6", gp, g2), "2 Python: the -gm run at L' is within .005 of G_parent (matched)")
    # 3. engine vs P2
    fe = files(eng)
    check(set(fe) == set(fg), f"3 {ENG} vs {GM}: same file set")
    other = sorted(k for k in fg if k != "summary.json" and fg.get(k) != fe.get(k))
    check(not other, f"3 {ENG} vs {GM}: every file but summary.json byte-identical ({len(fg) - 1}); differing: {other}")
    d3 = jdiff(gm / "summary.json", eng / "summary.json")
    outside = [x for x in d3 if not x.startswith("ADDED /calibration/")]
    say(f"  summary.json paths differing: {d3}")
    check(not outside, "3 summary.json differs only by the added /calibration")
    # 4. the calibration record
    c = json.loads((eng / "summary.json").read_text(encoding="utf-8")).get("calibration") or {}
    keys = ("target", "tolerance", "leverage_requested", "gross_requested", "corrected", "leverage", "gross",
            "matched", "construction_reused", "rule")
    check(sorted(c) == sorted(keys), f"4 calibration keys = {sorted(keys)}")
    check(c.get("target") == gp, "4 target == G_parent (bit)")
    check(c.get("tolerance") == TOLERANCE, "4 tolerance == 0.005")
    check(c.get("leverage_requested") == float(L_PARENT), "4 leverage_requested == 1.1414")
    check(c.get("gross_requested") == gcal, "4 gross_requested == Python G_cal (mechanics of the L 1.1414 run, bit)")
    check(c.get("corrected") is True, "4 corrected true (Python: corrected)")
    lev = c.get("leverage")
    rel = abs(lev - float(l_py)) / float(l_py) if isinstance(lev, float) else None
    check(rel is not None and rel <= 1e-12, f"4 leverage == float(Python L') within 1e-12 relative (rel {rel})")
    check(c.get("gross") == g2, "4 gross == Python G of the -gm run (bit)")
    check(c.get("gross") == mech_g(f"build-equity/{ENG}"), "4 gross == mechanics G of the engine NAV (bit)")
    check(c.get("matched") is True, "4 matched true (Python: matched)")
    say(f"  construction_reused = {c.get('construction_reused')} (read, not compared)")
    say(f"  rule = {c.get('rule')!r}")
    say(f"RESULT: {'HOLDS' if not bad else 'MISS: ' + '; '.join(bad)}")
    out = LOGS / "c2-gm-pair-cmp.txt"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0 if not bad else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "gparent":
        gparent()
    elif a[0] == "run":
        sys.exit(run(a[1], a[2], "--engine" in a, "--dry" in a))
    elif a[0] == "compare":
        sys.exit(compare())
