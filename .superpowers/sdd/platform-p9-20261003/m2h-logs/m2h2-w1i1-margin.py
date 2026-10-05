"""Root M2h finish: the W1-I1 path-length margins (E2 re-review 2 items 6 and 11, re-review 4 item 6) at HEAD in
pool-2: for every committed cell spec / template under scripts/specs with an IC pass, the deepest IC cache path of its
longest library id under the cycle's own cache dir (cycle.phases.Cycle.cache_dir, as the exe resolves it from pool-2),
at the spec's own layout (audit-exact when an IC pass asks for it) and at the Release layout; margin = 250 - length.
Plus the tiny-world roots of this step and the canary's %TEMP% root shape. Read-only; no exe runs.
"""
from __future__ import annotations

import glob
import json
import os
from pathlib import Path
import sys
import tempfile

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "scripts"))
import research_cycle as RC  # noqa: E402
from cycle import phases as P  # noqa: E402
import research_spec  # noqa: E402

rows, skipped = [], []
for f in sorted(glob.glob(str(REPO / "scripts" / "specs" / "**" / "*.json"), recursive=True)):
    rel = Path(f).relative_to(REPO).as_posix()
    try:
        doc = json.loads(Path(f).read_text(encoding="utf-8"))
    except ValueError:
        continue
    if not isinstance(doc, dict) or doc.get("schema") not in (RC.SCHEMA, research_spec.TEMPLATE_SCHEMA):
        continue
    try:
        spec = RC.load_spec(Path(f))
    except Exception as exc:  # noqa: BLE001  (a template on a nominal parent that is not on disk, etc.)
        skipped.append(f"{rel}: {str(exc)[:100]}")
        continue
    if "ic" not in spec or "library" not in spec.get("inputs", {}):
        continue
    try:
        c = RC.Cycle(spec, RC.Resolver(REPO), verify=False)
        lib = REPO / spec["inputs"]["library"]["path"]
        ids = P.library_ids(json.loads(lib.read_text(encoding="utf-8"))) if lib.is_file() else []
        cache = os.path.abspath(REPO / c.cache_dir())
    except Exception as exc:  # noqa: BLE001
        skipped.append(f"{rel}: {str(exc)[:100]}")
        continue
    if not ids:
        skipped.append(f"{rel}: library {spec['inputs']['library']['path']} not on disk or no ids")
        continue
    ic = spec["ic"]
    sets = [ic["flags"]] + ([research_spec.apply_flags(ic["flags"], ic.get("w_flags") or {}, "w")]
                            if "w_output" in ic else [])
    audit = any(P.option_value(s, "--eval-mode") == P.CACHE_AUDIT_MODE for s in sets)
    cid = max(ids, key=len)
    n_own = len(P.deepest_cache_path(cache, cid, audit))
    n_rel = len(P.deepest_cache_path(cache, cid, False))
    rows.append((P.CACHE_PATH_MAX - n_own, n_own, n_rel, audit, len(cid), cid, rel, Path(cache).as_posix()))

rows.sort()
print(f"pool-2 root {REPO.as_posix()} ({len(REPO.as_posix())} chars); bound CACHE_PATH_MAX {P.CACHE_PATH_MAX}; "
      f"{len(rows)} specs with an IC pass and a library on disk; {len(skipped)} skipped")
print("margin\tlen(own layout)\tlen(Release)\taudit\tlen(id)\tid\tspec\tcache dir")
for r in rows[:15]:
    print("\t".join(str(x) for x in r))
print(f"refused at pool-2 (margin < 0): {[r[6] for r in rows if r[0] < 0]}")
print(f"margin <= 2: {[(r[6], r[0]) for r in rows if r[0] <= 2]}")
print("--- tiny-world roots (ids planted_a etc.; ic.cache tiny-cache)")
tw_id = "planted_a"
for root in sorted(glob.glob(str(REPO / "build-equity" / "p9-m2h-tw*"))) + [
        os.path.join(tempfile.gettempdir(), "tiny-world-" + "x" * 8)]:
    cache = Path(root, "tiny-cache").as_posix()
    for audit in (False, True):
        n = len(P.deepest_cache_path(cache, tw_id, audit))
        print(f"{cache}\taudit {audit}\tlen {n}\tmargin {P.CACHE_PATH_MAX - n}")
print("--- skipped (no IC pass resolvable here)")
for s in skipped:
    print(s)
