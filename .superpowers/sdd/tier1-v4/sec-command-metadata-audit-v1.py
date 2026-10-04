"""Inventory retained execution metadata; never infer a complete SEC budget."""
import hashlib
import json
import os
from pathlib import Path
import re

if not os.environ.get("ATX_GUARD_JOB"):
    raise RuntimeError("External memory guard required")
base = Path(__file__).resolve().parent
repo = base.parents[2]
output = base / "sec-command-metadata-audit-v1.json"
if output.exists():
    raise RuntimeError("Preserve prior evidence")
roots = [
    repo / "atx-db/data/raw/sec_text/_logs",
    repo / "atx-db/data/raw/sec_events/_logs",
    repo / "atx-db/data/raw/sec-earnings-release/_logs",
    repo / "atx-db/data/alpha_panel/v1/_tmp/events",
    repo / ".superpowers/sdd/tier1-v3",
    repo / ".superpowers/sdd/tier1-parity",
]
needle = re.compile(r"filing_text|events_sources|fetch_sec_earnings_releases|press_release")
pins = {}
records = []
scripts = []
skipped = []
roots_report = []
visited = 0
bytes_read = 0

def read_bounded(path, limit):
    global bytes_read
    before = path.stat()
    if before.st_size > limit:
        skipped.append({"path": str(path), "reason": "byte_bound", "bytes": before.st_size})
        return None
    raw = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError("Metadata changed while read")
    bytes_read += len(raw)
    if bytes_read > 64 * 1024 * 1024:
        raise RuntimeError("Metadata audit total byte bound exceeded")
    return raw

def remember(path, raw):
    pins[str(path)] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}

for root in roots:
    if not root.exists():
        roots_report.append({"path": str(root), "exists": False})
        continue
    if root.is_symlink() or root.is_junction():
        raise RuntimeError("Do not traverse reparse roots")
    resolved = root.resolve(strict=True)
    if not resolved.is_relative_to(repo):
        raise RuntimeError("Metadata root escaped workspace")
    roots_report.append({"path": str(root), "exists": True})
    pending = [(root, 0)]
    while pending:
        directory, depth = pending.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                visited += 1
                if visited > 50000:
                    raise RuntimeError("Metadata entry bound exceeded")
                path = Path(entry.path)
                if entry.is_symlink() or path.is_junction():
                    skipped.append({"path": str(path), "reason": "reparse"})
                    continue
                if entry.is_dir(follow_symlinks=False):
                    # Traverse only execution-log directories, never raw objects.
                    if depth < 2 and re.search(r"log|receipt|guard", entry.name, re.I):
                        pending.append((path, depth + 1))
                    continue
                if not entry.is_file(follow_symlinks=False):
                    continue
                if path.suffix.lower() == ".json":
                    raw = read_bounded(path, 256 * 1024)
                    if raw is None:
                        continue
                    try:
                        value = json.loads(raw)
                    except (ValueError, UnicodeError):
                        continue
                    if not isinstance(value, dict):
                        continue
                    command = value.get("command")
                    if not isinstance(command, list) or not all(isinstance(x, str) for x in command):
                        continue
                    if not needle.search(" ".join(command)):
                        continue
                    remember(path, raw)
                    admission = value.get("admission") or {}
                    records.append({
                        "path": str(path), "command": command,
                        "status": value.get("status"), "returncode": value.get("returncode"),
                        "child_pid": value.get("child_pid"), "guard_pid": value.get("guard_pid"),
                        "run_seconds": value.get("run_seconds"),
                        "admitted_at_utc": admission.get("admitted_at_utc"),
                        "requested_at_utc": admission.get("requested_at_utc"),
                        "stop_reason": value.get("stop_reason"),
                        "job_limit_gb": value.get("job_limit_gb"),
                        "scope": "named retained child only; invocation inventory not certified complete",
                    })
                elif path.suffix.lower() in {".sh", ".ps1"} and "sec_text" in path.parts:
                    raw = read_bounded(path, 65536)
                    if raw is None:
                        continue
                    lines = raw.decode("utf-8-sig").splitlines()
                    matches = [{"line": i, "text": line} for i, line in enumerate(lines, 1)
                               if needle.search(line) or re.search(r"THREADS|run_memory_guarded|while |for |exit |sleep |--receipt", line)]
                    if matches:
                        remember(path, raw)
                        scripts.append({"path": str(path), "selected_execution_lines": matches,
                                        "executed_by_audit": False})

for path_text, pin in pins.items():
    path = Path(path_text)
    raw = read_bounded(path, 256 * 1024)
    if raw is None or {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()} != pin:
        raise RuntimeError("Retained execution evidence changed")
result = {
    "status": "NAMED_RETAINED_EXECUTION_METADATA_INVENTORIED_NOT_BUDGET_CERTIFICATION",
    "roots": roots_report, "visited_entries": visited, "bytes_read_including_recheck": bytes_read,
    "executions": records, "scripts": scripts, "skipped": skipped, "pins": pins,
    "complete_invocation_inventory": False, "budget_upper_bound": None,
    "remaining_budget_granted": 0, "network_attempts": 0, "source_bodies_read": 0,
    "qualification": "Named small retained metadata only. Missing children, unguarded producers, rate-lock coverage, baseline interval and hidden attempts remain unresolved. Neither file timestamps nor first/last outcome receipts establish process lifetime. No historical cap, source integrity or acquisition permission is inferred.",
}
with output.open("x", encoding="utf-8") as sink:
    json.dump(result, sink, indent=2)
    sink.write("\n")
print(json.dumps({"status": result["status"], "executions": len(records), "scripts": len(scripts),
                  "visited_entries": visited, "bytes_read_including_recheck": bytes_read,
                  "complete_invocation_inventory": False, "remaining_budget_granted": 0}))
