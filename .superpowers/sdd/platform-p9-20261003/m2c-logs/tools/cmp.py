"""Compare two run output dirs by file SHA-256 and JSON leaf path only; never prints a value.

usage: cmp.py A B [--allow REGEX]...   (REGEX matched against a JSON leaf path, e.g. '/wall_seconds$')
JSON: leaf paths that differ (added / removed / changed); JSONL: per line; CSV: header equality and
the count of differing rows only. A differing leaf not matched by any --allow is OUTSIDE.
"""
import hashlib, json, os, re, sys
a, b = sys.argv[1], sys.argv[2]
allow = [re.compile(sys.argv[i + 1]) for i, t in enumerate(sys.argv) if t == "--allow"]
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
    return h.hexdigest()
def files(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        for f in fn: out[os.path.relpath(os.path.join(dp, f), root).replace("\\", "/")] = os.path.join(dp, f)
    return out
def leaves(o, p=""):
    if isinstance(o, dict):
        if not o: yield p + "/{}", None
        for k, v in o.items(): yield from leaves(v, f"{p}/{k}")
    elif isinstance(o, list):
        if not o: yield p + "/[]", None
        for i, v in enumerate(o): yield from leaves(v, f"{p}/{i}")
    else: yield p, o
def jdiff(x, y):
    lx, ly = dict(leaves(x)), dict(leaves(y))
    return sorted(k for k in set(lx) | set(ly) if k not in lx or k not in ly or lx[k] != ly[k] or type(lx[k]) != type(ly[k]))
def ok(path): return any(r.search(path) for r in allow)
def collapse(paths):
    return sorted({re.sub(r"/\d+(?=/|$)", "/*", q) for q in paths})
fa, fb = files(a), files(b)
outside_total = 0
for rel in sorted(set(fa) | set(fb)):
    if rel not in fa or rel not in fb:
        print(f"ONE-SIDED {rel} ({'B only' if rel in fb else 'A only'})"); outside_total += 1; continue
    sa, sb = sha(fa[rel]), sha(fb[rel])
    if sa == sb: print(f"same   {rel} {sa[:8]}"); continue
    if rel.endswith(".json"):
        d = jdiff(json.load(open(fa[rel], encoding="utf-8")), json.load(open(fb[rel], encoding="utf-8")))
    elif rel.endswith(".jsonl"):
        la = open(fa[rel], encoding="utf-8").read().splitlines(); lb = open(fb[rel], encoding="utf-8").read().splitlines()
        if len(la) != len(lb): print(f"DIFF   {rel}: line count {len(la)} vs {len(lb)} OUTSIDE"); outside_total += 1; continue
        d = []
        for x, y in zip(la, lb): d += jdiff(json.loads(x), json.loads(y))
    elif rel.endswith(".csv"):
        la = open(fa[rel], encoding="utf-8").read().splitlines(); lb = open(fb[rel], encoding="utf-8").read().splitlines()
        rows = sum(1 for x, y in zip(la[1:], lb[1:]) if x != y)
        print(f"DIFF   {rel}: header {'same' if la[:1] == lb[:1] else 'DIFFERS'}, rows {len(la)} vs {len(lb)}, differing rows {rows} OUTSIDE")
        outside_total += 1; continue
    else:
        print(f"DIFF   {rel} {sa[:8]} vs {sb[:8]} (binary) OUTSIDE"); outside_total += 1; continue
    out = [q for q in d if not ok(q)]
    inside = [q for q in d if ok(q)]
    print(f"DIFF   {rel} {sa[:8]} vs {sb[:8]}: {len(d)} leaves, {len(inside)} allowed, {len(out)} outside")
    for q in collapse(out)[:40]: print(f"         OUTSIDE {q}")
    if inside: print(f"         allowed path classes: {collapse(inside)[:12]}")
    outside_total += len(out)
print(f"TOTAL outside the list: {outside_total}")
