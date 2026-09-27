"""Bounded synthetic reference receipt; no market data or C++ build.

EDGE executes the authors' hash-pinned Python source. CS/AR are an independent
scalar transcription of their R formulas; R/package execution remains pending.
The synthetic rows are fixed before execution, not fitted to expected outputs.
"""
import hashlib
import json
import math
from pathlib import Path
import urllib.request

import numpy

REV = "1caba55d63ebab6c855536be51c43bdfc48d2dec"
BASE = "https://raw.githubusercontent.com/eguidotti/bidask/" + REV + "/"
EDGE_SHA = "08305189738c2fe4b9156f23c6d8ef2c8aacd597efa415e894670aa7dc4f5f7d"
source = urllib.request.urlopen(BASE + "python/bidask/edge.py", timeout=20).read()
assert hashlib.sha256(source).hexdigest() == EDGE_SHA
scope = {}
exec(compile(source, BASE + "python/bidask/edge.py", "exec"), scope)
source_hashes = {"python/bidask/edge.py": EDGE_SHA}
for name in ("r/R/cs.R", "r/R/ar.R"):
    source_hashes[name] = hashlib.sha256(
        urllib.request.urlopen(BASE + name, timeout=20).read()).hexdigest()


def cs_ar(rows):
    cs, ar = [], []
    denom = 3 - 2 * math.sqrt(2)
    for prev, cur in zip(rows, rows[1:]):
        _, h0, l0, c0 = map(math.log, prev)
        _, h, l, _ = map(math.log, cur)
        gap = max(0.0, c0 - h) + min(0.0, c0 - l)
        beta = (h - l) ** 2 + (h0 - l0) ** 2
        gamma = (max(h + gap, h0) - min(l + gap, l0)) ** 2
        alpha = (math.sqrt(2 * beta) - math.sqrt(beta)) / denom - math.sqrt(gamma / denom)
        exp_alpha = math.exp(alpha)
        cs.append(2 * (exp_alpha - 1) / (1 + exp_alpha))
        ar.append(4 * (c0 - (h0 + l0) / 2) * (c0 - (h + l) / 2))
    return abs(sum(cs) / len(cs)), math.sqrt(abs(sum(ar) / len(ar)))


rows = [[100, 101, 99, 100.7], [100.5, 101.2, 99.8, 100.1],
        [99, 100, 98.4, 99.5], [101.3, 102, 100.7, 101.8],
        [100.8, 102.2, 100.2, 101.1], [102.1, 103.5, 101.2, 103],
        [101.7, 102.4, 100.9, 101.3], [103, 104, 102.6, 103.8]]
cases = {"irregular_quotes": rows,
         "overnight_gap": [r if i < 3 else [v * 1.8 for v in r] for i, r in enumerate(rows)],
         "constant_range": [[100, 101, 99, 100] for _ in range(8)]}
result = {}
for name, values in cases.items():
    cs, ar = cs_ar(values)
    edge = scope["edge"](*numpy.array(values, dtype=float).T)
    result[name] = {"ohlc": values, "cs": cs, "ar": ar, "edge": edge,
                    "equal_weight_full_spread": (cs + ar + edge) / 3}
receipt = {"upstream_revision": REV, "source_sha256": source_hashes,
           "numpy_version": numpy.__version__, "input_origin": "fixed synthetic OHLC",
           "edge_reference": "Executed unmodified pinned upstream Python edge.py",
           "cs_ar_reference": "Scalar transcription of pinned R equations; R execution unrun",
           "cpp_compilation_and_runtime": "UNRUN", "cases": result}
target = Path(__file__).with_suffix(".json")
target.write_text(json.dumps(receipt, indent=2, allow_nan=False) + "\n", encoding="utf-8")
print(json.dumps({k: {n: v for n, v in c.items() if n != "ohlc"}
                  for k, c in result.items()}, indent=2))
print("receipt_sha256", hashlib.sha256(target.read_bytes()).hexdigest())
