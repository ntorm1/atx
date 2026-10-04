"""Bounded retained-source discovery; metadata is never a completeness certificate.

No network or legacy archive imports. Explicit roots include configured overrides;
unresolved historical CLI overrides must remain release exceptions.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sqlite3
from pathlib import Path
from typing import Any, TextIO

from . import common as C


def configured_roots() -> dict[str, str]:
    package = Path(__file__).resolve().parents[3]
    configured = os.environ.get("ATX_DB_DATA_DIR")
    database = os.environ.get("ATX_DB_PATH")
    warehouse = (Path(os.path.expandvars(configured)).expanduser() if configured else
                 Path(os.path.expandvars(database)).expanduser().parent if database else package / "data")
    return {
        "repository_data": str(package.parent / "data"),
        "package_data": str(package / "data"),
        "resolved_warehouse_data": str(warehouse),
        "ATX_ALPHA_PANEL_ROOT": os.environ.get("ATX_ALPHA_PANEL_ROOT", str(C.DEFAULT_ROOT)),
    }


def audit_roots(roots: list[Path], *, max_files: int = 200000,
                max_depth: int = 16, metadata_sink: TextIO | None = None) -> dict[str, Any]:
    """Stream directory metadata, skip links/reparse points, stop on explicit bounds.

    Every regular file is counted even without a 13F-looking name. Candidate
    paths are retained separately for subsequent guarded content qualification.
    Permission failures, bounds and unresolved roots forbid completeness claims.
    """
    if max_files < 1 or max_depth < 1:
        raise ValueError("positive traversal bounds required")
    result: dict[str, Any] = {"schema": "atx.thirteenf-source-roots/v1",
        "available_at": dt.datetime.now(dt.UTC).isoformat(), "roots": [],
        "candidate_files": [], "exceptions": [], "files": 0, "bytes": 0,
        "source_completeness": "UNMEASURED", "unresolved_historical_cli_overrides": True}
    seen: set[str] = set()
    for root in roots:
        absolute = root.absolute()
        key = os.path.normcase(str(absolute))
        if key in seen:
            continue
        seen.add(key)
        record = {"path": str(absolute), "exists": absolute.exists(), "files": 0, "bytes": 0}
        result["roots"].append(record)
        if not record["exists"]:
            result["exceptions"].append({"path": str(absolute), "reason": "missing_root"})
            continue
        stack = [(absolute, 0)]
        while stack:
            directory, depth = stack.pop()
            try:
                with os.scandir(directory) as entries:
                    for entry in entries:
                        info = entry.stat(follow_symlinks=False)
                        # FILE_ATTRIBUTE_REPARSE_POINT includes Windows junctions.
                        if entry.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                            result["exceptions"].append({"path": entry.path, "reason": "reparse_point_skipped"})
                            continue
                        if entry.is_dir(follow_symlinks=False):
                            if depth >= max_depth:
                                result["exceptions"].append({"path": entry.path, "reason": "depth_bound"})
                            else:
                                stack.append((Path(entry.path), depth + 1))
                        elif entry.is_file(follow_symlinks=False):
                            if result["files"] >= max_files:
                                result["exceptions"].append({"path": entry.path, "reason": "file_bound"})
                                result["metadata_complete"] = False
                                return result
                            result["files"] += 1
                            result["bytes"] += info.st_size
                            record["files"] += 1
                            record["bytes"] += info.st_size
                            if metadata_sink is not None:
                                metadata_sink.write(json.dumps({"path": entry.path,
                                    "bytes": info.st_size, "mtime_ns": info.st_mtime_ns}) + "\n")
                            name = entry.path.lower().replace("-", "_")
                            if "13f" in name or "thirteenf" in name:
                                result["candidate_files"].append({"path": entry.path,
                                    "bytes": info.st_size, "mtime_ns": info.st_mtime_ns,
                                    "kind": "receipt" if entry.name.endswith("jsonl") else "unqualified"})
            except OSError as exc:
                result["exceptions"].append({"path": str(directory), "reason": type(exc).__name__})
    result["metadata_complete"] = not result["exceptions"]
    return result


def inspect_submissions_cache(archive: Path, index: Path, receipt: Path) -> dict[str, Any]:
    """Read-only authenticated directory metadata, without opening ZIP members.

    Cache digest authenticates this locally generated cache against its retained
    receipt. Archive stat equality is checked, but archive contents are explicitly
    UNVERIFIED until a separately scheduled full source hash/member audit.
    """
    expected = receipt.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("invalid submissions directory digest receipt")
    actual = C.sha256_file(index)
    if actual != expected:
        raise ValueError("submissions directory cache digest mismatch")
    before = C.file_identity(index)
    con = sqlite3.connect(index.resolve().as_uri() + "?mode=ro&immutable=1", uri=True)
    try:
        con.execute("PRAGMA query_only=ON")
        con.execute("PRAGMA cache_size=-8192")
        metadata = dict(con.execute("SELECT key,value FROM metadata LIMIT 65"))
        if len(metadata) > 64:
            raise ValueError("unexpected directory metadata size")
        identity = json.loads(metadata["identity"])
        stat = archive.stat()
        if identity["source_stat"] != [stat.st_dev,stat.st_ino,stat.st_size,stat.st_mtime_ns]:
            raise ValueError("submissions archive stat identity differs from directory")
        counts = dict(con.execute("SELECT kind,count(*) FROM members GROUP BY kind"))
        sample = list(con.execute("SELECT name,kind FROM members WHERE kind IN(1,2) ORDER BY kind,name LIMIT 8"))
        schema = list(con.execute("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name"))
        if C.file_identity(index) != before or C.sha256_file(index) != actual:
            raise RuntimeError("directory changed during read-only inspection")
        return {"schema": "atx.thirteenf-submissions-cache-inspection/v1",
            "archive": C.file_identity(archive), "archive_expected_sha256": identity["source_sha256"],
            "archive_content_verified": False, "cache": before,"cache_sha256": actual,
            "cache_digest_receipt": C.file_identity(receipt),"metadata": metadata,
            "member_counts_by_kind": counts,"sample_names": sample,"tables": schema,
            "source_completeness": "UNMEASURED", "available_at": dt.datetime.now(dt.UTC).isoformat()}
    finally:
        con.close()


def inspect_ledgers(files:list[Path], *, max_records:int=100000,
                    max_bytes:int=64*1024*1024, max_record_bytes:int=256*1024,
                    max_candidates:int=2000) -> dict[str,Any]:
    """Bounded receipt metadata leads; never read, hash or certify objects.

    Opaque accession URLs without a form stay UNKNOWN, never treated as proof of
    non-13F content. Candidate truncation and input truncation are explicit.
    """
    if not 1<=len(files)<=2 or len(set(map(str,files)))!=len(files):
        raise ValueError("one or two unique explicit ledgers required")
    if not 1<=max_records<=100000 or not 1<=max_bytes<=64*1024*1024 or not 1<=max_record_bytes<=256*1024 or not 1<=max_candidates<=2000:
        raise ValueError("ledger inspection bounds invalid")
    reports=[]
    for path in files:
        before=C.file_identity(path)
        records=bytes_read=unknown=matched=0
        candidates=[]
        known_leads=[]
        exceptions=[]
        complete=True
        with path.open("rb") as stream:
            while True:
                if records>=max_records or bytes_read>=max_bytes:
                    if stream.read(1):
                        exceptions.append({"reason":"record_or_byte_bound"})
                        complete=False
                    break
                line=stream.readline(min(max_record_bytes+1,max_bytes-bytes_read+1))
                if not line:
                    break
                bytes_read+=len(line)
                records+=1
                if len(line)>max_record_bytes or bytes_read>max_bytes:
                    exceptions.append({"record":records,"reason":"record_or_byte_bound"})
                    complete=False
                    break
                try:
                    record=json.loads(line)
                    if not isinstance(record,dict):
                        raise ValueError("object required")
                except (ValueError,UnicodeError) as exc:
                    exceptions.append({"record":records,"reason":"malformed_record","detail":str(exc)[:160]})
                    complete=False
                    continue
                metadata={}
                def collect(obj,prefix="",depth=0):
                    if depth>3 or len(metadata)>=32:
                        return
                    for key,value in obj.items():
                        if len(metadata)>=32:
                            break
                        label=(prefix+str(key))[:128]
                        if isinstance(value,dict):
                            collect(value,label+".",depth+1)
                        elif re.search(r"url|path|form|accession|sha256|digest|status|fetch|retriev|object|cache",str(key),re.I) and isinstance(value,(str,int,float,bool,type(None))):
                            metadata[label]=value[:512] if isinstance(value,str) else value
                collect(record)
                forms=[str(v).upper().strip() for k,v in metadata.items() if re.search(r"(^|\.)form$",k,re.I)]
                is_13f=any(v in {"13F-HR","13F-HR/A","13F-NT","13F-NT/A"} for v in forms) or any(re.search(r"13f(?:[-_/ ]|$)",str(v),re.I) for v in metadata.values())
                classification="13F_LEAD" if is_13f else "OTHER_FORM" if forms and all(v in {"10-K","10-K/A","10-Q","10-Q/A","8-K","8-K/A","3","3/A","4","4/A","5","5/A","NPORT-P","NPORT-P/A"} for v in forms) else "UNKNOWN"
                matched+=is_13f
                unknown+=classification=="UNKNOWN"
                candidate={"record":records,"classification":classification,"metadata":metadata,
                           "referenced_paths_status":"UNVALIDATED_CONFIGURED_ROOTS"}
                if is_13f and len(known_leads)<256:
                    known_leads.append(candidate)
                if classification!="OTHER_FORM" and len(candidates)<max_candidates:
                    candidates.append(candidate)
        if C.file_identity(path)!=before:
            raise RuntimeError("ledger changed during inspection")
        reports.append({"file":before,"records_read":records,"bytes_read":bytes_read,
            "13f_lead_records":matched,"unknown_records":unknown,"candidates":candidates,
            "known_13f_leads":known_leads,"known_13f_lead_bound_reached":matched>len(known_leads),
            "candidate_bound_reached":matched+unknown>len(candidates),
            "scan_complete":complete,"exceptions":exceptions})
    return {"schema":"atx.thirteenf-ledger-leads/v1","files":reports,
        "source_completeness":"UNMEASURED","objects_read":0,"network_attempts":0,
        "limits":"Receipt metadata alone cannot establish original retention or whole-store absence",
        "available_at":dt.datetime.now(dt.UTC).isoformat()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    audit = sub.add_parser("audit-roots")
    audit.add_argument("--data-root", type=Path, action="append", required=True)
    audit.add_argument("--output", type=Path, required=True)
    audit.add_argument("--max-files", type=int, default=200000)
    audit.add_argument("--max-depth", type=int, default=16)
    audit.add_argument("--file-inventory", type=Path,
                       help="Streaming JSONL metadata for every file, including opaque names")
    schema = sub.add_parser("schemas")
    schema.add_argument("--file", type=Path, action="append", required=True)
    schema.add_argument("--output", type=Path, required=True)
    directory = sub.add_parser("inspect-submissions-cache")
    for name in ("archive", "index", "receipt", "output"):
        directory.add_argument("--" + name, type=Path, required=True)
    leads=sub.add_parser("inspect-ledgers")
    leads.add_argument("--file",type=Path,action="append",required=True)
    leads.add_argument("--output",type=Path,required=True)
    leads.add_argument("--max-records",type=int,default=100000)
    leads.add_argument("--max-bytes",type=int,default=64*1024*1024)
    leads.add_argument("--max-candidates",type=int,default=2000)
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.error("audit output exists; choose a new evidence path")
    if args.command == "inspect-ledgers":
        result=inspect_ledgers(args.file,max_records=args.max_records,max_bytes=args.max_bytes,max_candidates=args.max_candidates)
        C.write_json_atomic(args.output,result)
        return 0 if all(item["scan_complete"] for item in result["files"]) else 2
    if args.command == "inspect-submissions-cache":
        C.write_json_atomic(args.output, inspect_submissions_cache(args.archive,args.index,args.receipt))
        return 0
    if args.command == "schemas":
        if len(args.file) > 8:
            parser.error("at most eight explicit source files")
        con = C.connect(memory="64MB", threads=1)
        try:
            records = []
            for path in args.file:
                escaped = path.as_posix().replace("'", "''")
                rows = con.execute(f"DESCRIBE SELECT * FROM read_parquet('{escaped}')").fetchall()
                records.append({"file": C.file_identity(path), "columns": rows})
            C.write_json_atomic(args.output, {"schema": "atx.thirteenf-source-schemas/v1", "files": records})
        finally:
            con.close()
        return 0
    if args.file_inventory:
        if args.file_inventory.exists():
            parser.error("file inventory exists; choose a new evidence path")
        args.file_inventory.parent.mkdir(parents=True, exist_ok=True)
        # Open exclusively: never overwrite prior audit evidence.
        with args.file_inventory.open("x", encoding="utf-8") as sink:
            result = audit_roots(args.data_root, max_files=args.max_files,
                                 max_depth=args.max_depth, metadata_sink=sink)
        result["file_inventory"] = str(args.file_inventory)
        result["file_inventory_sha256"] = C.sha256_file(args.file_inventory)
        result["file_inventory_bytes"] = args.file_inventory.stat().st_size
    else:
        result = audit_roots(args.data_root, max_files=args.max_files, max_depth=args.max_depth)
    result["configured_roots"] = configured_roots()
    C.write_json_atomic(args.output, result)
    return 0 if result["metadata_complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
