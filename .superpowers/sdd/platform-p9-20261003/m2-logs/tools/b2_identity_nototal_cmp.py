"""M2 blind compare: the no---dsr-total run vs root's M2f Edbg run with its total-count block removed (stdout: the
'== total-count DSR' header and the '   DSR_' lines after it; JSON: the deflated_total key); nav_summ_run popped;
ledger COPY path substituted. Prints line numbers / paths / counts only."""
import json, math, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from b2_identity_cmp import ROOT, subst, leaves  # noqa: E402
every = True
for pair in ("X5", "YF0"):
    lp = pair.lower()
    nd, od = ROOT / f"build-equity/p9-m2-b2-{lp}-nototal-run", ROOT / f"build-equity/p9-m2f-b2-{lp}-edbg-run"
    rc = json.loads((nd / "receipt.json").read_text(encoding="utf-8"))
    old, block, inb = [], 0, False
    for line in subst((od / "stdout.log").read_text(encoding="utf-8")).splitlines():
        if line.startswith("== total-count DSR"):
            inb, block = True, block + 1
            continue
        if inb and line.startswith("   DSR_"):
            block += 1
            continue
        inb = False
        old.append(line)
    new = (nd / "stdout.log").read_text(encoding="utf-8").splitlines()
    diff = [i + 1 for i, (a, b) in enumerate(zip(old, new)) if a != b]
    s_ok = old == new
    ea, eb = subst((od / "stderr.log").read_text(encoding="utf-8")), (nd / "stderr.log").read_text(encoding="utf-8")
    ja = json.loads((ROOT / f"build-equity/p9-m2f-b2-identity/{pair}-Edbg.json").read_text(encoding="utf-8"))
    jb = json.loads((ROOT / f"build-equity/p9-m2-b2-identity/{pair}-nototal.json").read_text(encoding="utf-8"))
    ja, jb = (ja if isinstance(ja, list) else [ja]), (jb if isinstance(jb, list) else [jb])
    for r in ja:
        r.pop("nav_summ_run", None), r.pop("deflated_total", None)
    for r in jb:
        r.pop("nav_summ_run", None)
    la, lb = dict(leaves(ja)), dict(leaves(jb))
    bad = sorted(p for p in set(la) | set(lb) if p not in la or p not in lb or
                 not (type(la[p]) is type(lb[p]) and (la[p] == lb[p] or subst(str(la[p])) == str(lb[p]) or
                      (isinstance(la[p], float) and math.isnan(la[p]) and math.isnan(lb[p])))))
    ok = rc.get("outcome") == "completed" and rc.get("exit_code") == 0 and s_ok and ea == eb and not bad
    print(f"== {pair} (no --dsr-total): run {rc.get('outcome')} exit {rc.get('exit_code')}; old block lines removed "
          f"{block}; stdout {len(old)} vs {len(new)} lines, {'EQUAL' if s_ok else f'DIFFER at lines {diff}'}; stderr "
          f"{'byte-equal' if ea == eb else 'DIFFER'}; json leaves {len(la)} vs {len(lb)}, "
          f"{'all equal' if not bad else f'DIFFER at {bad}'} -> {'HOLDS' if ok else 'DOES NOT HOLD'}")
    every &= ok
print("OVERALL", "HOLDS" if every else "DOES NOT HOLD")
