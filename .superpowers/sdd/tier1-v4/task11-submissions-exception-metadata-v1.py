"""Inspect the two retained inventory exceptions; no payload or gate admission."""
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get("ATX_GUARD_JOB"):
    raise RuntimeError("External memory guard required")

base = Path(__file__).resolve().parent
repo = base.parents[2]
out = base / "task11-submissions-exception-metadata-v1.json"
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
audit = base / "task11-submissions-progress-v4.json"
code = repo / "atx-db/src/atx_db/_submissions_archive.py"
pins = {str(audit): identity(audit), str(code): identity(code)}
if pins[str(code)]["sha256"] != "12d48c0334fa0013726200967809f9fb0cb2eee75113debd8cd78d0eba89f09f":
    raise RuntimeError("Native archive reader changed")
prior = json.loads(audit.read_text(encoding="utf-8"))
expected_exceptions = [
    {"cik": 19617, "member": "CIK0000019617.json", "reason": "submission member exceeds explicit byte bound"},
    {"cik": 1665650, "member": "CIK0001665650.json", "reason": "submission member exceeds explicit byte bound"},
]
if prior["exceptions"] != expected_exceptions or not prior["terminal_exhaustion"]:
    raise RuntimeError("Inventory exception frontier differs")

from atx_db._submissions_archive import SubmissionsArchive, _one_info


class ReadOnlyArchive(SubmissionsArchive):
    def _build_index(self):
        raise RuntimeError("Missing/corrupt directory; this audit never rebuilds it")


records = []
with ReadOnlyArchive(archive) as source:
    if source.sha256 != archive_expected["sha256"] or archive.stat().st_size != archive_expected["bytes"]:
        raise RuntimeError("Original archive changed")
    pins[str(archive)] = archive_expected
    for path in (source.index_path, source._receipt_path):
        pins[str(path)] = identity(path)
    for exception in expected_exceptions:
        prefix = exception["member"][:-5]
        members = []
        cursor = source._connection().execute(
            "SELECT name, kind, central_offset, header_offset, record FROM members "
            "WHERE name=? OR (kind=2 AND substr(name,1,?)=?) ORDER BY name",
            (exception["member"], len(prefix) + 1, prefix + "-"),
        )
        for name, kind, central_offset, header_offset, central_record in cursor:
            if len(members) >= 1000:
                raise RuntimeError("Unexpected exception history metadata population")
            source._source.seek(central_offset)
            if source._source.read(len(central_record)) != central_record:
                raise RuntimeError("Directory record differs from original archive")
            info = _one_info(central_record)
            if info.filename != name or info.header_offset + source._concat != header_offset:
                raise RuntimeError("Directory member identity differs")
            members.append({"name": name, "kind": kind, "expanded_bytes": info.file_size,
                            "compressed_bytes": info.compress_size, "crc32": info.CRC,
                            "central_record_sha256": hashlib.sha256(central_record).hexdigest()})
        cursor.close()
        main = [m for m in members if m["kind"] == 1]
        history = [m for m in members if m["kind"] == 2]
        if len(main) != 1 or main[0]["expanded_bytes"] <= 4 * 1024 * 1024:
            raise RuntimeError("Expected oversized main member missing")
        records.append({"cik": exception["cik"], "main": main[0],
                        "history_directory_members": history,
                        "history_directory_count": len(history),
                        "history_expanded_bytes": sum(m["expanded_bytes"] for m in history),
                        "largest_history_expanded_bytes": max((m["expanded_bytes"] for m in history), default=0)})
    source.assert_unchanged()

for path, pin in pins.items():
    if identity(Path(path)) != pin:
        raise RuntimeError("Metadata/source changed during inspection")

result = {
    "schema": "atx.controller.submissions-exception-metadata/v1",
    "status": "ORIGINAL_DIRECTORY_METADATA_VERIFIED",
    "records": records, "pins": pins,
    "source_payloads_read": False, "exception_closure": False,
    "history_reference_closure": False, "source_qualified": False, "gate_promotion": False,
    "qualification": "Only original ZIP directory records for the two oversized main files and CIK-prefix history names were inspected. Prefix history membership does not prove the main document references, completeness, public clocks, filing bodies, cohort or ownership qualification. The frozen 4 MiB inventory bound is unchanged. No SEC HTTP was performed.",
}
with out.open("x", encoding="utf-8") as stream:
    json.dump(result, stream, indent=2)
    stream.write("\n")
print(json.dumps({"status": result["status"], "exceptions": [
    {"cik": row["cik"], "main_expanded_bytes": row["main"]["expanded_bytes"],
     "history_directory_count": row["history_directory_count"],
     "largest_history_expanded_bytes": row["largest_history_expanded_bytes"]}
    for row in records], "gate_promotion": False}))
