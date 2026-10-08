"""M2 blind diagnosis of test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced on the merged head: replicate the test
(same argv swap, same ledger copy rule, chdir to pool-2), discard nav_summ's stdout, print only argv flag names, the
exit / exception type, and the JSON paths that differ (counts within / beyond 1e-12 relative), never a value."""
from __future__ import annotations

import contextlib
import io
import json
import math
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-2")
sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else str(ROOT / 'atx-impl' / 'tools'))
os.environ.pop("ATX_EQUITY_BIN", None)
import nav_summ as NS  # noqa: E402


def leaves(x, path=""):
    if isinstance(x, dict):
        for k, v in x.items():
            yield from leaves(v, f"{path}.{k}")
    elif isinstance(x, list):
        for i, v in enumerate(x):
            yield from leaves(v, f"{path}[{i}]")
    else:
        yield path, x


def generic(p: str) -> str:
    import re
    return re.sub(r"^\[\d+\]", "[*]", p)


def compare(name, got, want):
    la, lb = dict(leaves(got)), dict(leaves(want))
    only_g, only_w = sorted(set(la) - set(lb)), sorted(set(lb) - set(la))
    near, far, other = {}, {}, {}
    for p in set(la) & set(lb):
        x, y = la[p], lb[p]
        if type(x) is type(y) and (x == y or (isinstance(x, float) and math.isnan(x) and math.isnan(y))):
            continue
        if isinstance(x, (int, float)) and isinstance(y, (int, float)) and not isinstance(x, bool):
            d = near if math.isclose(x, y, rel_tol=1e-12, abs_tol=1e-300) else far
        else:
            d = other
        g = generic(p)
        d[g] = d.get(g, 0) + 1
    print(f"  {name}: leaves {len(la)} vs {len(lb)}; only got {len(only_g)} {sorted({generic(p) for p in only_g})[:20]}; "
          f"only want {len(only_w)} {sorted({generic(p) for p in only_w})[:20]}")
    print(f"  {name}: numeric within 1e-12 (not equal) by generic path: {dict(sorted(near.items()))}")
    print(f"  {name}: numeric BEYOND 1e-12 by generic path: {dict(sorted(far.items()))}")
    print(f"  {name}: non-numeric differing by generic path: {dict(sorted(other.items()))}")


def main() -> int:
    equity = ROOT / "build-equity"
    summ, pbo = equity / "mega-nav-v71-summ-n37.json", equity / "mega-nav-v71-pbo-n37.json"
    want_rows = json.loads(summ.read_text(encoding="utf-8"))
    argv = list(want_rows[0]["nav_summ_run"]["argv"])
    print("argv flags:", [t for t in argv if isinstance(t, str) and t.startswith("--")])
    print("argv tokens:", len(argv), "| non-flag tokens:", sum(1 for t in argv if not str(t).startswith("--")))
    added = {r["ledger"]["trial_id"] for r in want_rows if r.get("ledger", {}).get("appended")}
    tmp = Path(tempfile.mkdtemp(prefix="n37diag-", dir="C:/Users/natha/AppData/Local/Temp/claude/c--atx/"
                                                     "a3de34c1-d7ad-4b5c-9079-b9c90532d424/scratchpad/root-m2/pt"))
    swap = {"--ledger": str(tmp / "trials.jsonl"), "--ledger-n": str(tmp / "trials.jsonl"),
            "--json": str(tmp / "summ.json"), "--pbo-json": str(tmp / "pbo.json")}
    if "--ledger" in argv:
        lines = (equity.parent / argv[argv.index("--ledger") + 1]).read_text(encoding="utf-8").splitlines()
        (tmp / "trials.jsonl").write_text("".join(l + "\n" for l in lines
                                                  if l.strip() and json.loads(l).get("trial_id") not in added),
                                          encoding="utf-8")
    for flag, value in swap.items():
        if flag in argv:
            argv[argv.index(flag) + 1] = value
    os.chdir(equity.parent)
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = NS.main(argv)
        print("NS.main returned", rc)
    except SystemExit as e:
        print("NS.main SystemExit code type:", type(e.code).__name__, "| int code:", e.code if isinstance(e.code, int)
              else None)
        msg = str(e.code) if not isinstance(e.code, int) else ""
        print("  SystemExit message words (letters only, first 30):", " ".join(w for w in msg.split() if w.isalpha())[:400])
        print("  stderr words (letters only):", " ".join(w for w in err.getvalue().split() if w.isalpha())[:600])
        return 1
    except Exception as e:  # noqa: BLE001
        print("NS.main raised", type(e).__name__, getattr(e, "filename", ""))
        return 1
    got_rows = json.loads((tmp / "summ.json").read_text(encoding="utf-8"))
    print("rows got / want:", len(got_rows), len(want_rows))
    for g, w in zip(got_rows, want_rows):
        g.pop("nav_summ_run", None), w.pop("nav_summ_run", None)
        if "ledger" in g:
            g["ledger"]["path"] = w["ledger"]["path"]
    compare("summ", got_rows, want_rows)
    if "--pbo-json" in argv and pbo.exists():
        gp, wp = json.loads((tmp / "pbo.json").read_text(encoding="utf-8")), json.loads(pbo.read_text(encoding="utf-8"))
        gp.pop("nav_summ_run", None), wp.pop("nav_summ_run", None)
        compare("pbo", gp, wp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
