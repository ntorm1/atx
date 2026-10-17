"""SIGN identity on the ledgered wave results (ruling C3-11): root's step, BEFORE root merges SIGN's deletion commit.

For every wave-result.json given, its screen rows (screen.rows: the {id, kind, prior_sign, status, runner_sign} the
Python rule decided, with the recorded decision and reason) go to the IC exe's sign verb as they stand (the verb
ignores the recorded keys), and each row's (verdict, reason) is compared with the recorded (decision, reason) and,
while scripts/wave_rules.py still defines it, with wave_rules.sign_pm7_35 on the same row; kept / dropped are compared
with the recorded lists. A rule wave (screen null) is listed and skipped. Prints counts and ids only (no statistic is
in these rows; nothing else of the wave result is read). Exit 0 iff every row of every screen is identical.

  python sign-identity-ledgered.py --root <tree> --exe <tree>/build-equity/bin/atx-equity-strategy-ic.exe \
      --work <new dir> <tree>/.superpowers/sdd/platform-v8-20260929/waves/{y-2,y-3,y-5,y-s}/wave-result.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True, help="the tree whose scripts/wave_rules.py is compared")
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True, help="a new directory for the rows files and receipts")
    ap.add_argument("results", type=Path, nargs="+")
    a = ap.parse_args()
    sys.path.insert(0, str(a.root / "scripts"))
    import wave_rules as WR                              # noqa: PLC0415  (the tree's own module)
    python_rule = getattr(WR, "sign_pm7_35", None)
    a.work.mkdir(parents=True, exist_ok=False)
    print(f"exe {a.exe} sha256 {sha(a.exe.read_bytes())}; python rule: "
          f"{'wave_rules.sign_pm7_35' if python_rule else 'retired (recorded decisions only)'}")
    ok = True
    for k, path in enumerate(a.results, 1):
        doc = json.loads(path.read_text(encoding="utf-8"))
        screen = doc.get("screen")
        if not screen:
            print(f"{path}: wave {doc.get('wave')}: no screen (a rule wave): skipped")
            continue
        rows = screen["rows"]
        rows_path, receipt_path = a.work / f"rows-{k}.json", a.work / f"receipt-{k}.json"
        data = (json.dumps({"rows": rows}, indent=2) + "\n").encode("utf-8")
        rows_path.write_bytes(data)
        done = subprocess.run([str(a.exe), "sign", "--rule", screen["sign_rule"], "--rows", str(rows_path),
                               "--output", str(receipt_path)], capture_output=True, text=True, timeout=120)
        if done.returncode != 0:
            ok = False
            print(f"{path}: wave {doc.get('wave')}: the verb refused (exit {done.returncode}): {done.stderr.strip()}")
            continue
        got = json.loads(receipt_path.read_text(encoding="utf-8"))
        same_rec = [r["id"] for r, g in zip(rows, got["rows"])
                    if (g["verdict"], g["reason"]) == (r["decision"], r["reason"])]
        same_py = [r["id"] for r, g in zip(rows, got["rows"]) if python_rule is not None and
                   (g["verdict"], g["reason"]) == python_rule(r, r["kind"], int(r["prior_sign"]))]
        lists = got["kept"] == screen["kept"] and got["dropped"] == screen["dropped"]
        pinned = got["rows_sha256"] == sha(data) and len(got["rows"]) == len(rows)
        wave_ok = len(same_rec) == len(rows) and lists and pinned and \
            (python_rule is None or len(same_py) == len(rows))
        ok &= wave_ok
        print(f"{path}: wave {doc.get('wave')}, rule {screen['sign_rule']}: {len(rows)} rows; verb == recorded "
              f"{len(same_rec)} / {len(rows)}; verb == python "
              f"{f'{len(same_py)} / {len(rows)}' if python_rule else 'n/a'}; kept / dropped equal {lists}; "
              f"receipt pins the rows {pinned}; {'IDENTICAL' if wave_ok else 'DIFFERS'}")
        if not wave_ok:
            print(f"   differing ids: {[r['id'] for r in rows if r['id'] not in same_rec]}")
    print(f"RESULT {'PASS: every screen row identical' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
