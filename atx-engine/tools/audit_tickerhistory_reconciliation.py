#!/usr/bin/env python3
"""Read-only, bounded reconciliation of a failed identified training book.

No price repair, accepted-input publication, strategy metrics or source mutation.
Only manifest.json marks completion. Raw evidence ZIP members are NOT accepted data.
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import zipfile

import prepare_tickerhistory as qa

MAX_JSON_BYTES = 16 << 20
MAX_GAPS = 10_000
MAX_EVIDENCE_ROWS = 20_000
MAX_EVIDENCE_BYTES = 16 << 20  # Raw retained original AND accepted bytes, combined.
DAY_NS = 86_400_000_000_000
FIELDS = ("dn", "open", "high", "low", "close", "volume", "shares", "closePr",
          "closeUnadjPr", "returnFactor", "totalReturn", "cumulReturnFactor")
GAP_SCHEMA = "iteration6-baseline-required-mark-coverage-v1"


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def strict_json(raw: bytes):
    if len(raw) > MAX_JSON_BYTES:
        raise ValueError("JSON exceeds 16 MiB")
    def bad_constant(value):
        raise ValueError("Nonfinite JSON constant: " + value)
    value = json.loads(raw, object_pairs_hook=unique_object, parse_constant=bad_constant)
    pending = [(value, 0)]
    while pending:
        item, depth = pending.pop()
        if depth > 64:
            raise ValueError("JSON depth exceeds 64")
        if isinstance(item, dict):
            pending.extend((child, depth + 1) for child in item.values())
        elif isinstance(item, list):
            pending.extend((child, depth + 1) for child in item)
    return value


def read_document(path: Path, receipts: dict, role: str):
    path = path.resolve(strict=True)
    with path.open("rb") as stream:
        raw = stream.read(MAX_JSON_BYTES + 1)
    value = strict_json(raw)
    receipts[role] = {"path": str(path), "sha256": digest(raw), "size_bytes": len(raw)}
    return value


def positive_string(value) -> int:
    if not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]{0,18}", value):
        raise ValueError("Invalid canonical security ID")
    result = int(value)
    if result > qa.I64_MAX:
        raise ValueError("Security ID exceeds i64")
    return result


def integer_string(value) -> int:
    if not isinstance(value, str) or not re.fullmatch(r"-?(0|[1-9][0-9]*)", value):
        raise ValueError("Invalid canonical integer string")
    result = int(value)
    if str(result) != value or not -(1 << 63) <= result <= qa.I64_MAX:
        raise ValueError("Integer outside canonical i64")
    return result


def session_date(value) -> str:
    nanos = integer_string(value)
    if nanos % DAY_NS:
        raise ValueError("Session label is not midnight UTC")
    return (dt.date(1970, 1, 1) + dt.timedelta(days=nanos // DAY_NS)).isoformat()


def parent_map(document):
    parents = {}
    for parent in document["parents"]:
        role, sha = parent["role"], parent["sha256"]
        if role in parents or not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError("Duplicate or malformed artifact parent")
        parents[role] = sha
    return parents


def identified(path: Path, receipts: dict, role: str):
    document = read_document(path, receipts, role + "_manifest")
    if document.get("schema") != "atx.panel-artifact" or document.get("schema_version") != 1:
        raise ValueError("Unsupported panel identity")
    body = {key: value for key, value in document.items() if key != "artifact_id"}
    if digest(b"atx-panel-artifact-v1\n" + canonical(body)) != document["artifact_id"]:
        raise ValueError("Panel artifact ID mismatch")
    axes = document["axes"]
    if axes["instrument_namespace"] != "spiderrock.securityID" or axes["date_encoding"] != "UnixNanoseconds":
        raise ValueError("Unsupported identity namespace/encoding")
    ids = axes["instrument_ids"]
    for sid in ids:
        positive_string(sid)
    keys = [integer_string(key) for key in axes["session_keys"]]
    mapping = [integer_string(key) for key in axes["original_instrument_indices"]]
    if (not ids or not keys or len(ids) != len(set(ids)) or any(a >= b for a, b in zip(keys, keys[1:]))
            or len(mapping) != len(ids) or min(mapping) < 0 or len(set(mapping)) != len(mapping)
            or int(document["shape"]["dates"]) != len(keys)
            or int(document["shape"]["instruments"]) != len(ids)):
        raise ValueError("Invalid identified axes")
    if not str(path).endswith(".manifest.json"):
        raise ValueError("Expected <payload>.manifest.json")
    payload = Path(str(path)[:-len(".manifest.json")]).resolve(strict=True)
    expected = document["payload"]
    if payload.stat().st_size != int(expected["size_bytes"]) or qa.sha256(payload) != expected["sha256"]:
        raise ValueError("Panel payload hash/size mismatch")
    receipts[role + "_payload"] = {"path": str(payload), "sha256": expected["sha256"],
                                   "size_bytes": payload.stat().st_size}
    parent_map(document)
    return document


def validate_inputs(options, receipts):
    prep = read_document(options.preparation_manifest, receipts, "preparation")
    gaps = read_document(options.gaps, receipts, "required_gaps")
    request = read_document(options.attempt_dir / "request.json", receipts, "attempt_request")
    failure = read_document(options.attempt_dir / "failure.json", receipts, "attempt_failure")
    context = identified(options.context_manifest, receipts, "context")
    evaluation = identified(options.evaluation_manifest, receipts, "evaluation")
    books = identified(options.books_manifest, receipts, "books")
    ingestion = read_document(options.ingestion_manifest, receipts, "ingestion")
    oracle = read_document(options.source_fidelity_receipt, receipts, "prior_source_fidelity")
    if prep.get("status") != "complete" or prep.get("policy_version") != qa.POLICY_VERSION:
        raise ValueError("Incomplete or unsupported preparation policy")
    start, end = prep["window"]["start_inclusive"], prep["window"]["end_inclusive"]
    qa.parse_date(start.encode()); qa.parse_date(end.encode())
    if start > end or end >= "2020-01-01":
        raise ValueError("Preparation must stay inside the declared training partition")
    recipe = strict_json(context["recipe"].encode())
    context_start = session_date(recipe["start_inclusive_nanos"])
    context_end = session_date(recipe["end_exclusive_nanos"])
    if start > context_start or end < (dt.date.fromisoformat(context_end) - dt.timedelta(days=1)).isoformat():
        raise ValueError("Preparation does not cover context")
    context_dates = [session_date(key) for key in context["axes"]["session_keys"]]
    if context_dates[0] < context_start or context_dates[-1] >= context_end:
        raise ValueError("Context axes exceed declared source window")
    cp = parent_map(context)
    expected_parents = {"preparation_manifest": receipts["preparation"]["sha256"],
        "preparation_declared_original_zip": prep["source"]["sha256"],
        "ingestion_input_zip": prep["accepted"]["sha256"],
        "ingestion_manifest": receipts["ingestion"]["sha256"]}
    if any(cp.get(role) != sha for role, sha in expected_parents.items()):
        raise ValueError("Context preparation/ingestion parents do not match")
    if (ingestion.get("schema") != "atx-ingestion-v1" or ingestion.get("status") != "complete"
            or ingestion["input"]["sha256"] != prep["accepted"]["sha256"]
            or ingestion["preparation"]["manifest_sha256"] != receipts["preparation"]["sha256"]
            or ingestion["preparation"]["original_source_sha256"] != prep["source"]["sha256"]):
        raise ValueError("Ingestion source chain mismatch")
    if (oracle.get("status") != "passed" or oracle["source"]["sha256"] != prep["accepted"]["sha256"]
            or oracle["panel"]["sha256"] != context["payload"]["sha256"]):
        raise ValueError("Prior source-fidelity receipt does not match actual inputs")
    if (request.get("status") != "started" or failure.get("status") != "failed"
            or request["recipe"] != failure["recipe"]
            or strict_json(evaluation["recipe"].encode()) != request["recipe"]):
        raise ValueError("Attempt/evaluation recipe mismatch")
    for attempt in (request, failure):
        if (attempt["source_context_artifact_id"] != context["artifact_id"]
                or attempt["source_context_payload_sha256"] != context["payload"]["sha256"]):
            raise ValueError("Attempt context identity mismatch")
    if parent_map(evaluation).get("source-context") != context["artifact_id"]:
        raise ValueError("Evaluation does not descend from context")
    if parent_map(books).get("research") != evaluation["artifact_id"]:
        raise ValueError("Books do not descend from evaluation")
    for artifact in (evaluation, books):
        for name in ("instrument_ids", "instrument_namespace", "original_instrument_indices"):
            if artifact["axes"][name] != context["axes"][name]:
                raise ValueError("Instrument axes changed across artifacts")
    evaluation_keys = evaluation["axes"]["session_keys"]
    if not set(evaluation_keys) <= set(context["axes"]["session_keys"]):
        raise ValueError("Evaluation dates absent from context")
    if not set(books["axes"]["session_keys"]) <= set(evaluation_keys):
        raise ValueError("Book decisions absent from evaluation")
    if (gaps.get("schema") != GAP_SCHEMA or not gaps.get("scope")
            or gaps["evaluation_artifact_id"] != evaluation["artifact_id"]
            or gaps["books_artifact_id"] != books["artifact_id"]):
        raise ValueError("Required-gap inventory has wrong schema or identities")
    rows = gaps["gaps"]
    if not 0 < len(rows) <= MAX_GAPS or len(rows) != gaps["missing_required_cells"]:
        raise ValueError("Required-gap count mismatch or bound exceeded")
    unique = set()
    for gap in rows:
        date, sid = gap["session_date_utc"], gap["security_id"]
        positive_string(sid); qa.parse_date(date.encode())
        d, c, i = gap["evaluation_observation"], gap["context_observation"], gap["instrument_index"]
        if any(type(index) is not int or index < 0 for index in (d, c, i)):
            raise ValueError("Invalid gap indices")
        if (evaluation_keys[d] != gap["session_key_ns"] or context["axes"]["session_keys"][c] != gap["session_key_ns"]
                or evaluation["axes"]["instrument_ids"][i] != sid or session_date(gap["session_key_ns"]) != date
                or not context_start <= date < context_end or (date, sid) in unique):
            raise ValueError("Gap date/ID axes mismatch or duplicate request")
        unique.add((date, sid))
    if set(gaps["affected_security_ids"]) != {sid for _, sid in unique}:
        raise ValueError("Affected ID inventory mismatch")
    if gaps["first_missing_required_cell"] != rows[0]:
        raise ValueError("First gap disagrees with full inventory")
    first = rows[0]
    diagnostic = re.search(r" at period=(0|[1-9][0-9]*) instrument=(0|[1-9][0-9]*) "
                           r"session_key_ns=(-?(?:0|[1-9][0-9]*)) security_id=([1-9][0-9]*)\Z",
                           failure["error"])
    expected = (str(first["evaluation_observation"]), str(first["instrument_index"]),
                first["session_key_ns"], first["security_id"])
    if diagnostic is None or diagnostic.groups() != expected:
        raise ValueError("Failure diagnostic is not bound to first requested gap")
    return prep, gaps, context_start, context_end


class EvidenceBudget:
    def __init__(self):
        self.rows = 0
        self.bytes = 0

    def retain(self, rows):
        self.rows += len(rows)
        self.bytes += sum(len(row["raw"]) for row in rows)
        if self.rows > MAX_EVIDENCE_ROWS or self.bytes > MAX_EVIDENCE_BYTES:
            raise ValueError("Retained evidence bound exceeded; no keys or duplicates may be truncated")


def scan_archive(path, requested_ids, start, end_exclusive, consume_group):
    """Only one requested-ID date group plus framing buffers is materialized."""
    stats = {"framed_data_rows": 0, "processed_through_date": None, "member_crc_verified": False,
             "scope": "date-ordered prefix through declared context; framing may read ahead"}
    with zipfile.ZipFile(path) as archive:
        members = [member for member in archive.infolist() if not member.is_dir()]
        if len(members) != 1 or "tbltickerhistory" not in members[0].filename:
            raise ValueError("Expected one TickerHistory source member")
        member = members[0]
        stats.update(member=member.filename, member_declared_crc32=f"{member.CRC:08x}",
                     member_uncompressed_bytes=member.file_size)
        with archive.open(member) as stream:
            framed = iter(qa.lines(stream))
            header = next(framed, b"")
            names = header.rstrip(b"\r\n").decode("ascii").split("\t")
            if (not names or names[0] != "tradingDate" or len(names) != len(set(names))
                    or any(name not in names for name in (*qa.REQUIRED, *FIELDS))):
                raise ValueError("Missing/duplicate reconciliation source fields")
            field = {name: i for i, name in enumerate(names)}
            current = None
            group = collections.defaultdict(list)
            date_bytes = 0
            def flush():
                for sid, records in group.items():
                    duplicates = {int(sid)} if len(records) > 1 else set()
                    for record in records:
                        reasons, flags = qa.classify_row(record["values"], field, int(sid), duplicates)
                        record.update(qa_reasons=reasons, qa_flags=flags)
                    consume_group((current, sid), records)
            for ordinal, raw in enumerate(framed, 1):
                stats["framed_data_rows"] = ordinal
                content = raw.rstrip(b"\r\n")
                if content.count(b"\t") + 1 != len(names):
                    raise ValueError(f"Bad source width at row {ordinal}")
                date = content.split(b"\t", 1)[0].decode("ascii")
                if date != current:
                    qa.parse_date(date.encode())
                    if current is not None and date < current:
                        raise ValueError("Non-monotonic source dates")
                    flush()
                    group = collections.defaultdict(list)
                    current, date_bytes = date, 0
                if end_exclusive is not None and date >= end_exclusive:
                    stats["first_unprocessed_boundary_date"] = date
                    break
                stats["processed_through_date"] = date
                if date < start:
                    continue
                date_bytes += len(raw)
                if date_bytes > qa.MAX_DATE_BYTES:
                    raise ValueError("Original date group exceeds byte bound")
                values = content.split(b"\t")
                parsed = qa.positive_id(values[field["securityID"]])
                sid = str(parsed) if parsed is not None else None
                if sid not in requested_ids:
                    continue
                group[sid].append({"raw": raw, "ordinal": ordinal, "values": values,
                                   "date": date, "security_id": sid, "sha256": digest(raw)})
            else:
                flush()
                stats["member_crc_verified"] = True
                stats["scope"] = "entire decompressed member consumed; ZIP CRC checked by zipfile"
    return header, field, stats


def select_original(source, gaps, start, end, budget):
    requests = collections.defaultdict(list)
    links = {}
    for gap in gaps:
        key = (gap["session_date_utc"], gap["security_id"])
        links[key] = {"previous": None, "current": None, "next": None}
        requests[key[1]].append(key)
    previous = {}
    selected = {}
    def save(key, rows):
        if key is not None and key not in selected:
            budget.retain(rows)
            selected[key] = rows
        return key
    def consume(key, rows):
        date, sid = key
        prior = previous.get(sid)
        for target in requests[sid]:
            link = links[target]
            if date == target[0]:
                link["current"] = save(key, rows)
                if prior is not None:
                    link["previous"] = save(*prior)
            elif date > target[0] and link["next"] is None:
                link["next"] = save(key, rows)
                if prior is not None and prior[0][0] < target[0]:
                    link["previous"] = save(*prior)
        previous[sid] = (key, rows)
        # Latest original groups are potential predecessors even before we know
        # which will be emitted. Count their union with saved evidence as well.
        live = {key: records for key, records in previous.values()}
        live.update(selected)
        live_rows = sum(len(records) for records in live.values())
        live_bytes = sum(len(row["raw"]) for records in live.values() for row in records)
        if live_rows > MAX_EVIDENCE_ROWS or live_bytes > MAX_EVIDENCE_BYTES:
            raise ValueError("Live predecessor/evidence bound exceeded; no truncation permitted")
    header, field, stats = scan_archive(source, set(requests), start, end, consume)
    for target, link in links.items():
        prior = previous.get(target[1])
        if link["current"] is None and link["next"] is None and prior is not None:
            link["previous"] = save(*prior)
    return header, field, stats, selected, links


def residuals(previous, current, field):
    result = {"status": "not_comparable", "economic_status": "unverified", "reasons": []}
    if len(previous) != 1 or len(current) != 1:
        result["reasons"] = ["missing-or-duplicate-original-session"]
        return result
    p, t = previous[0], current[0]
    def ordinal(row):
        text = row["values"][field["dn"]]
        if not re.fullmatch(rb"-?[0-9]{1,19}", text):
            return None
        value = int(text)
        return value if -(1 << 63) <= value <= qa.I64_MAX else None
    p_dn, t_dn = ordinal(p), ordinal(t)
    if p_dn is None or t_dn is None or t_dn != p_dn + 1 or p["date"] >= t["date"]:
        result["reasons"] = ["original-dn-not-consecutive-or-invalid"]
        return result
    result.update(status="diagnostic-only", previous_original_row=p["ordinal"], current_original_row=t["ordinal"],
                  previous_qa_reasons=p["qa_reasons"], current_qa_reasons=t["qa_reasons"], metrics={})
    def value(row, name):
        return qa.numeric(row["values"][field[name]])
    cp, c = value(p, "close"), value(t, "close")
    fp, f = value(p, "cumulReturnFactor"), value(t, "cumulReturnFactor")
    adjusted, unadjusted = value(t, "closePr"), value(t, "closeUnadjPr")
    q, reported = value(t, "returnFactor"), value(t, "totalReturn")
    definitions = {
        "prior_close_absolute": ([unadjusted, cp], [cp], lambda: unadjusted - cp),
        "prior_close_relative": ([unadjusted, cp], [cp], lambda: unadjusted / cp - 1),
        "daily_factor": ([adjusted, unadjusted, q], [unadjusted, adjusted, q], lambda: adjusted / unadjusted - q),
        "reported_return": ([c, adjusted, reported], [c, adjusted], lambda: c / adjusted - 1 - reported),
        "cumulative_daily_factor": ([f, fp, q], [f, fp, q], lambda: f / fp * q - 1),
        "adjusted_return": ([c, f, cp, fp, reported], [c, f, cp, fp], lambda: (c * f) / (cp * fp) - 1 - reported)}
    for name, (required, positive, calculate) in definitions.items():
        entry = {"status": "not_comparable", "residual": None}
        if any(v is None for v in required) or any(v is None or v <= 0 for v in positive):
            entry["reason"] = "invalid-or-nonpositive-input"
        elif name == "adjusted_return" and any(
                not math.isfinite(product) or product <= 0 for product in (c * f, cp * fp)):
            entry["reason"] = "nonfinite-or-unrepresentable-adjusted-price-product"
        else:
            try:
                answer = calculate()
                if not math.isfinite(answer):
                    raise ArithmeticError()
                entry = {"status": "diagnostic-only", "residual": answer,
                         "absolute_bin": next((f"le_{limit:g}" for limit in (1e-8, 1e-6, 1e-4, 1e-3)
                                               if abs(answer) <= limit), "gt_0.001")}
            except (ArithmeticError, OverflowError):
                entry["reason"] = "nonfinite-or-unrepresentable-arithmetic"
        result["metrics"][name] = entry
    return result


def record_metadata(record, field, evidence_row):
    return {"date": record["date"], "security_id": record["security_id"],
            "ticker_at_observation": record["values"][field["ticker_tk"]].decode("utf-8", "replace"),
            "source_data_row_ordinal": record["ordinal"], "row_sha256": record["sha256"],
            "evidence_data_row": evidence_row, "qa_reasons": record["qa_reasons"], "qa_flags": record["qa_flags"],
            "values": {name: record["values"][field[name]].decode("ascii", "replace") for name in FIELDS}}


def audit(options):
    output = options.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    receipts = {}
    try:
        prep, request, start, end = validate_inputs(options, receipts)
        source = options.source.resolve(strict=True)
        accepted_name = prep["accepted"]["filename"]
        if Path(accepted_name).name != accepted_name:
            raise ValueError("Preparation accepted filename must be a basename")
        accepted = options.preparation_manifest.resolve().parent / accepted_name
        for role, path, info in (("original_zip", source, prep["source"]),
                                 ("accepted_zip", accepted, prep["accepted"])):
            if path.stat().st_size != info["size_bytes"] or qa.sha256(path) != info["sha256"]:
                raise ValueError(role + " full compressed SHA256/size mismatch")
            receipts[role] = {"path": str(path), "sha256": info["sha256"], "size_bytes": info["size_bytes"]}
        budget = EvidenceBudget()
        header, field, original_scan, selected, links = select_original(source, request["gaps"], start, end, budget)
        if (original_scan["member"] != prep["source"]["member"]
                or original_scan["member_declared_crc32"] != prep["source"]["member_crc32"]):
            raise ValueError("Original ZIP member metadata disagrees with preparation")
        wanted = set(selected) | set(links)
        accepted_rows = {}
        def consume_accepted(key, rows):
            if key in wanted:
                budget.retain(rows)
                accepted_rows[key] = rows
        accepted_header, accepted_field, accepted_scan = scan_archive(
            accepted, {key[1] for key in wanted}, start, None, consume_accepted)
        if accepted_header != header or accepted_field != field:
            raise ValueError("Accepted header differs from original bytes")
        originals = sorted((row for rows in selected.values() for row in rows), key=lambda row: row["ordinal"])
        accepted_evidence = sorted((row for rows in accepted_rows.values() for row in rows), key=lambda row: row["ordinal"])
        evidence = output / "evidence.zip.partial"
        with zipfile.ZipFile(evidence, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for name, rows in (("original_rows.tsv", originals), ("accepted_rows.tsv", accepted_evidence)):
                with archive.open(qa.zip_info(name), "w", force_zip64=True) as stream:
                    stream.write(header)
                    for row in rows:
                        stream.write(row["raw"])
        row_numbers = {row["ordinal"]: i + 1 for i, row in enumerate(originals)}
        accepted_numbers = {row["ordinal"]: i + 1 for i, row in enumerate(accepted_evidence)}
        groups = []
        mismatches = 0
        for key in sorted(wanted):
            original = selected.get(key, [])
            kept = accepted_rows.get(key, [])
            expected = collections.Counter(row["sha256"] for row in original if not row["qa_reasons"])
            actual = collections.Counter(row["sha256"] for row in kept)
            agrees = expected == actual
            mismatches += not agrees
            groups.append({"date": key[0], "security_id": key[1], "accepted_matches_qa_v1_original_bytes": agrees,
                "original_rows": [record_metadata(row, field, row_numbers[row["ordinal"]]) for row in original],
                "accepted_rows": [record_metadata(row, field, accepted_numbers[row["ordinal"]]) for row in kept]})
        results = []
        counts = collections.Counter()
        for gap in request["gaps"]:
            key = (gap["session_date_utc"], gap["security_id"])
            link = links[key]
            original = selected.get(key, [])
            kept = accepted_rows.get(key, [])
            if not original:
                category = "source_row_absent" if not kept else "accepted_without_original_requires_investigation"
            elif all(row["qa_reasons"] for row in original):
                category = "quarantined_by_qa_v1" if not kept else "quarantine_disagrees_with_accepted"
            elif not kept:
                category = "qa_eligible_original_missing_from_accepted"
            elif collections.Counter(row["sha256"] for row in kept) != collections.Counter(
                    row["sha256"] for row in original if not row["qa_reasons"]):
                category = "accepted_bytes_disagree_with_original"
            else:
                category = "accepted_row_present_missing_panel_requires_investigation"
            counts[category] += 1
            previous = selected.get(link["previous"], [])
            following = selected.get(link["next"], [])
            results.append({"required_gap": gap, "classification": category,
                "economic_status": "unverified", "neighbors": link,
                "previous_to_current": residuals(previous, original, field),
                "current_to_next": residuals(original, following, field),
                "native_evidence_scope": "missing mark supplied by bound gap inventory; no native segment decoding in this audit"})
        # Verify every consumed input still has the same exact bytes before publication.
        for receipt in receipts.values():
            path = Path(receipt["path"])
            if path.stat().st_size != receipt["size_bytes"] or qa.sha256(path) != receipt["sha256"]:
                raise ValueError("Input changed during audit: " + str(path))
        tool_files = {"audit_tool_sha256": qa.sha256(Path(__file__)),
                      "shared_qa_tool_sha256": qa.sha256(Path(qa.__file__))}
        report = {"schema": "tickerhistory-reconciliation-audit-v1", "status": "complete",
            "purpose": "training-only-read-only-source-reconciliation-no-repair-no-performance",
            "inputs": receipts, "tools": tool_files, "policy_version": qa.POLICY_VERSION,
            "required_gap_scope": request["scope"], "required_gap_count": len(results),
            "classifications": dict(counts), "qa_original_accepted_group_mismatches": mismatches,
            "context_window": {"start_inclusive": start, "end_exclusive": end},
            "original_scan": original_scan, "accepted_scan": accepted_scan,
            "compressed_sha256_scope": "entire original and accepted ZIP bytes verified before and after audit",
            "bounds": {"max_retained_rows": MAX_EVIDENCE_ROWS, "max_retained_raw_bytes": MAX_EVIDENCE_BYTES,
                       "retained_rows": budget.rows, "retained_raw_bytes": budget.bytes,
                       "max_source_date_bytes": qa.MAX_DATE_BYTES, "max_line_bytes": qa.MAX_LINE_BYTES},
            "evidence": {"filename": "evidence.zip", "sha256": qa.sha256(evidence),
                         "size_bytes": evidence.stat().st_size,
                         "members": ["original_rows.tsv", "accepted_rows.tsv"], "is_accepted_input": False},
            "groups": groups, "gaps": results,
            "limits": ["Same-vendor residual agreement is not independent economic verification",
                "Cumulative inverse daily-factor recurrence is derived from the declared return construction",
                "Residual bins are diagnostics, not acceptance thresholds; no factor or price repair",
                "No source absence is interpreted as delisting, suspension, bankruptcy or zero return",
                "Prior source-fidelity receipt is reused by exact source/panel hash; native segment values not independently checked",
                "Required-gap inventory is validated against identities/axes, not recomputed from holdings in this tool",
                "Counterfactual support after the first failure is not continued replay or strategy performance",
                "Publication vintage, instrument classification, shares timing and borrow availability remain unknown"]}
        report["audit_id"] = digest(b"tickerhistory-reconciliation-audit-v1\n" + canonical(report))
        evidence.rename(output / "evidence.zip")
        qa.write_json(output / "manifest.json.partial", report)
        (output / "manifest.json.partial").rename(output / "manifest.json")
        return report
    except Exception as error:
        qa.write_json(output / "failure.json", {"status": "failed", "error": str(error), "inputs": receipts,
            "instruction": "No completed audit; preserve partial evidence and use a fresh output directory"})
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("source", "preparation-manifest", "gaps", "attempt-dir", "context-manifest",
                 "evaluation-manifest", "books-manifest", "ingestion-manifest", "source-fidelity-receipt", "output-dir"):
        parser.add_argument("--" + flag, required=True, type=Path)
    report = audit(parser.parse_args())
    print(json.dumps({"audit_id": report["audit_id"], "required_gap_count": report["required_gap_count"],
                      "classifications": report["classifications"], "economic_status": "unverified"}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
