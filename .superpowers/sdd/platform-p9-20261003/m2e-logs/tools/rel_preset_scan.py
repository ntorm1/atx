"""M2e C2 root item (iv): was any P9 reference produced under the `rel` (or `rel-avx2`) preset?

Scans pool-2's build receipts (build-equity/mega-*-receipt.json: Preset) and every bounded-run receipt
(build-equity/*/receipt.json: the executable path, command[0], and build_type). The `rel` preset builds into
build-rel/ and `rel-avx2` into build-rel-avx2/ (CMakePresets.json); equity-dev = build-equity/, equity-rel =
build-equity-rel/. Prints counts and any hit; reads no output file.
"""
import collections
import glob
import json
import os
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else r"C:\atx-wt\pool-2"
BE = os.path.join(ROOT, "build-equity")


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


presets = collections.Counter()
odd_builds = []
for f in sorted(glob.glob(os.path.join(BE, "mega-*-receipt.json"))):
    try:
        r = load(f)
    except Exception as e:  # noqa: BLE001
        print("unreadable build receipt", os.path.basename(f), e)
        continue
    presets[r.get("Preset")] += 1
    if r.get("Preset") not in ("equity-dev", "equity-rel"):
        odd_builds.append((os.path.basename(f), r.get("Preset")))
print("build receipts by preset:", dict(presets))
print("build receipts outside equity-dev / equity-rel:", odd_builds)

runs = 0
types = collections.Counter()
trees = collections.Counter()
hits = []
for f in sorted(glob.glob(os.path.join(BE, "*", "receipt.json"))):
    try:
        r = load(f)
    except Exception:  # noqa: BLE001
        continue
    cmd = r.get("command") or []
    if not cmd:
        continue
    runs += 1
    name = os.path.basename(os.path.dirname(f))
    p9 = name.startswith("p9-")
    exe = str(cmd[0]).replace("/", "\\").lower()
    parts = exe.split("\\")
    tree = parts[parts.index("bin") - 1] if "bin" in parts else os.path.dirname(exe)
    trees[(p9, tree)] += 1
    types[(p9, r.get("build_type"))] += 1
    if tree in ("build-rel", "build-rel-avx2") or "\\build-rel\\" in exe or "\\build-rel-avx2\\" in exe:
        hits.append((name, cmd[0]))
print("run receipts:", runs)
print("build_type by (is_p9_run, type):", dict(types))
print("exe tree by (is_p9_run, tree):", dict(trees))
print("runs on a rel / rel-avx2 exe:", hits)
print("present trees:", [d for d in ("build-rel", "build-rel-avx2") if os.path.isdir(os.path.join(ROOT, d))])
