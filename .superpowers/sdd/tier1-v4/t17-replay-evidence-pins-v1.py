"""Pin the already retained small controller evidence before replay recovery."""
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get("ATX_GUARD_JOB"):
    raise RuntimeError("External guard required")
base = Path(__file__).resolve().parent
out = base / "t17-replay-evidence-pins-v1.json"
if out.exists():
    raise RuntimeError("Preserve existing evidence pins")
names = ["t17-interruption-inspection-v4.json", "t17-financial-progress-audit-v2.json"]
pins = {}
for name in names:
    path = base / name
    before = path.stat()
    if before.st_size > 1024 * 1024:
        raise RuntimeError("Unexpected metadata size")
    raw = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError("Metadata changed")
    pins[name] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
for name, pin in pins.items():
    raw = (base / name).read_bytes()
    if {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()} != pin:
        raise RuntimeError("Metadata changed between passes")
with out.open("x", encoding="utf-8") as stream:
    json.dump({"status": "SMALL_RETAINED_CONTROLLER_EVIDENCE_PINNED", "pins": pins,
               "unit_admission": False, "source_replay": False}, stream, indent=2)
    stream.write("\n")
print(json.dumps(pins))
