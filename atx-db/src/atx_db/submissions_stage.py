"""Bounded retained Submissions extraction; no warehouse, pandas, or network.

The immutable plan pins the complete ZIP and its streamed directory. Each CIK
is normalized in recent-then-referenced-history order, matching the old loader's
first-accession rule. A failed CIK contributes dispositions, never partial rows.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import os
import re
import shutil
from collections import Counter
from pathlib import Path

from ._submissions_archive import SubmissionsArchive, _one_info
from .companyfacts_stage import _RowGroupWriter as _CompanyFactsRowGroupWriter, _file_sha256, _write_json_atomic

RULE_VERSION = "submissions-extract-v1"
ARCHIVE_SHA = "702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165"
OBSERVED_AT = "2026-09-20T00:07:35.073781"
TARGET_BYTES = 55_000_000
ITEM_202 = re.compile(r"(^|[^0-9])2\.02([^0-9]|$)")
SOURCE_COLUMNS = (
    ("cik", "string"), ("accession_number", "string"),
    ("filing_date", "date32"), ("report_date", "date32"),
    ("acceptance_datetime", "timestamp_us"), ("acceptance_datetime_raw", "string"),
    ("form", "string"), ("primary_document", "string"), ("primary_doc_description", "string"),
    ("file_number", "string"), ("film_number", "string"), ("items", "string"),
    ("size", "int64"), ("is_xbrl", "bool"), ("is_inline_xbrl", "bool"),
    ("act", "string"), ("source_url", "string"),
)
DIRECTORY_COLUMNS = (
    ("batch_id", "int64"), ("cik", "string"), ("member", "string"),
    ("kind", "int64"), ("crc32", "int64"), ("compressed_bytes", "int64"),
    ("uncompressed_bytes", "int64"),
)
MEMBER_COLUMNS = DIRECTORY_COLUMNS + (
    ("disposition", "string"), ("reason", "string"), ("rows", "int64"),
    ("referenced", "bool"), ("source_sha256", "string"),
)
CIK_COLUMNS = (
    ("batch_id", "int64"), ("cik", "string"), ("disposition", "string"),
    ("reason", "string"), ("rows", "int64"), ("history_members", "int64"),
    ("missing_history_members", "int64"), ("duplicate_accessions", "int64"),
)


class _RowGroupWriter:
    """Coalesce tiny CIK batches before the shared 131072-row Parquet buffer.

    A Submissions batch can contain tens of thousands of one-row CIK/member
    receipts. Bound the Arrow chunk objects as well as the number of rows;
    the CompanyFacts helper's numeric row bound alone does not cover that shape.
    This changes physical buffering only, preserving row order and values.
    """
    def __init__(self, path, arrow_schema):
        self._writer = _CompanyFactsRowGroupWriter(path, arrow_schema)
        self._schema = arrow_schema
        self._pending = []
        self._pending_rows = 0

    @property
    def rows(self):
        return self._writer.rows

    def add(self, batch):
        if batch.num_rows:
            self._pending.append(batch)
            self._pending_rows += batch.num_rows
            if self._pending_rows >= 4096 or len(self._pending) >= 64:
                self._flush()

    def _flush(self):
        import pyarrow as pa
        if self._pending:
            table = pa.Table.from_batches(self._pending, schema=self._schema).combine_chunks()
            self._pending, self._pending_rows = [], 0
            for batch in table.to_batches(max_chunksize=4096):
                self._writer.add(batch)

    def close(self):
        self._flush()
        self._writer.close()

    def abort(self):
        self._pending, self._pending_rows = [], 0
        self._writer.abort()


def schema(columns=SOURCE_COLUMNS):
    import pyarrow as pa
    types = {"string": pa.string(), "date32": pa.date32(), "timestamp_us": pa.timestamp("us"),
             "int64": pa.int64(), "bool": pa.bool_()}
    return pa.schema([(name, types[kind]) for name, kind in columns])


def _write_rows(writer, rows, columns):
    if rows:
        import pyarrow as pa
        writer.add(pa.RecordBatch.from_pylist(rows, schema=schema(columns)))


def code_pins():
    root = Path(__file__).parent
    return {name: _file_sha256(root / name) for name in (
        "submissions_stage.py", "_submissions_archive.py", "companyfacts_stage.py", "sec_submissions.py")}


def _disk(path, planned=0):
    if shutil.disk_usage(path).free < 35 * 1024**3 + planned:
        raise ValueError("submissions work would cross the 35 GiB disk reserve")


def _directory_groups(archive):
    group, current = [], None
    for name, kind, record in archive._connection().execute("SELECT name,kind,record FROM members ORDER BY name"):
        info = _one_info(record)
        cik = name[3:13] if kind in (1, 2) else None
        if group and cik != current:
            yield current, group
            group = []
        current = cik
        group.append({"cik": cik, "member": name, "kind": kind, "crc32": info.CRC,
                      "compressed_bytes": info.compress_size, "uncompressed_bytes": info.file_size})
    if group:
        yield current, group


def write_plan(archive_path: Path, out: Path, *, ciks=None, target_bytes=TARGET_BYTES):
    """Stream directory metadata; the JSON plan contains only small batch ranges."""
    archive_path, out = Path(archive_path).resolve(), Path(out).resolve()
    selected = None if ciks is None else sorted({f"{int(cik):010d}" for cik in ciks})
    if selected == [] or target_bytes < 1:
        raise ValueError("empty scope or invalid byte target")
    if (out / "plan.json").exists():
        plan = load_plan(out)
        if (plan["archive"]["path"] != str(archive_path) or plan["scope"]["ciks"] != selected
                or plan["target_bytes"] != target_bytes or _file_sha256(archive_path) != ARCHIVE_SHA):
            raise ValueError("submissions existing plan/request differs")
        return plan
    out.mkdir(parents=True, exist_ok=True)
    _disk(out, 256 * 1024**2)
    path = out / "directory.parquet"
    temporary = path.with_suffix(".parquet.tmp")
    writer = _RowGroupWriter(temporary, schema(DIRECTORY_COLUMNS))
    batches, pending, scope_seen = [], [], set()
    counts = Counter()
    batch = None
    with SubmissionsArchive(archive_path) as archive:
        if archive.sha256 != ARCHIVE_SHA:
            raise ValueError("submissions archive differs from the retained source")
        try:
            for cik, members in _directory_groups(archive):
                if selected is not None and cik not in selected:
                    continue
                if cik is not None and sum(row["kind"] == 1 for row in members) != 1:
                    raise ValueError(f"missing or duplicate main member: {cik}")
                size = sum(row["uncompressed_bytes"] for row in members)
                if batch is None or (batch["members"] and batch["uncompressed_bytes"] + size > target_bytes):
                    batch = {"batch_id": len(batches), "first_cik": cik, "last_cik": cik,
                             "main_members": 0, "members": 0, "uncompressed_bytes": 0}
                    batches.append(batch)
                if cik is not None:
                    batch["first_cik"] = batch["first_cik"] or cik
                    batch["last_cik"] = cik
                    scope_seen.add(cik) if selected is not None else None
                for row in members:
                    pending.append({"batch_id": batch["batch_id"], **row})
                    counts[str(row["kind"])] += 1
                    batch["main_members"] += int(row["kind"] == 1)
                    batch["members"] += 1
                batch["uncompressed_bytes"] += size
                if len(pending) >= 1000:
                    _write_rows(writer, pending, DIRECTORY_COLUMNS)
                    pending.clear()
            _write_rows(writer, pending, DIRECTORY_COLUMNS)
            writer.close()
            archive.assert_unchanged()
        except BaseException:
            writer.abort()
            raise
        if selected is not None and scope_seen != set(selected):
            raise ValueError("selected CIKs are absent from the retained archive")
        archive_info = {"path": str(archive_path), "sha256": archive.sha256,
            "bytes": archive_path.stat().st_size, "entries": archive.entry_count,
            "unique_members": archive.unique_member_count, "main_members": archive.main_member_count,
            "history_members": archive.history_member_count, "uncompressed_bytes": archive.expanded_bytes,
            "observed_at": OBSERVED_AT, "observation_basis": "file_mtime"}
    os.replace(temporary, path)
    plan = {"rule_version": RULE_VERSION, "code_files": code_pins(), "archive": archive_info,
            "scope": {"ciks": selected, "all_forms": True, "include_history": True},
            "target_bytes": target_bytes, "batches": batches, "member_kinds": dict(counts),
            "directory": {"file": path.name, "sha256": _file_sha256(path), "rows": writer.rows},
            "normalization": "legacy recent-then-history, first accession per CIK; no current identity in extract"}
    _write_json_atomic(out / "plan.json", plan)
    return plan


def load_plan(out):
    out = Path(out)
    plan = json.loads((out / "plan.json").read_text(encoding="utf-8"))
    if (plan["rule_version"] != RULE_VERSION or plan["code_files"] != code_pins()
            or plan["archive"]["sha256"] != ARCHIVE_SHA
            or _file_sha256(out / plan["directory"]["file"]) != plan["directory"]["sha256"]):
        raise ValueError("submissions plan/source/directory pin drift")
    return plan


def batch_receipt(out, batch_id, plan=None):
    out = Path(out)
    path = out / f"batch-{batch_id:04d}.json"
    if not path.exists():
        return None
    plan = plan or load_plan(out)
    receipt = json.loads(path.read_text(encoding="utf-8"))
    if (receipt["plan_sha256"] != _file_sha256(out / "plan.json")
            or receipt["batch"] != plan["batches"][batch_id]):
        raise ValueError("submissions completed batch has different pins")
    for item in receipt["files"].values():
        target = out / item["file"]
        if not target.is_file() or _file_sha256(target) != item["sha256"]:
            raise ValueError("submissions completed batch file drift")
    return receipt


def _date(value):
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def _acceptance(value):
    if not value:
        return None
    if not isinstance(value, str):
        raise ValueError("non-string acceptanceDateTime needs explicit legacy parity")
    try:
        result = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return result.astimezone(dt.UTC).replace(tzinfo=None) if result.tzinfo else result
    except ValueError:
        return None


def _string(value, *, coercing=False):
    if value is None:
        return None
    if isinstance(value, str):
        return value.strip()
    if coercing and isinstance(value, (int, float, bool)):
        return str(value).strip()
    raise ValueError("unsupported non-string submission descriptor")


def _size(value):
    if value is None or value == "":
        return None
    try:
        number = float(value)
    except (ValueError, TypeError):
        return None
    if not math.isfinite(number):
        return None
    integer = int(value) if isinstance(value, (str, int)) and re.fullmatch(r"[-+]?\d+", str(value)) else int(number)
    if number != integer or not -(2**63) <= integer < 2**63:
        raise ValueError("submission size cannot be represented by legacy nullable BIGINT")
    return integer


def normalized_rows(columnar, cik, source_url):
    if not columnar:
        return
    if not isinstance(columnar, dict) or any(not isinstance(v, list) for v in columnar.values()):
        raise ValueError("malformed columnar submissions")
    count = len(next(iter(columnar.values())))
    if any(len(v) != count for v in columnar.values()):
        raise ValueError("unequal submission column lengths")
    for index in range(count):
        def value(name):
            return columnar[name][index] if name in columnar else None
        accession = _string(value("accessionNumber"))
        if not accession:
            raise ValueError("missing accession key")
        def flag(name):
            raw = value(name)
            return None if raw is None or raw == "" else bool(int(raw))
        yield {"cik": cik, "accession_number": accession,
            "filing_date": _date(value("filingDate")), "report_date": _date(value("reportDate")),
            "acceptance_datetime": _acceptance(value("acceptanceDateTime")),
            "acceptance_datetime_raw": _string(value("acceptanceDateTime"), coercing=True),
            "form": _string(value("form")), "primary_document": _string(value("primaryDocument")),
            "primary_doc_description": _string(value("primaryDocDescription")),
            "file_number": _string(value("fileNumber"), coercing=True),
            "film_number": _string(value("filmNumber"), coercing=True),
            "items": _string(value("items"), coercing=True), "size": _size(value("size")),
            "is_xbrl": flag("isXBRL"), "is_inline_xbrl": flag("isInlineXBRL"),
            "act": _string(value("act"), coercing=True), "source_url": source_url}


def _extract_cik(archive, members, out, batch_id):
    """A temporary CIK Parquet prevents partial rows escaping a failed history member."""
    cik = members[0]["cik"]
    meta = {row["member"]: row for row in members}
    main = f"CIK{cik}.json"
    temp = out / f".batch-{batch_id:04d}-cik-{cik}.parquet.tmp"
    writer = _RowGroupWriter(temp, schema())
    seen, consumed, receipts, pending = set(), set(), [], []
    duplicates = rows = candidates = missing = 0
    status, reason = "empty", "no_filings"
    try:
        raw = archive.read(main)
        payload = json.loads(raw)
        if not isinstance(payload, dict):
            raise ValueError("main submission member is not an object")
        if payload.get("cik") is not None and int(payload["cik"]) != int(cik):
            raise ValueError("main submission CIK disagrees with archive member")
        filings = payload.get("filings") or {}
        if not isinstance(filings, dict):
            raise ValueError("filings is not an object")
        references = []
        for item in filings.get("files") or []:
            name = item.get("name")
            if not name:
                continue
            if not re.fullmatch(rf"CIK{cik}-submissions-\d+\.json", name):
                raise ValueError("history reference crosses the source CIK or namespace")
            if name not in meta:
                missing += 1
            references.append(name)
        if len(set(references)) != len(references):
            raise ValueError("duplicate history reference")
        for name in (main, *references):
            if name not in meta:
                continue
            if name != main:
                raw = archive.read(name)
                columnar = json.loads(raw)
            else:
                columnar = filings.get("recent") or {}
            consumed.add(name)
            before = rows
            for row in normalized_rows(columnar, cik, f"{archive.path}!{name}"):
                if row["accession_number"] in seen:
                    duplicates += 1
                    continue
                seen.add(row["accession_number"])
                pending.append(row)
                rows += 1
                candidates += int((row["form"] or "").upper() == "8-K" and bool(ITEM_202.search(row["items"] or "")))
                if len(pending) >= 1000:
                    _write_rows(writer, pending, SOURCE_COLUMNS)
                    pending.clear()
            receipts.append({**meta[name], "disposition": "read", "reason": None,
                "rows": rows-before, "referenced": True, "source_sha256": hashlib.sha256(raw).hexdigest()})
        if missing:
            status, reason = "missing_history", f"missing_referenced_members={missing}"
        elif set(meta) != consumed:
            status, reason = "error", "unreferenced_history_member"
        else:
            status = "loaded" if rows else "empty"
            reason = None if rows else "no_filings"
        _write_rows(writer, pending, SOURCE_COLUMNS)
        writer.close()
    except Exception as exc:
        writer.abort()
        status, reason = "error", f"{type(exc).__name__}: {str(exc)[:240]}"
    for name, item in meta.items():
        if name not in {row["member"] for row in receipts}:
            raw = archive.read(name)  # CRC proof remains required for every present member.
            receipts.append({**item, "disposition": "retained_unloaded", "reason": reason,
                "rows": 0, "referenced": name in consumed, "source_sha256": hashlib.sha256(raw).hexdigest()})
    if status not in ("loaded", "empty"):
        rows = candidates = 0
        for receipt in receipts:
            receipt.update(disposition="retained_unloaded", reason=reason, rows=0)
    return temp, receipts, {"batch_id": batch_id, "cik": cik, "disposition": status,
        "reason": reason, "rows": rows, "history_members": len(meta)-1,
        "missing_history_members": missing, "duplicate_accessions": duplicates}, candidates


def extract_batch(out, batch_id):
    import pyarrow.parquet as pq
    out = Path(out).resolve()
    plan = load_plan(out)
    previous = batch_receipt(out, batch_id, plan)
    if previous is not None:
        return {**previous, "reused": True}
    batch = plan["batches"][batch_id]
    _disk(out, batch["uncompressed_bytes"] * 2 + 256 * 1024**2)
    members = pq.read_table(out / "directory.parquet", filters=[("batch_id", "=", batch_id)],
                            use_threads=False).to_pylist()
    paths = {kind: out / f"batch-{batch_id:04d}{suffix}.parquet" for kind, suffix in
             (("rows", ""), ("members", "-members"), ("ciks", "-ciks"))}
    columns = {"rows": SOURCE_COLUMNS, "members": MEMBER_COLUMNS, "ciks": CIK_COLUMNS}
    writers = {kind: _RowGroupWriter(path.with_suffix(".parquet.tmp"), schema(columns[kind]))
               for kind, path in paths.items()}
    counts, candidates, total_members = Counter(), 0, 0
    try:
        with SubmissionsArchive(Path(plan["archive"]["path"])) as archive:
            if archive.sha256 != plan["archive"]["sha256"]:
                raise ValueError("submissions source changed")
            groups = {}
            for member in members:
                groups.setdefault(member["cik"], []).append(member)
            for cik, group in groups.items():
                if cik is None:
                    for item in group:
                        raw = archive.read(item["member"])
                        _write_rows(writers["members"], [{**item, "disposition": "non_cik_metadata",
                            "reason": "preserved_non_filing_member", "rows": 0, "referenced": False,
                            "source_sha256": hashlib.sha256(raw).hexdigest()}], MEMBER_COLUMNS)
                        total_members += 1
                    continue
                temp, receipts, disposition, item_count = _extract_cik(archive, group, out, batch_id)
                if disposition["disposition"] in ("loaded", "empty"):
                    for record_batch in pq.ParquetFile(temp).iter_batches(batch_size=4096, use_threads=False):
                        writers["rows"].add(record_batch)
                temp.unlink(missing_ok=True)  # Only the exact temporary CIK file created above.
                _write_rows(writers["members"], receipts, MEMBER_COLUMNS)
                _write_rows(writers["ciks"], [disposition], CIK_COLUMNS)
                counts[disposition["disposition"]] += 1
                candidates += item_count
                total_members += len(receipts)
            archive.assert_unchanged()
        for writer in writers.values():
            writer.close()
    except BaseException:
        for writer in writers.values():
            writer.abort()
        raise
    if total_members != batch["members"]:
        raise ValueError("submissions batch member denominator differs")
    files = {}
    for kind, path in paths.items():
        os.replace(path.with_suffix(".parquet.tmp"), path)
        files[kind] = {"file": path.name, "sha256": _file_sha256(path),
                       "bytes": path.stat().st_size, "rows": writers[kind].rows}
    receipt = {"rule_version": RULE_VERSION, "plan_sha256": _file_sha256(out / "plan.json"),
        "archive_sha256": ARCHIVE_SHA, "batch": batch, "files": files,
        "dispositions": dict(counts), "item_202_candidates": candidates, "reused": False}
    _write_json_atomic(out / f"batch-{batch_id:04d}.json", receipt)
    return receipt


def assemble(out):
    import pyarrow.parquet as pq
    out = Path(out).resolve()
    plan = load_plan(out)
    if _file_sha256(Path(plan["archive"]["path"])) != ARCHIVE_SHA:
        raise ValueError("submissions retained archive drift at assembly")
    receipts = [batch_receipt(out, batch["batch_id"], plan) for batch in plan["batches"]]
    if any(receipt is None for receipt in receipts):
        raise ValueError("submissions extract has incomplete batches")
    total_rows = total_members = total_ciks = candidates = missing = 0
    dispositions = Counter()
    members_path, ciks_path = out / "members.parquet", out / "ciks.parquet"
    writers = {"members": _RowGroupWriter(members_path.with_suffix(".parquet.tmp"), schema(MEMBER_COLUMNS)),
               "ciks": _RowGroupWriter(ciks_path.with_suffix(".parquet.tmp"), schema(CIK_COLUMNS))}
    try:
        for receipt in receipts:
            total_rows += receipt["files"]["rows"]["rows"]
            total_members += receipt["files"]["members"]["rows"]
            total_ciks += receipt["files"]["ciks"]["rows"]
            candidates += receipt["item_202_candidates"]
            dispositions.update(receipt["dispositions"])
            for kind in writers:
                for chunk in pq.ParquetFile(out / receipt["files"][kind]["file"]).iter_batches(batch_size=4096, use_threads=False):
                    writers[kind].add(chunk)
                    if kind == "ciks":
                        missing += sum(chunk.column(chunk.schema.get_field_index("missing_history_members")).to_pylist())
        for writer in writers.values():
            writer.close()
    except BaseException:
        for writer in writers.values():
            writer.abort()
        raise
    if total_members != plan["directory"]["rows"] or total_ciks != sum(b["main_members"] for b in plan["batches"]):
        raise ValueError("submissions assembled denominator differs")
    for path in (members_path, ciks_path):
        os.replace(path.with_suffix(".parquet.tmp"), path)
    complete = (plan["scope"]["ciks"] is None and total_members == plan["archive"]["unique_members"]
                and total_ciks == plan["archive"]["main_members"] and missing == 0
                and not any(dispositions[key] for key in ("error", "missing_history", "unavailable")))
    result = {"rule_version": RULE_VERSION, "plan_sha256": _file_sha256(out / "plan.json"),
        "archive": plan["archive"], "scope_complete": complete, "rows": total_rows,
        "members_read": total_members, "main_members": total_ciks, "missing_history_members": missing,
        "dispositions": dict(dispositions), "item_202_candidates": candidates,
        "members_sha256": _file_sha256(members_path), "ciks_sha256": _file_sha256(ciks_path),
        "batches": [{"receipt": f"batch-{i:04d}.json", "sha256": _file_sha256(out / f"batch-{i:04d}.json"),
                     "files": receipt["files"]} for i, receipt in enumerate(receipts)]}
    _write_json_atomic(out / "manifest.json", result)
    return result
