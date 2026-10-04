r"""S2-4 / W1-I1 measurement: longest cache path a Release exe writes under --eval-mode audit-exact.

Path names only (no file content, no statistic). Layout (strategy_ic_signal_cache.cpp:113-127,
strategy_ic_result_cache.cpp:70, strategy_ic_runner.cpp:497/539):
  ROOT = <cache dir>\<vm identity>        (legacy Debug identity: DIR itself)
  signal  ROOT\<role sha64>[\fp_<16>]\<id>.<dsl16>.f64 / .json
  IC      ROOT\<role sha64>[\fp_<16>]\ic1_<16>\<id>.<dsl16>.json   (deepest)
  partial beside final: .<16 hex>.partial (25 chars)
"""
import json, os, subprocess, sys
REPO = r"C:\atx-wt\pool-2"
IDENT = sys.argv[1]  # Release + audit identity directory name
def ids_of(lib):
    try:
        d = json.load(open(os.path.join(REPO, lib), encoding="utf-8"))
    except Exception:
        return []
    return [c["id"] for c in d.get("candidates", []) if isinstance(c, dict) and "id" in c]
specs = subprocess.run(["git", "-C", REPO, "ls-files", "scripts/specs"], capture_output=True, text=True).stdout.split()
rows = []
LAYOUT = {}
def layout(root):
    out = {}
    if not os.path.isdir(root): return out
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.endswith(".f64"):
                cid = f.split(".")[0]
                k = "field" if os.path.basename(dp).startswith("fp_") else "base"
                if out.get(cid) != "field": out[cid] = k
    return out
for s in specs:
    if not s.endswith(".json"): continue
    try: d = json.load(open(os.path.join(REPO, s), encoding="utf-8"))
    except Exception: continue
    cache = (d.get("ic") or {}).get("cache") if isinstance(d, dict) else None
    if not cache: continue
    root = os.path.normpath(os.path.join(REPO, cache))
    libs = [v.get("path") for k, v in (d.get("inputs") or {}).items() if k.endswith("library") and isinstance(v, dict)]
    ids = sorted({i for l in libs if l for i in ids_of(l)}, key=len)
    if not ids: continue
    # per candidate: field candidate (fp_ dir) if the on-disk cache shows it under fp_, base if it
    # shows it directly under the role dir, unknown (counted as field: worst case) otherwise
    seen = LAYOUT.setdefault(root, layout(root))
    best = (0, "", "")
    for i in ids:
        kind = seen.get(i, "unseen")
        depth = len(root) + 1 + len(IDENT) + 1 + 64 + (20 if kind != "base" else 0) + 1 + 20 + 1 + len(i) + 1 + 16 + 5
        if depth > best[0]: best = (depth, i, kind)
    rows.append((best[0], s, cache, best[1], best[2]))
rows.sort(reverse=True)
print(f"identity dir: {IDENT} ({len(IDENT)} chars)")
print("spec-bound deepest IC-result entry per spec (layout from the on-disk cache; unseen = counted as field), top 8 of", len(rows), "specs with an ic.cache:")
for w, s, c, i, k in rows[:10]: print(f"  {w}  margin to 250: {250 - w:+d}  {s}  cache={c}  deepest id={i} ({len(i)}, {k})")
# real: every entry on disk in the spec caches, rebased under the Release audit root
print(r"real entries on disk, rebased (Debug = legacy identity, so DIR\<rel>):")
for c in sorted({r[2] for r in rows}):
    root = os.path.normpath(os.path.join(REPO, c))
    if not os.path.isdir(root): print(f"  {c}: absent"); continue
    best = (0, "")
    for dp, dn, fn in os.walk(root):
        for f in fn:
            rel = os.path.relpath(os.path.join(dp, f), root)
            if rel.split(os.sep)[0].startswith("dslvm"): rel = rel.split(os.sep, 1)[1]
            p = os.path.join(root, IDENT, rel)
            if len(p) > best[0]: best = (len(p), p)
    print(f"  {c}: longest {best[0]} (margin to 250: {250 - best[0]:+d}; to MAX_PATH 259: {259 - best[0]:+d})\n     {best[1]}")
