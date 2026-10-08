"""M2 (RESUME 9) B2 TRAIN identity comparator, blind: prints line numbers, JSON paths, counts and match / miss only,
never a value (the PM's M2 dispatch: READ NO STATISTIC; report "equal / differ at <paths>").

Per pair (X5, YF0): new = build-equity/p9-m2-b2-<pair>-run (merged head, Debug verb p9-2t), JSON
build-equity/p9-m2-b2-identity/<PAIR>.json; old = root's M2f Edbg run at c2416d62 (Debug verb p9-2i):
build-equity/p9-m2f-b2-<pair>-edbg-run, JSON build-equity/p9-m2f-b2-identity/<PAIR>-Edbg.json.
Substitution list: the ledger COPY path (old -> new; both slash forms) and nav_summ_run (popped).
  - stdout: after the substitution, equal except ONE added line per listed dir that starts
    "   DSR_hand (N_tot - M=" and names "ruling PRE-15";
  - stderr: byte-equal after the substitution;
  - JSON: rows with nav_summ_run popped and deflated_total.tot_minus_m popped from new: leaf paths equal; every leaf
    byte-equal (floats bit-equal), else within 1e-12 relative (B2-R1); differing paths listed (path only);
  - new nav_summ_run.engine_exe_sha256 = the p9-2t receipt's verb SHA-256 (EXE-CUR) = the file's;
  - PRE-15 public cross-check (B2 report "State for root" item 2): YF0 tot_minus_m n == 123 and its DSR, printed to
    4 places as nav_summ prints it, equals R0-12's logged DSR_hand at N 123 (the B2 report's stated value);
    prints match / miss only.
"""
from __future__ import annotations

import difflib
import hashlib
import json
import math
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-2")
OLD_LEDGER = ("C:/Users/natha/AppData/Local/Temp/claude/c--atx/6ea15be9-c1e8-4316-99d4-4854dfd7c03c/scratchpad/"
              "m2f-ledger-copy/trials.jsonl")
NEW_LEDGER = ("C:/Users/natha/AppData/Local/Temp/claude/c--atx/a3de34c1-d7ad-4b5c-9079-b9c90532d424/scratchpad/"
              "root-m2/ledger-copy/trials.jsonl")
SUBS = [(OLD_LEDGER, NEW_LEDGER), (OLD_LEDGER.replace("/", "\\"), NEW_LEDGER.replace("/", "\\"))]
RECEIPT = ROOT / "build-equity" / "mega-p9-2t-receipt.json"
R0_12_DSR_HAND_N123 = "0.7589"   # B2 report "State for root" item 2 (R0-12's logged DSR_hand .7589 at N 123)


def subst(text: str) -> str:
    for a, b in SUBS:
        text = text.replace(a, b)
    return text


def leaves(x, path=""):
    if isinstance(x, dict):
        for k, v in x.items():
            yield from leaves(v, f"{path}.{k}")
    elif isinstance(x, list):
        for i, v in enumerate(x):
            yield from leaves(v, f"{path}[{i}]")
    else:
        yield path, x


def rows_of(p: Path, pop_tmm: bool):
    doc = json.loads(p.read_text(encoding="utf-8"))
    rows = doc if isinstance(doc, list) else [doc]
    runs, tmms = [], []
    for r in rows:
        runs.append(r.pop("nav_summ_run", None))
        dt = r.get("deflated_total")
        if pop_tmm and isinstance(dt, dict) and "tot_minus_m" in dt:
            tmms.append(dt.pop("tot_minus_m"))
    return rows, runs, tmms


def main() -> int:
    every = True
    verb_sha = json.loads(RECEIPT.read_text(encoding="utf-8-sig"))["Executables"]["atx-research-eval"]["Sha256"]
    for pair in ("X5", "YF0"):
        lp = pair.lower()
        nd, od = ROOT / f"build-equity/p9-m2-b2-{lp}-run", ROOT / f"build-equity/p9-m2f-b2-{lp}-edbg-run"
        rc = json.loads((nd / "receipt.json").read_text(encoding="utf-8"))
        print(f"== {pair}: run outcome {rc.get('outcome')} exit {rc.get('exit_code')}")
        ok = rc.get("outcome") == "completed" and rc.get("exit_code") == 0
        # stdout
        a = subst((od / "stdout.log").read_text(encoding="utf-8")).splitlines()
        b = (nd / "stdout.log").read_text(encoding="utf-8").splitlines()
        added, other = [], []
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
            if tag == "equal":
                continue
            if tag == "insert" and all(b[j].startswith("   DSR_hand (N_tot - M=") and "ruling PRE-15" in b[j]
                                       for j in range(j1, j2)):
                added += [j + 1 for j in range(j1, j2)]
            else:
                other.append(f"{tag} old lines {i1 + 1}-{i2} / new lines {j1 + 1}-{j2}")
        s_ok = len(added) == 1 and not other
        print(f"  stdout: {len(a)} vs {len(b)} lines; added PRE-15 line(s) at new line {added}; "
              f"{'EQUAL except the one PRE-15 line' if s_ok else 'DIFFER at ' + '; '.join(other or ['count'])}")
        ok &= s_ok
        # stderr
        ea = subst((od / "stderr.log").read_text(encoding="utf-8"))
        eb = (nd / "stderr.log").read_text(encoding="utf-8")
        print(f"  stderr: {'byte-equal' if ea == eb else 'DIFFER'} ({len(ea)} vs {len(eb)} chars)")
        ok &= ea == eb
        # json
        ra, _, _ = rows_of(ROOT / f"build-equity/p9-m2f-b2-identity/{pair}-Edbg.json", False)
        rb, runs_b, tmms = rows_of(ROOT / f"build-equity/p9-m2-b2-identity/{pair}.json", True)
        la, lb = dict(leaves(ra)), dict(leaves(rb))
        only_a, only_b = sorted(set(la) - set(lb)), sorted(set(lb) - set(la))
        bit, near, far, subd = 0, [], [], 0
        for p in sorted(set(la) & set(lb)):
            x, y = la[p], lb[p]
            if isinstance(x, str) and isinstance(y, str) and subst(x) == y and x != y:
                subd += 1
            elif type(x) is type(y) and (x == y or (isinstance(x, float) and math.isnan(x) and math.isnan(y))):
                bit += 1
            elif isinstance(x, float) and isinstance(y, float) and math.isclose(x, y, rel_tol=1e-12, abs_tol=1e-300):
                near.append(p)
            else:
                far.append(p)
        j_ok = not only_a and not only_b and not far
        print(f"  json: rows {len(ra)} vs {len(rb)}; tot_minus_m popped from {len(tmms)} row(s); leaves equal {bit}, "
              f"ledger-path substituted {subd}, within 1e-12 not bit-equal {len(near)} {near}, "
              f"DIFFER {far}; only old {only_a}; only new {only_b}")
        ok &= j_ok and len(tmms) >= 1
        # EXE-CUR: the verb named by the run
        for r in runs_b:
            exe = (r or {}).get("engine_exe")
            real = hashlib.sha256(Path(exe).read_bytes()).hexdigest() if exe and Path(exe).is_file() else None
            same = (r or {}).get("engine_exe_sha256") == real == verb_sha
            print(f"  nav_summ_run names verb sha256 {str((r or {}).get('engine_exe_sha256'))[:16]} "
                  f"= file = receipt p9-2t: {same}")
            ok &= same
        if pair == "YF0":
            t = tmms[0] if tmms else {}
            hit = t.get("n") == 123 and isinstance(t.get("dsr"), float) and f"{t['dsr']:.4f}" == R0_12_DSR_HAND_N123
            print(f"  PRE-15 public cross-check (n == 123 and DSR = R0-12 logged DSR_hand at N 123): "
                  f"{'match' if hit else 'MISS (stop for the PM)'}")
            ok &= hit
        print(f"  RESULT {pair}: {'HOLDS' if ok else 'DOES NOT HOLD'}")
        every &= ok
    print(f"\nOVERALL {'HOLDS' if every else 'DOES NOT HOLD'}")
    return 0 if every else 1


if __name__ == "__main__":
    raise SystemExit(main())
