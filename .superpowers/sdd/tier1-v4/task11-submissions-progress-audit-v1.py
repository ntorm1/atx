"""Audit retained bounded inventory outputs, never certify complete 13F sources."""
import hashlib
import json
import os
from pathlib import Path
import re

if not os.environ.get("ATX_GUARD_JOB"):
    raise RuntimeError("External memory guard required")
base = Path(__file__).resolve().parent
repo = base.parents[2]
out = base / "task11-submissions-progress-v1.json"
if out.exists():
    raise RuntimeError("Preserve prior evidence")

def identity(path):
    before = path.stat()
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError("Changed input")
    return {"sha256": sha.hexdigest(), "bytes": after.st_size}

archive = repo / "atx-db/data/cache/submissions.zip"
archive_expected = {"sha256": "702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165", "bytes": 1564656199}
if identity(archive) != archive_expected:
    raise RuntimeError("Original archive changed")
code_expected = {
    "thirteenf_quality": "02a04d7e4dfe8221cb0d65c076ecd506fa049ff2dc3e75628aee6702e7dbd5c5",
    "submissions_archive": "12d48c0334fa0013726200967809f9fb0cb2eee75113debd8cd78d0eba89f09f",
}
paths = {
    "thirteenf_quality": repo / "atx-db/src/atx_db/alpha_panel/thirteenf_quality.py",
    "submissions_archive": repo / "atx-db/src/atx_db/_submissions_archive.py",
}
pins = {str(archive): archive_expected}
for name, path in paths.items():
    pin = identity(path)
    if pin["sha256"] != code_expected[name]:
        raise RuntimeError("Native inventory reader changed")
    pins[str(path)] = pin

after = None
records = []
seen = set()
counts = {"main_members": 0, "history_members": 0, "filings": 0}
exceptions = []
for part, version in ((0, 1), (1, 2), (2, 1)):
    tag = f"part{part:04d}-v{version}"
    output = base / f"task11-submissions-inventory-{tag}.jsonl"
    receipt = output.with_suffix(output.suffix + ".receipt.json")
    guard_path = base / f"logs/t11-submissions-inventory-{tag}.receipt.json"
    for path in (output, receipt, guard_path):
        pins[str(path)] = identity(path)
    native = json.loads(receipt.read_text(encoding="utf-8"))
    guard = json.loads(guard_path.read_text(encoding="utf-8"))
    if guard["status"] != "failed" or guard["returncode"] != 2 or guard["cap_hit"] or guard.get("stop_reason"):
        raise RuntimeError("Not a completed native unqualified partition")
    if guard["job_limit_gb"] > 1 or guard["native_peak_job_memory_bytes"] > guard["job_cap_bytes"]:
        raise RuntimeError("Guard bound violated")
    command = [".venv/Scripts/python.exe", "-m", "atx_db.alpha_panel.thirteenf_quality", "inventory-submissions",
               "--archive", archive.as_posix()]
    if after is not None:
        command.extend(["--after", after])
    command.extend(["--through", "CIK9999999999.json", "--max-main", "10000", "--first", "2019-01-01", "--output", output.as_posix()])
    if guard["command"] != command:
        raise RuntimeError("Unexpected bounded native command")
    if (native["schema"] != "atx.thirteenf-submissions-inventory-partition/v1" or
            native["source_completeness"] != "UNQUALIFIED_PARTITION" or
            native["after"] != after or native["through"] != "CIK9999999999.json" or
            native["archive_sha256"] != archive_expected["sha256"] or
            native["code_sha256"] != code_expected or native["first_filing_date"] != "2019-01-01" or
            native["counts"]["main_members"] != 10000 or not native["bound_reached"] or
            native["output_sha256"] != pins[str(output)]["sha256"]):
        raise RuntimeError("Partition identity/range contract mismatch")
    rows = 0
    with output.open("rb") as stream:
        while raw := stream.readline(65537):
            if len(raw) > 65536 or not raw.endswith(b"\n"):
                raise RuntimeError("Unbounded or partial output row")
            row = json.loads(raw)
            main = f"CIK{row['cik']:010d}.json"
            if ((after is not None and main <= after) or main > native["last_main_member"] or
                    row["inventory_sha256"] != archive_expected["sha256"] or
                    not re.fullmatch(r"\d{18}", row["accession"]) or row["filing_date"] < "2019-01-01"):
                raise RuntimeError("Output row escapes bound/source identity")
            seen.add((row["cik"], row["accession"]))
            rows += 1
    if rows != native["counts"]["filings"]:
        raise RuntimeError("Output row count differs")
    for name in counts:
        counts[name] += native["counts"][name]
    exceptions.extend(native["exceptions"])
    records.append({"part": part, "version": version, "after": after,
                    "through_observed": native["last_main_member"], "counts": native["counts"],
                    "output_identity": pins[str(output)], "native_receipt": pins[str(receipt)],
                    "guard_receipt": pins[str(guard_path)]})
    after = native["last_main_member"]

for path, pin in pins.items():
    if identity(Path(path)) != pin:
        raise RuntimeError("Evidence/source changed in second pass")
result = {
    "status": "BOUNDED_INVENTORY_CHAIN_BYTES_AND_EXECUTION_VERIFIED",
    "partitions": records, "counts": counts, "unique_cik_accessions": len(seen),
    "duplicate_cik_accession_rows": counts["filings"] - len(seen), "exceptions": exceptions,
    "last_main_member": after, "terminal_exhaustion": False, "complete_source_qualified": False,
    "availability_clocks_qualified": False, "pins": pins,
    "qualification": "Three bounded native inventory outputs only, with successful intentional-exit2 receipts, current original archive/code and two-pass output/receipt identities. Full CIK/history/exception closure is missing. Submission acceptance strings are raw leads, not independently timezone-qualified public clocks. No original filing body, economic/cohort/holding or ownership feature acceptance is inferred. Interrupted part0001-v1 remains outside this chain.",
}
with out.open("x", encoding="utf-8") as stream:
    json.dump(result, stream, indent=2)
    stream.write("\n")
print(json.dumps({k: v for k, v in result.items() if k not in {"pins", "partitions"}}))
