"""PM5-16 / PM5-18 (3) cause test (i), read-only (orientation-consumers.md Q5 (i); integrator W0-2c).

argv: OLD_train_daily_ic.csv NEW_train_daily_ic.csv EXPECTED_IDS (comma-separated)
Compares the member rows (not __combined__) on the common sessions (before 2022-09-30): the raw columns
(pearson, rank_ic) must be bit-identical for every id; oriented_rank_ic must be bit-identical for every id outside
EXPECTED_IDS and the exact negation (sign bit flipped) on every finite cell of each id in EXPECTED_IDS.
Prints ONE dict of counts and booleans. On failure it names the first failing (id, column) in old-file order;
it never prints a value, a sign or an id next to a sign. A row on or after the seal is refused.
"""
import csv, datetime as dt, math, struct, sys
ns = lambda d: (d - dt.date(1970, 1, 1)).days * 86_400_000_000_000
SEAL, BEFORE = ns(dt.date(2024, 1, 1)), ns(dt.date(2022, 9, 30))
HEADER = ["id", "horizon", "decision_index", "session_ns", "pearson", "rank_ic", "oriented_rank_ic"]
bits = lambda x: struct.unpack("<Q", struct.pack("<d", x))[0]
def load(path):
    out = {}
    with open(path, newline="", encoding="utf-8") as f:
        r = csv.reader(f); assert next(r) == HEADER
        for row in r:
            s = int(row[3]); assert s < SEAL, "on/after seal: refuse"
            if s < BEFORE and row[0] != "__combined__":
                out.setdefault(row[0], {})[(s, int(row[1]))] = tuple(float(v) if v else math.nan for v in row[4:])
    return out
same = lambda a, b: (math.isnan(a) and math.isnan(b)) or bits(a) == bits(b)
neg = lambda a, b: not math.isnan(a) and not math.isnan(b) and bits(b) == bits(a) ^ (1 << 63)
old, new, expect = load(sys.argv[1]), load(sys.argv[2]), set(sys.argv[3].split(","))
eq_ids, neg_ids, other, raw_ok, cells, negated_cells, first = set(), set(), set(), True, 0, 0, None
for cid, o in old.items():
    n = new.get(cid, {}); keys = [k for k in o if k in n]; assert keys and len(keys) == len(o)
    e = g = bad = 0
    for k in keys:
        (p0, r0, x0), (p1, r1, x1) = o[k], n[k]; cells += 1
        for col, a, b in (("pearson", p0, p1), ("rank_ic", r0, r1)):
            if not same(a, b):
                raw_ok = False
                if first is None: first = [cid, col]
        if same(x0, x1): e += 1
        elif neg(x0, x1): g += 1
        else: bad += 1
    finite = sum(1 for k in keys if not math.isnan(o[k][2]))
    cls = eq_ids if bad == 0 and g == 0 else neg_ids if bad == 0 and g == finite else other
    cls.add(cid)
    if cls is neg_ids: negated_cells += g
    if first is None and (cls is other or (cls is neg_ids) != (cid in expect)):
        first = [cid, "oriented_rank_ic"]
res = {"ids": len(old), "ids_new": len(new), "id_sets_equal": set(new) == set(old), "expected_ids": len(expect),
       "cells": cells, "raw_columns_identical": raw_ok, "ids_equal": len(eq_ids),
       "ids_negated_every_finite_cell": len(neg_ids), "negated_cells": negated_cells, "ids_other": len(other),
       "negated_are_expected": neg_ids == expect, "equal_are_the_rest": eq_ids == set(old) - expect}
res["pass"] = (raw_ok and res["id_sets_equal"] and not other and res["negated_are_expected"]
               and res["equal_are_the_rest"])
res["first_failure"] = first
print(res)
