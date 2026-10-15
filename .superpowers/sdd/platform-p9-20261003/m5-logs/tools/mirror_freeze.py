"""Root M5: what test_rows_retired_by_a_p9_lane_are_gone_at_the_freeze would report with P9_FREEZE = True, without
editing the file: imports scripts/tests/test_no_python_mirror.py and prints every ALLOWLIST row whose retired_by is a
P9 lane (module, symbols, lane), the G-P5 count, and whether FROZEN_PAIRS names a given symbol.
usage: mirror_freeze.py [SYMBOL ...]  (run from the pool-2 root)"""
import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("mirror", Path("scripts/tests/test_no_python_mirror.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
left = [(mod, list(sy), lane) for mod, sy, _r, _c, lane in m.ALLOWLIST if lane in m.P9_LANES]
print(f"P9_FREEZE in file: {m.P9_FREEZE}; rows left for P9 lanes at the freeze: {len(left)}")
for row in left:
    print("  ", row)
print(f"G-P5 count: {sum(len(r[1]) for r in left)} function(s) in {len(left)} row(s); ALLOWLIST rows {len(m.ALLOWLIST)}")
for s in sys.argv[1:]:
    print(f"FROZEN_PAIRS names {s}: {any(sym == s for _mod, sym in m.FROZEN_PAIRS)}")
