"""Root M2h finish: the evidence lines of the tiny-world waves (m2h2-engine-o1.py): match / verify receipts, the run
recipe, the cell of record and the cell spec resolved, the --aim-leverage of every NAV and ref NAV that ran (their
bounded-runner receipts) and of the cell of record's planned NAV argv (research_cycle.py plan). Read-only.

  python m2h2-report.py LABEL [LABEL ...]
"""
from __future__ import annotations

import glob
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[4]
PY = Path(sys.executable).as_posix()
TDIR = "scripts/specs/p9/kinds/templates"
MATCH = ("--calibrate-gross", "--calibrate-tolerance")


def flag(argv: list[str], name: str):
    return argv[argv.index(name) + 1] if name in argv else None


def receipt(root: Path, rel: str):
    p = root / rel
    return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None


def resolved(root: Path, rel: str) -> dict:
    sys.path.insert(0, str(root / "scripts"))
    import research_cycle as RC  # noqa: PLC0415  (the world's byte copy of HEAD's scripts)
    return RC.load_spec(root / rel)


def report(label: str) -> None:
    root = REPO / "build-equity" / f"p9-m2h-tw-{label}"
    wdir = root / "waves" / f"m2h-{label}" / "receipts"
    print(f"===== wave {label} (root {root.name})")
    rec = sorted(p.name for p in wdir.glob("*.json"))
    print("receipts:", rec)
    m = receipt(wdir, "06-match.json")
    if m:
        o = m["outputs"]
        keys = ("mode", "tolerance", "g_parent", "g_calibration", "leverage_calibration", "corrected", "leverage",
                "g_cell", "cell_spec", "record_spec", "record_spec_digest", "engine")
        print("match.outputs:", json.dumps({k: o.get(k) for k in keys}))
        if o.get("calibration"):
            print("match.outputs.calibration (the exe's summary.json record):", json.dumps(o["calibration"]))
            print("leverage check (:374): leverage_requested", o["calibration"]["leverage_requested"], "== cell L",
                  o["leverage_calibration"], "->", o["calibration"]["leverage_requested"] == float(o["leverage_calibration"]),
                  "; target == G_parent ->", o["calibration"]["target"] == o["g_parent"])
            print("mechanics G bit-equal: reader g_cell", repr(o["g_cell"]), "exe calibration.gross",
                  repr(o["calibration"]["gross"]), "->", o["g_cell"] == o["calibration"]["gross"])
    v = receipt(wdir, "07-verify.json")
    if v:
        print("verify.inputs match.record_spec_digest:", v["inputs"].get("match.record_spec_digest"),
              "== match.outputs.record_spec_digest ->",
              m is not None and v["inputs"].get("match.record_spec_digest") == m["outputs"].get("record_spec_digest"))
    for p in sorted(wdir.glob("*.failed-*.json")):
        f = json.loads(p.read_text(encoding="utf-8"))
        print(f"failed receipt {p.name}: {str(f.get('error') or f.get('outputs') or '')[:400]}")
    for spec in sorted(glob.glob(str(root / TDIR / f"*-m2h-{label}*.json"))):
        rel = Path(spec).relative_to(root).as_posix()
        s = resolved(root, rel)
        nav, ref = s["nav"], s.get("ref")
        eff = None if ref is None else ref.get("leverage", nav["leverage"])
        print(f"spec {Path(spec).name}: nav.leverage {nav['leverage']}; nav.output {nav['output']}; match flags on "
              f"nav.flags {[f for f in nav['flags'] if f in MATCH]}; ref "
              f"{None if ref is None else {k: ref.get(k) for k in ('output', 'leverage')}}; ref has own flags "
              f"{None if ref is None else 'flags' in ref}; EFFECTIVE ref leverage {eff}")
    for run in sorted(glob.glob(str(root / "tiny-nav-*-run*")) + glob.glob(str(root / "tiny-ref-*-run*"))):
        r = receipt(Path(run), "receipt.json")
        if not r:
            continue
        argv = r["command"]
        print(f"ran {Path(run).name}: outcome {r.get('outcome')}; --aim-leverage {flag(argv, '--aim-leverage')}; "
              f"--trade-fraction {flag(argv, '--trade-fraction')}; --calibrate-gross {flag(argv, '--calibrate-gross')}; "
              f"--calibrate-tolerance {flag(argv, '--calibrate-tolerance')}; exe {r.get('executable_sha256', '')[:8]}")
    for nav_dir in sorted(glob.glob(str(root / "tiny-nav-*"))):
        summ = Path(nav_dir) / "summary.json"
        if summ.is_file() and "calibration" in json.loads(summ.read_text(encoding="utf-8")):
            print(f"{Path(nav_dir).name}/summary.json calibration:",
                  json.dumps(json.loads(summ.read_text(encoding="utf-8"))["calibration"]))
    if m and m["outputs"].get("record_spec"):
        rel = m["outputs"]["record_spec"]
        done = subprocess.run([PY, "scripts/research_cycle.py", "plan", rel, "--root", str(root)], cwd=root,
                              capture_output=True, text=True)
        for line in done.stdout.splitlines():
            if " nav " in line and "--combined" in line:
                argv = line.split()
                print(f"cell of record {Path(rel).name} planned NAV argv: --aim-leverage "
                      f"{flag(argv, '--aim-leverage')}; --trade-fraction {flag(argv, '--trade-fraction')}; match flags "
                      f"{[f for f in argv if f.strip(chr(34)) in MATCH]}")
            if line.startswith("== ") or "done" in line[:40]:
                print("   plan:", line[:160])
        print(f"plan exit {done.returncode}")


if __name__ == "__main__":
    for lab in sys.argv[1:]:
        report(lab)
