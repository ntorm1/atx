"""B2 fix round 2 (rulings B2-N37, B2-FIX2-EXE): blind diagnosis of
atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced from pool-14.

The form of root's m2-logs/tools/n37_diag.py (copy: r10-n37_diag-root-m2.py): the test's argv swap, ledger copy rule
and chdir to the parent of ATX_EQUITY_ROOT; nav_summ's stdout / stderr are discarded. Printed: argv flag names, exit
types, counts, generic JSON paths, booleans, digit-count histograms, and the max |relative difference| / max ulp
distance between got and want leaves (noise magnitudes). Never a value of a row, the golden or the ledger.

Inputs: ATX_EQUITY_ROOT (pool-2's build-equity; read only), ATX_EQUITY_BIN (the lane's own Debug bin; required, kept
in the environment so nav_summ resolves the verb there), argv[1] the tools dir to import (pool-14's atx-impl/tools),
argv[2] the temp parent (the lane's scratch). Writes only under argv[2].

Also printed:
  (1) formatting check (cause (a)): every float token of the verb's raw JSON stdout against Python's shortest repr of
      the same double (equal text / equal digits / longer / shorter), and a histogram of significant-digit counts;
  (2) the test's own comparison helpers (numeric_mismatches, take_degenerate_disclosure) on the real rows when the
      tools dir has them, then on in-memory perturbed copies (teeth; no file is edited).
"""
from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import math
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

TOOLS = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("C:/atx-wt/pool-14/atx-impl/tools")
TMP_PARENT = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(tempfile.gettempdir())
EQUITY = os.environ.get("ATX_EQUITY_ROOT")
BIN = os.environ.get("ATX_EQUITY_BIN")
if not EQUITY or not BIN:
    raise SystemExit("set ATX_EQUITY_ROOT and ATX_EQUITY_BIN")
sys.path.insert(0, str(TOOLS))
import nav_summ as NS  # noqa: E402

REAL_JSON = json
TOK = {"floats": 0, "text_equal_repr": 0, "digits_equal_repr": 0, "longer_than_repr": 0, "shorter_than_repr": 0,
       "documents": 0}
SIGDIG: dict[int, int] = {}


def digits(tok: str) -> str:
    mant = re.split(r"[eE]", tok.lstrip("+-"))[0].replace(".", "")
    return mant.lstrip("0").rstrip("0") or "0"


def check_float(tok: str) -> float:
    v = float(tok)
    r = repr(v)
    TOK["floats"] += 1
    TOK["text_equal_repr"] += tok == r
    dt, dr = digits(tok), digits(r)
    TOK["digits_equal_repr"] += dt == dr
    TOK["longer_than_repr"] += len(dt) > len(dr)
    TOK["shorter_than_repr"] += len(dt) < len(dr)
    SIGDIG[len(dt)] = SIGDIG.get(len(dt), 0) + 1
    return v


class JsonShim:
    """nav_summ's ``json`` while EngineStats.run runs: its one json.loads is the verb's stdout."""

    def __getattr__(self, name):
        return getattr(REAL_JSON, name)

    @staticmethod
    def loads(s, **kw):
        TOK["documents"] += 1
        REAL_JSON.loads(s, parse_float=check_float)
        return REAL_JSON.loads(s, **kw)


_orig_run = NS.EngineStats.run


def _run(self, cells, *extra):
    NS.json = JsonShim()
    try:
        return _orig_run(self, cells, *extra)
    finally:
        NS.json = REAL_JSON


NS.EngineStats.run = _run


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
    return re.sub(r"^\[\d+\]", "[*]", p)


def ordered(x: float) -> int:
    i = struct.unpack("<q", struct.pack("<d", x))[0]
    return i if i >= 0 else -(i & 0x7FFFFFFFFFFFFFFF)


def compare(name, got, want):
    la, lb = dict(leaves(got)), dict(leaves(want))
    only_g, only_w = sorted(set(la) - set(lb)), sorted(set(lb) - set(la))
    near, far, other, typed = {}, {}, {}, {}
    max_rel, max_rel_at, max_ulp, max_ulp_at, equal = 0.0, None, 0, None, 0
    for p in set(la) & set(lb):
        x, y = la[p], lb[p]
        if type(x) is type(y) and (x == y or (isinstance(x, float) and math.isnan(x) and math.isnan(y))):
            equal += 1
            continue
        g = generic(p)
        if isinstance(x, (int, float)) and isinstance(y, (int, float)) and not isinstance(x, bool) \
                and not isinstance(y, bool):
            if type(x) is not type(y):
                typed[g] = typed.get(g, 0) + 1
            rel = abs(x - y) / max(abs(x), abs(y))
            if rel > max_rel:
                max_rel, max_rel_at = rel, g
            if isinstance(x, float) and isinstance(y, float) and math.isfinite(x) and math.isfinite(y):
                u = abs(ordered(x) - ordered(y))
                if u > max_ulp:
                    max_ulp, max_ulp_at = u, g
            d = near if math.isclose(x, y, rel_tol=1e-12, abs_tol=0.0) else far
        else:
            d = other
        d[g] = d.get(g, 0) + 1
    print(f"  {name}: leaves {len(la)} vs {len(lb)}; bit-equal {equal}; only got {len(only_g)} "
          f"{sorted({generic(p) for p in only_g})[:20]}; only want {len(only_w)} {sorted({generic(p) for p in only_w})[:20]}")
    print(f"  {name}: numeric within 1e-12 relative (abs_tol 0; not equal), leaves {sum(near.values())}, by generic path: "
          f"{dict(sorted(near.items()))}")
    print(f"  {name}: numeric BEYOND 1e-12 relative by generic path: {dict(sorted(far.items()))}")
    print(f"  {name}: int / float type mismatch by generic path: {dict(sorted(typed.items()))}")
    print(f"  {name}: non-numeric differing by generic path: {dict(sorted(other.items()))}")
    print(f"  {name}: max |relative difference| {max_rel:.3e} at {max_rel_at}; max ulp distance {max_ulp} at {max_ulp_at}")
    return max_rel


def main() -> int:
    equity = Path(EQUITY).resolve()
    exe = Path(BIN) / "atx-research-eval.exe"
    print("tools dir:", TOOLS, "| ATX_EQUITY_BIN:", BIN, "| verb exists:", exe.is_file())
    print("verb sha256:", hashlib.sha256(exe.read_bytes()).hexdigest() if exe.is_file() else None)
    print("nav_summ default verb resolves to the lane's bin:",
          NS.default_engine_exe().resolve() == exe.resolve())
    summ, pbo = equity / "mega-nav-v71-summ-n37.json", equity / "mega-nav-v71-pbo-n37.json"
    want_text = summ.read_text(encoding="utf-8")
    want_rows = json.loads(want_text)
    print("golden is the canonical dump:", json.dumps(want_rows, indent=2, sort_keys=True) + "\n" == want_text)
    argv = list(want_rows[0]["nav_summ_run"]["argv"])
    print("argv flags:", [t for t in argv if isinstance(t, str) and t.startswith("--")])
    print("argv tokens:", len(argv), "| non-flag tokens:", sum(1 for t in argv if not str(t).startswith("--")))
    ref = argv[argv.index("--reference") + 1] if "--reference" in argv else None
    print("reference token occurs in argv more than once (also a dir):", ref is not None and argv.count(ref) > 1)
    added = {r["ledger"]["trial_id"] for r in want_rows if r.get("ledger", {}).get("appended")}
    TMP_PARENT.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix="n37diag-", dir=str(TMP_PARENT)))
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
        print("NS.main SystemExit code type:", type(e.code).__name__, "| int code:",
              e.code if isinstance(e.code, int) else None)
        return 1
    except Exception as e:  # noqa: BLE001
        print("NS.main raised", type(e).__name__, getattr(e, "filename", ""))
        return 1
    print("verb documents read:", TOK["documents"], "| float tokens:", TOK["floats"])
    print("formatting: token text == Python repr:", TOK["text_equal_repr"], "| digits equal:", TOK["digits_equal_repr"],
          "| longer than repr:", TOK["longer_than_repr"], "| shorter than repr:", TOK["shorter_than_repr"])
    print("formatting: significant-digit histogram of the verb's float tokens:", dict(sorted(SIGDIG.items())))
    got_rows = json.loads((tmp / "summ.json").read_text(encoding="utf-8"))
    print("rows got / want:", len(got_rows), len(want_rows))
    for g, w in zip(got_rows, want_rows):
        g.pop("nav_summ_run", None), w.pop("nav_summ_run", None)
        if "ledger" in g:
            g["ledger"]["path"] = w["ledger"]["path"]
    deg = [i for i, g in enumerate(got_rows) if isinstance(g.get("paired"), dict) and g["paired"].get("degenerate")]
    print("degenerate rows (got):", len(deg), "| want rows with a degenerate key:",
          sum(1 for w in want_rows if isinstance(w.get("paired"), dict) and "degenerate" in w["paired"]))
    for i in deg:
        gp, wp = got_rows[i]["paired"], want_rows[i]["paired"]
        omr = gp.get("one_minus_rho")
        print(f"  degenerate row: dir is the reference: "
              f"{ref is not None and Path(got_rows[i]['dir']).resolve() == (equity.parent / ref).resolve()}; "
              f"one_minus_rho is float: {isinstance(omr, float)}; below 1e-6: {isinstance(omr, float) and omr < 1e-6}")
        for keys in (("memmel_se",), ("t",), ("lw", "se")):
            gv, wv = gp, wp
            for k in keys:
                gv, wv = gv.get(k) if isinstance(gv, dict) else None, wv.get(k) if isinstance(wv, dict) else None
            print(f"    {'.'.join(keys)}: got is None {gv is None}; want is float {isinstance(wv, float)}; "
                  f"want finite {isinstance(wv, float) and math.isfinite(wv)}")
    gp_doc = wp_doc = None
    if "--pbo-json" in argv and pbo.exists():
        gp_doc = json.loads((tmp / "pbo.json").read_text(encoding="utf-8"))
        wp_doc = json.loads(pbo.read_text(encoding="utf-8"))
        gp_doc.pop("nav_summ_run", None), wp_doc.pop("nav_summ_run", None)
    rel_s = compare("summ", copy.deepcopy(got_rows), copy.deepcopy(want_rows))
    rel_p = compare("pbo", gp_doc, wp_doc) if gp_doc is not None else 0.0
    print(f"max |relative difference| over summ and pbo: {max(rel_s, rel_p):.3e}")
    teeth(got_rows, want_rows, gp_doc, wp_doc)
    return 0


def first_float_path(x, path=""):
    for p, v in leaves(x, path):
        if isinstance(v, float) and math.isfinite(v) and v != 0.0:
            return p
    return None


def set_at(x, path: str, fn):
    parts = re.findall(r"\.([^.\[]+)|\[(\d+)\]", path)
    for k, i in parts[:-1]:
        x = x[k] if k else x[int(i)]
    k, i = parts[-1]
    if k:
        x[k] = fn(x[k])
    else:
        x[int(i)] = fn(x[int(i)])


def teeth(got_rows, want_rows, gp_doc, wp_doc):
    """The test's helpers on the real rows, then on perturbed copies (in memory)."""
    try:
        import test_nav_summ_v8 as T
    except Exception as e:  # noqa: BLE001
        print("teeth: test module not importable:", type(e).__name__)
        return
    if not hasattr(T, "numeric_mismatches") or not hasattr(T, "take_degenerate_disclosure"):
        print("teeth: the test module has no numeric helpers (pre-fix tree)")
        return

    def judge(g_rows, w_rows, g_pbo, w_pbo):
        g_rows, w_rows = copy.deepcopy(g_rows), copy.deepcopy(w_rows)
        n_deg, problems = 0, []
        for i, (g, w) in enumerate(zip(g_rows, w_rows)):
            is_deg, bad = T.take_degenerate_disclosure(g, w)
            n_deg += is_deg
            problems += [f"[{i}]{p}" for p in bad + T.numeric_mismatches(g, w)]
        if g_pbo is not None:
            problems += T.numeric_mismatches(copy.deepcopy(g_pbo), copy.deepcopy(w_pbo))
        return n_deg, problems

    n_deg, problems = judge(got_rows, want_rows, gp_doc, wp_doc)
    print(f"teeth 0 (real rows, helpers as the test calls them): degenerate rows {n_deg}; problems {len(problems)} "
          f"{[generic(p) for p in problems[:10]]}")
    nondeg = next(i for i, g in enumerate(got_rows) if not (isinstance(g.get("paired"), dict)
                                                            and g["paired"].get("degenerate")))
    path = first_float_path(got_rows[nondeg])
    cases = []
    for scale, label in ((1 + 3e-12, "a float leaf x (1 + 3e-12)"), (1 + 5e-13, "a float leaf x (1 + 5e-13)")):
        g = copy.deepcopy(got_rows)
        set_at(g[nondeg], path, lambda v, s=scale: v * s)
        cases.append((label, g, gp_doc))
    g = copy.deepcopy(got_rows)
    set_at(g[nondeg], path, lambda v: None)
    cases.append(("a float leaf -> null", g, gp_doc))
    g = copy.deepcopy(got_rows)
    g[nondeg]["extra_key"] = 1
    cases.append(("an extra key", g, gp_doc))
    g = copy.deepcopy(got_rows)
    g[nondeg]["scenario"] = str(g[nondeg].get("scenario")) + "x"
    cases.append(("a string leaf changed", g, gp_doc))
    if gp_doc is not None:
        gp2 = copy.deepcopy(gp_doc)
        set_at(gp2, first_float_path(gp2), lambda v: v * (1 + 3e-12))
        cases.append(("a pbo float leaf x (1 + 3e-12)", got_rows, gp2))
    deg = [i for i, r in enumerate(got_rows) if isinstance(r.get("paired"), dict) and r["paired"].get("degenerate")]
    if deg:
        g = copy.deepcopy(got_rows)
        g[deg[0]]["paired"]["one_minus_rho"] = 1e-3
        cases.append(("degenerate row with 1 - rho = 1e-3", g, gp_doc))
        g = copy.deepcopy(got_rows)
        g[deg[0]]["paired"]["t"] = 0.0
        cases.append(("degenerate row with a refused key not null", g, gp_doc))
        g = copy.deepcopy(got_rows)
        del g[deg[0]]["paired"]["degenerate"]
        cases.append(("degenerate flag removed", g, gp_doc))
    for label, g, gp in cases:
        try:
            n, probs = judge(g, want_rows, gp, wp_doc)
            print(f"teeth: {label}: degenerate rows {n}; problems {len(probs)} {[generic(p) for p in probs[:4]]}")
        except Exception as e:  # noqa: BLE001
            print(f"teeth: {label}: raised {type(e).__name__}")


if __name__ == "__main__":
    raise SystemExit(main())
