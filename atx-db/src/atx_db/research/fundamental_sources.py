"""Pinned, bounded staged accounting inputs for F.1; never opens a warehouse or a label.

The source manifests are existing X.4/1.4 contracts. They are checked in full before their
explicit paths are handed to candidate workers; directory names do not establish identity.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
from typing import Any

SNAPSHOT_DATE = dt.date(2026, 9, 20)
FSDS_VERSION = "x4_fsds_stage_v2"
CF_VERSION = "cf-extract-v2"
CLOCK_BASIS = "fsds_acceptance_cf_fc1_v1"


class FundamentalSourceError(ValueError):
    """An input fails the explicit version, scope, path or content contract."""


def file_sha256(path: Path | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False, default=str).encode()).hexdigest()


def _manifest(path: Path, expected_sha256: str) -> dict[str, Any]:
    if file_sha256(path) != expected_sha256:
        raise FundamentalSourceError(f"manifest SHA256 differs from pin: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def _child(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise FundamentalSourceError(f"missing or escaped manifest file: {relative}")
    return path


def verify_sources(fsds_manifest: Path | str, cf_manifest: Path | str, *, fsds_sha256: str,
                   cf_sha256: str, snapshot_date: dt.date = SNAPSHOT_DATE) -> dict[str, Any]:
    """Stream-verify all 276 FSDS files, 85 CF batches and member/plan/receipt identities.

    Returns only small immutable pin metadata, not facts. Complete source scope is required;
    diagnostic subsets are selected later and cannot alter this source-acceptance claim.
    """
    if snapshot_date != SNAPSHOT_DATE:
        raise FundamentalSourceError("snapshot date must remain 2026-09-20")
    fsds_path, cf_path = Path(fsds_manifest).resolve(), Path(cf_manifest).resolve()
    fsds, cf = _manifest(fsds_path, fsds_sha256), _manifest(cf_path, cf_sha256)
    quarters = [f"{2009 + (q + 1) // 4}q{(q + 1) % 4 + 1}" for q in range(69)]
    if (fsds.get("schema") != "x4_fsds_staging_manifest_v2" or fsds.get("stager_version") != FSDS_VERSION
            or sorted(fsds.get("quarters", {})) != quarters or fsds.get("accepted_zone") != "America/New_York"):
        raise FundamentalSourceError("FSDS needs the complete69-quarter explicit v2 manifest")
    files = []
    rows_by_table: dict[str, int] = {}
    for quarter in quarters:
        record = fsds["quarters"][quarter]
        if record.get("stager_version") != FSDS_VERSION or set(record.get("tables", {})) != {"num", "sub", "tag", "pre"}:
            raise FundamentalSourceError(f"mixed/incomplete FSDS quarter: {quarter}")
        for table, entry in sorted(record["tables"].items()):
            path = _child(fsds_path.parent, entry["path"])
            if path.stat().st_size != entry["parquet_bytes"] or file_sha256(path) != entry["parquet_sha256"]:
                raise FundamentalSourceError(f"FSDS content mismatch: {quarter}/{table}")
            if entry.get("missing_columns") or entry.get("line_rows_delta"):
                raise FundamentalSourceError(f"FSDS lossy staging: {quarter}/{table}")
            if table == "sub" and any(entry.get("accepted_dst", {}).get(k, 0)
                                      for k in ("accepted_null_rows", "ambiguous", "nonexistent")):
                raise FundamentalSourceError(f"FSDS unresolved acceptance clocks: {quarter}")
            files.append({"source": "fsds", "slice": quarter, "table": table, "path": path.as_posix(),
                          "sha256": entry["parquet_sha256"], "rows": entry["rows"], "bytes": path.stat().st_size})
            rows_by_table[table] = rows_by_table.get(table, 0) + entry["rows"]
    if (cf.get("rule_version") != CF_VERSION or len(cf.get("batches", [])) != 85
            or sorted(x["batch_id"] for x in cf["batches"]) != list(range(85))
            or cf["totals"].get("members") != 20_390 or cf["totals"].get("error") != 0):
        raise FundamentalSourceError("CF-R requires85 completed v2 batches and20,390 dispositions without errors")
    plan_path = _child(cf_path.parent, "plan.json")
    if file_sha256(plan_path) != cf["plan_sha256"]:
        raise FundamentalSourceError("CF plan hash mismatch")
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    if plan.get("rule_version") != CF_VERSION or plan["allowlist"]["allowlist_sha256"] != cf["allowlist"]["allowlist_sha256"]:
        raise FundamentalSourceError("CF plan version or allowlist mismatch")
    for entry in cf["batches"]:
        path = _child(cf_path.parent, entry["file"])
        receipt = json.loads(_child(cf_path.parent, f"batch-{entry['batch_id']:04d}.json").read_text(encoding="utf-8"))
        if (path.stat().st_size != entry["parquet_bytes"] or file_sha256(path) != entry["parquet_sha256"]
                or receipt.get("plan_sha256") != cf["plan_sha256"]):
            raise FundamentalSourceError(f"CF batch content/plan mismatch: {entry['batch_id']}")
        files.append({"source": "cf", "slice": entry["batch_id"], "table": "facts", "path": path.as_posix(),
                      "sha256": entry["parquet_sha256"], "rows": entry["rows"], "bytes": entry["parquet_bytes"]})
    member = cf["members_file"]
    member_path = _child(cf_path.parent, member["file"])
    if member["rows"] != 20_390 or file_sha256(member_path) != member["sha256"]:
        raise FundamentalSourceError("CF member dispositions hash/count mismatch")
    if sum(x["rows"] for x in cf["batches"]) != cf["totals"]["rows"]:
        raise FundamentalSourceError("CF batch rows do not reconcile to manifest totals")
    return {"schema": "fundamental_source_pins_v1", "snapshot_date": str(snapshot_date), "scope_complete": True,
            "clock_basis": CLOCK_BASIS, "fsds_manifest": fsds_path.as_posix(), "fsds_sha256": fsds_sha256,
            "cf_manifest": cf_path.as_posix(), "cf_sha256": cf_sha256, "members_sha256": member["sha256"],
            "cf_plan_sha256": cf["plan_sha256"], "cf_archive_sha256": cf["archive"]["sha256"],
            "cf_allowlist_sha256": cf["allowlist"]["allowlist_sha256"], "fsds_quarters": len(quarters),
            "fsds_rows": rows_by_table, "cf_batches": 85, "cf_totals": cf["totals"],
            "fsds_orphan_facts": fsds.get("num_orphan_facts", 0), "files": files}


def atomic_json(path: Path, value: Any) -> None:
    """Generated run artifacts use atomic publication; authored sources use apply_patch."""
    import os
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".pending")
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2, default=str, allow_nan=False) + "\n",
                         encoding="utf-8")
    os.replace(temporary, path)


def parquet_relation(files: list[Path | str]) -> str:
    from .research_lake import sql_text
    if not files:
        raise FundamentalSourceError("empty pinned Parquet input")
    return "read_parquet([" + ",".join(sql_text(Path(p).as_posix()) for p in files) + "])"


def check_file(entry: dict[str, Any]) -> Path:
    path = Path(entry["path"])
    if path.stat().st_size != entry["bytes"] or file_sha256(path) != entry["sha256"]:
        raise FundamentalSourceError(f"consumed file differs from pin: {path}")
    return path


def prepare_filing_index(pins: dict[str, Any], target: Path, *, root: Path) -> dict[str, Any]:
    """Small global SUB index preserves cross-quarter accession joins and detects conflicts."""
    from .research_lake import connect_bounded, sql_text
    files = [check_file(f) for f in pins["files"] if f["source"] == "fsds" and f["table"] == "sub"]
    con = connect_bounded(None, root=root, memory_limit="256MB", threads=1)
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        con.execute(f"""CREATE TEMP TABLE sub_distinct AS SELECT DISTINCT adsh,cik,form,period,fy,fp,
          filed,accepted_utc::TIMESTAMP accepted_utc,fye FROM {parquet_relation(files)}""")
        con.execute("""CREATE TEMP TABLE sub_counts AS SELECT adsh,count(*) AS sub_contexts
                       FROM sub_distinct GROUP BY adsh""")
        con.execute(f"""COPY (SELECT d.*,c.sub_contexts FROM sub_distinct d JOIN sub_counts c USING(adsh)
                      ORDER BY adsh,cik,accepted_utc) TO {sql_text(target.as_posix())}
                      (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 16384)""")
        rows, conflicts = con.execute("SELECT count(*),count(*) FILTER(WHERE sub_contexts<>1) FROM sub_distinct JOIN sub_counts USING(adsh)").fetchone()
    finally:
        con.close()
    return {"path": target.as_posix(), "sha256": file_sha256(target), "bytes": target.stat().st_size,
            "rows": rows, "conflicting_sub_rows": conflicts}


def _candidate_query(pins: dict[str, Any], source: str, slice_id: str | int, *,
                     filing_index: dict[str, Any], buckets: int, part: int = 0,
                     parts: int = 1) -> tuple[str, list[dict[str, Any]]]:
    from .research_lake import sql_text
    entries = [f for f in pins["files"] if f["source"] == source and str(f["slice"]) == str(slice_id)]
    by_table = {f["table"]: f for f in entries}
    if not entries or source not in {"cf", "fsds"}:
        raise FundamentalSourceError("source slice not present in pinned scope")
    for entry in entries:
        check_file(entry)
    if parts < 1 or not 0 <= part < parts:
        raise FundamentalSourceError("invalid source partition")
    limit = sql_text(str(SNAPSHOT_DATE)) + "::DATE"
    if source == "cf":
        entry = by_table["facts"]
        raw = f"""SELECT cik,replace(accession_number,'-','') AS accession,taxonomy,concept,
          NULL::VARCHAR taxonomy_version,unit,period_start,period_end,
          CASE WHEN period_start IS NULL THEN 0
            WHEN date_diff('day',period_start,period_end)+1 BETWEEN 70 AND 112 THEN 1
            WHEN date_diff('day',period_start,period_end)+1 BETWEEN 150 AND 215 THEN 2
            WHEN date_diff('day',period_start,period_end)+1 BETWEEN 240 AND 310 THEN 3
            WHEN date_diff('day',period_start,period_end)+1 BETWEEN 350 AND 380 THEN 4 END qtrs,
          fiscal_year AS reported_fy,fiscal_period AS reported_fp,form,filed_date,
          NULL::DATE filing_period,NULL::VARCHAR fye,NULL::TIMESTAMP accepted_utc,
          available_at AS raw_available_at,
          greatest(available_at,filed_date::TIMESTAMP+INTERVAL 46 HOUR) available_at,
          'cf_fc1_reconstructed' AS clock_basis,value,
          coalesce(value_exact,CAST(value AS VARCHAR)) value_exact,
          NULL::VARCHAR segments,NULL::VARCHAR coreg,false AS abstract,false AS custom,
          1::BIGINT sub_contexts,frame,{sql_text(entry['sha256'])} source_sha256,
          {sql_text(pins['cf_archive_sha256'])} archive_sha256,
          CASE WHEN filed_date IS NULL OR available_at IS NULL THEN 'missing_clock'
               WHEN filed_date>{limit} OR period_end>{limit}
                 OR greatest(available_at,filed_date::TIMESTAMP+INTERVAL 46 HOUR)>=({limit}+INTERVAL 1 DAY) THEN 'post_snapshot'
               WHEN form NOT IN ('10-K','10-K/A','10-Q','10-Q/A','20-F','20-F/A','40-F','40-F/A') THEN 'disallowed_form'
               WHEN taxonomy NOT IN ('us-gaap','dei') THEN 'unsupported_taxonomy'
               WHEN period_end IS NULL THEN 'missing_period' ELSE 'candidate' END source_status
          FROM {parquet_relation([entry['path']])}
          WHERE coalesce(try_cast(cik AS BIGINT),0)%{parts}={part}"""
    else:
        entry, tag = by_table["num"], by_table["tag"]
        check_file(filing_index)
        raw = f"""SELECT s.cik,replace(n.adsh,'-','') accession,
          CASE WHEN n.version LIKE 'us-gaap/%' THEN 'us-gaap'
               WHEN n.version LIKE 'dei/%' THEN 'dei' ELSE 'custom' END taxonomy,
          n.tag concept,n.version taxonomy_version,n.uom unit,NULL::DATE period_start,n.ddate period_end,
          n.qtrs,s.fy reported_fy,s.fp reported_fp,s.form,s.filed filed_date,
          s.period filing_period,s.fye,s.accepted_utc,s.accepted_utc raw_available_at,
          s.accepted_utc available_at,'fsds_accepted_utc' clock_basis,n.value::DOUBLE AS value,
          n.value::VARCHAR value_exact,n.segments,n.coreg,t.abstract,t.custom,s.sub_contexts,
          NULL::VARCHAR frame,{sql_text(entry['sha256'])} source_sha256,
          {sql_text(pins['fsds_sha256'])} archive_sha256,
          CASE WHEN s.adsh IS NULL THEN 'missing_sub'
               WHEN s.sub_contexts<>1 THEN 'conflicting_sub'
               WHEN s.accepted_utc IS NULL THEN 'missing_clock'
               WHEN s.filed>{limit} OR n.ddate>{limit}
                 OR s.accepted_utc>=({limit}+INTERVAL 1 DAY) THEN 'post_snapshot'
               WHEN s.form NOT IN ('10-K','10-K/A','10-Q','10-Q/A','20-F','20-F/A','40-F','40-F/A') THEN 'disallowed_form'
               WHEN n.segments IS NOT NULL OR n.coreg IS NOT NULL THEN 'dimensional_or_coreg'
               WHEN t.tag IS NULL THEN 'missing_tag_context'
               WHEN t.abstract THEN 'abstract_tag'
               WHEN t.custom OR n.version NOT LIKE 'us-gaap/%' AND n.version NOT LIKE 'dei/%' THEN 'custom_tag'
               WHEN n.ddate IS NULL THEN 'missing_period'
               WHEN n.qtrs NOT BETWEEN 0 AND 4 THEN 'unsupported_duration'
               ELSE 'candidate' END source_status
          FROM (SELECT * FROM {parquet_relation([entry['path']])}
                WHERE cast('0x'||substr(sha256(replace(coalesce(adsh,''),'-','')),1,8) AS UBIGINT)%{parts}={part}) n
          LEFT JOIN {parquet_relation([filing_index['path']])} s ON n.adsh=s.adsh
          LEFT JOIN (SELECT tag,version,bool_or(abstract) abstract,bool_or(custom) custom
                     FROM {parquet_relation([tag['path']])} GROUP BY tag,version) t
            ON n.tag=t.tag AND n.version=t.version"""
    # SUB conflicts have multiple provenance contexts, each quarantined; no winner is selected.
    source_partition = (f"coalesce(try_cast(cik AS BIGINT),0)%{parts}" if source == "cf" else
                        f"cast('0x'||substr(sha256(coalesce(accession,'')),1,8) AS UBIGINT)%{parts}")
    query = f"""WITH raw AS ({raw}), identified AS (SELECT *,
      sha256(to_json(struct_pack(cik:=cik,accession:=accession,taxonomy:=taxonomy,concept:=concept,
        taxonomy_version:=taxonomy_version,unit:=unit,period_start:=period_start,period_end:=period_end,
        qtrs:=qtrs,reported_fy:=reported_fy,reported_fp:=reported_fp,form:=form,filed_date:=filed_date,
        value_exact:=value_exact,segments:=segments,coreg:=coreg,frame:=frame,
        source_sha256:=source_sha256,available_at:=available_at))) candidate_id
      FROM raw)
      SELECT *,{sql_text(source)} source_kind,{sql_text(str(slice_id))} source_slice,
        ({source_partition})::INTEGER source_partition,
        coalesce(try_cast(cik AS BIGINT)%{int(buckets)},0)::INTEGER bucket
      FROM identified ORDER BY bucket,cik,available_at,candidate_id"""
    return query, entries


def iter_candidates(pins: dict[str, Any], source: str, slice_id: str | int, *,
                    filing_index: dict[str, Any], buckets: int = 128, root: Path,
                    part: int = 0, parts: int = 1):
    from .research_lake import connect_bounded
    query, _ = _candidate_query(pins, source, slice_id, filing_index=filing_index,
                                buckets=buckets, part=part, parts=parts)
    con = connect_bounded(None, root=root, memory_limit="256MB", threads=1)
    try:
        yield from con.execute(query).to_arrow_reader(batch_size=16384)
    finally:
        con.close()


def iter_fsds_candidates(manifest_path: Path | str, *, pins: dict[str, Any], filing_index: dict[str, Any],
                         root: Path, quarter: str, ciks=None, buckets: int = 128):
    if Path(manifest_path).resolve() != Path(pins["fsds_manifest"]).resolve():
        raise FundamentalSourceError("manifest does not match source pins")
    for batch in iter_candidates(pins, "fsds", quarter, filing_index=filing_index, buckets=buckets, root=root):
        if ciks is not None:
            import pyarrow as pa
            import pyarrow.compute as pc
            batch = batch.filter(pc.is_in(batch.column("cik"), value_set=pa.array(list(ciks))))
        yield batch


def iter_cf_candidates(manifest_path: Path | str, *, pins: dict[str, Any], filing_index: dict[str, Any],
                       root: Path, batch_id: int, buckets: int = 128):
    if Path(manifest_path).resolve() != Path(pins["cf_manifest"]).resolve():
        raise FundamentalSourceError("manifest does not match source pins")
    yield from iter_candidates(pins, "cf", batch_id, filing_index=filing_index, buckets=buckets, root=root)


def normalize_source(pins: dict[str, Any], source: str, slice_id: str | int, *,
                     filing_index: dict[str, Any], output: Path, root: Path, buckets: int = 128,
                     part: int = 0, parts: int = 1) -> dict[str, Any]:
    """Stream a sorted source into bounded owner-bucket files; receipt is the commit point.

    A missing receipt makes every partial file unreadable. A retry overwrites only its
    own incomplete files. Completed output hashes are checked before skipping a slice.
    """
    import collections
    import os
    import time
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq

    output.mkdir(parents=True, exist_ok=True)
    receipt = output / "complete.json"
    source_part = part
    identity = source_identity(pins, source, slice_id, filing_index=filing_index,
                               buckets=buckets, part=source_part, parts=parts)
    if receipt.exists():
        prior = validate_source_receipt(receipt, pins, source, slice_id, filing_index=filing_index,
                                        buckets=buckets, part=source_part, parts=parts)
        return {**prior, "resumed": True}
    start = time.perf_counter()
    counts, outputs = collections.Counter(), []
    writer, current_bucket, rows, file_part = None, None, 0, 0
    target = pending = None

    def close_writer():
        nonlocal writer
        if writer is None:
            return
        writer.close()
        writer = None
        os.replace(pending, target)
        outputs.append({"path": target.as_posix(), "bucket": current_bucket,
                        "rows": rows, "sha256": file_sha256(target), "bytes": target.stat().st_size})

    try:
        for batch in iter_candidates(pins, source, slice_id, filing_index=filing_index,
                                     buckets=buckets, root=root, part=source_part, parts=parts):
            observed = pc.min_max(batch.column("source_partition")).as_py()
            if observed["min"] != source_part or observed["max"] != source_part:
                raise FundamentalSourceError("worker emitted a different requested source partition")
            counts.update(batch.column("source_status").to_pylist())
            bucket_values = batch.column("bucket").to_pylist()
            offset = 0
            while offset < len(bucket_values):
                bucket = bucket_values[offset]
                end = offset + 1
                while end < len(bucket_values) and bucket_values[end] == bucket:
                    end += 1
                if writer is None or current_bucket != bucket or rows >= 500_000:
                    close_writer()
                    file_part = file_part + 1 if current_bucket == bucket else 0
                    current_bucket, rows = bucket, 0
                    target = output / f"b{bucket:03d}-{file_part:03d}.parquet"
                    pending = target.with_suffix(".pending")
                    writer = pq.ParquetWriter(pending, batch.schema, compression="zstd",
                                             use_dictionary=True, write_statistics=True)
                block = batch.slice(offset, end-offset)
                writer.write_table(pa.Table.from_batches([block]), row_group_size=16384)
                rows += block.num_rows
                offset = end
        close_writer()
    finally:
        if writer is not None:
            writer.close()
    source_entry = next(f for f in pins["files"] if f["source"] == source and str(f["slice"]) == str(slice_id)
                        and f["table"] == ("num" if source == "fsds" else "facts"))
    result = {"schema": "fundamental_source_slice_v2", "identity": identity,
              "source": source, "slice": slice_id, "buckets": buckets, "files": outputs,
              "part": source_part, "parts": parts,
              "source_code_sha256": file_sha256(Path(__file__)),
              "source_file_sha256": source_entry["sha256"], "source_rows": source_entry["rows"],
              "rows": sum(f["rows"] for f in outputs), "source_status": dict(counts),
              "seconds": round(time.perf_counter()-start, 3)}
    atomic_json(receipt, result)
    return result


def source_identity(pins, source, slice_id, *, filing_index, buckets, part, parts):
    """Exact immutable source-slice contract, shared by runner and plan validation."""
    return canonical_sha256({"source_code": file_sha256(Path(__file__)), "pins": pins,
                             "source": source, "slice": int(slice_id) if source == "cf" else str(slice_id),
                             "buckets": buckets, "part": part, "parts": parts,
                             "filing_index": filing_index})


def validate_source_receipt(path, pins, source, slice_id, *, filing_index, buckets, part, parts):
    """Reject stale, mixed, swapped, duplicate or incomplete normalization receipts."""
    import pyarrow.parquet as pq
    record = json.loads(Path(path).read_text())
    expected = source_identity(pins, source, slice_id, filing_index=filing_index,
                               buckets=buckets, part=part, parts=parts)
    contract = (record.get("schema"), record.get("identity"), record.get("source"), str(record.get("slice")),
                record.get("buckets"), record.get("part"), record.get("parts"))
    if contract != ("fundamental_source_slice_v2", expected, source, str(slice_id), buckets, part, parts):
        raise FundamentalSourceError(f"source receipt identity/partition mismatch: {path}")
    source_entry = next(f for f in pins["files"] if f["source"] == source and str(f["slice"]) == str(slice_id)
                        and f["table"] == ("num" if source == "fsds" else "facts"))
    if record.get("source_file_sha256") != source_entry["sha256"] or record.get("source_rows") != source_entry["rows"]:
        raise FundamentalSourceError("source receipt denominator/input mismatch")
    total, seen = 0, set()
    for entry in record["files"]:
        consumed = check_file(entry)
        if consumed.resolve() in seen or not 0 <= entry["bucket"] < buckets:
            raise FundamentalSourceError("duplicate normalized file or invalid owner bucket")
        seen.add(consumed.resolve())
        parquet = pq.ParquetFile(consumed)
        rows = parquet.metadata.num_rows
        if rows != entry["rows"]:
            raise FundamentalSourceError("normalized file row count differs from receipt")
        for group_number in range(parquet.metadata.num_row_groups):
            group = parquet.metadata.row_group(group_number)
            for column, expected_value in (("source_partition", part), ("source_kind", source),
                                            ("source_slice", str(slice_id)), ("bucket", entry["bucket"])):
                if column not in parquet.schema_arrow.names:
                    raise FundamentalSourceError("normalized Parquet lacks source partition evidence")
                statistics = group.column(parquet.schema_arrow.get_field_index(column)).statistics
                if (statistics is None or statistics.null_count or statistics.min != expected_value
                        or statistics.max != expected_value):
                    raise FundamentalSourceError(f"normalized Parquet {column} differs from receipt contract")
        total += rows
    if total != record["rows"] or sum(record["source_status"].values()) != total:
        raise FundamentalSourceError("source disposition/file row denominator mismatch")
    if parts == 1 and total != source_entry["rows"]:
        raise FundamentalSourceError("unsplit normalized rows do not equal pinned raw source")
    return record
