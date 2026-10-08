"""Bounded immutable native original-to-position diagnostics.

The reader contract pins every consumed inventory, receipt, rule document and
original. No supported documentary admission rules have yet been independently
established, so this first producer keeps public/economic qualification missing.
"""
from __future__ import annotations

import argparse
import datetime as dt
import gzip
import hashlib
import json
import os
import re
from pathlib import Path
from typing import Any

import duckdb

from . import common as C
from .thirteenf_quality import accession_key, original_submission_evidence

SCHEMA = "atx.thirteenf-native-originals/v2"
MAX_OBJECT = 8 * 1024 * 1024
MAX_METADATA = 64 * 1024 * 1024
MAX_RECORD = 256 * 1024


def _rooted(path: Path, roots: list[Path]) -> Path:
    if not path.is_absolute():
        raise ValueError("consumed_path_not_absolute")
    resolved = path.resolve(strict=True)
    if not any(resolved.is_relative_to(root) for root in roots):
        raise ValueError("consumed_path_outside_roots")
    for ancestor in (path, *path.parents):
        info = ancestor.lstat()
        if ancestor.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("consumed_reparse_path_unsupported")
    return resolved


def _pin(item: dict[str, Any], roots: list[Path], maximum: int) -> dict[str, Any]:
    path = _rooted(Path(item["path"]), roots)
    identity = C.file_identity(path)
    if identity["bytes"] > maximum or identity["bytes"] != item["bytes"]:
        raise ValueError("consumed_file_byte_identity_or_bound")
    if "mtime_ns" in item and identity["mtime_ns"] != item["mtime_ns"]:
        raise ValueError("consumed_file_current_identity_mismatch")
    digest = C.sha256_file(path)
    if digest != item["sha256"]:
        raise ValueError("consumed_file_digest_mismatch")
    return dict(path=str(path), bytes=identity["bytes"], sha256=digest,
                mtime_ns=identity["mtime_ns"])


def _rows(path: Path):
    with path.open("rb") as stream:
        for number in range(1, 100001):
            line = stream.readline(MAX_RECORD + 1)
            if not line:
                return
            if len(line) > MAX_RECORD:
                raise ValueError("metadata_record_byte_bound")
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError("metadata_record_object_required")
            yield number, line, row
        if stream.read(1):
            raise ValueError("metadata_record_count_bound")


def _publish_file(con, query: str, path: Path) -> dict[str, Any]:
    partial = path.with_suffix(path.suffix + ".partial")
    if partial.exists():
        raise FileExistsError(partial)
    escaped = partial.as_posix().replace("'", "''")
    con.execute(f"COPY ({query}) TO '{escaped}' (FORMAT PARQUET, COMPRESSION ZSTD)")
    identity = dict(bytes=partial.stat().st_size, sha256=C.sha256_file(partial))
    os.link(partial, path)  # Atomic refuse-existing installation.
    partial.unlink()
    return identity


def _reader_code() -> dict[str, Any]:
    identity = C.code_identity("thirteenf_native", "thirteenf_quality", "common")
    identity.pop("git_head", None)  # Unrelated shared-tree commits are not reader changes.
    return identity


def build(plan_path: Path, *, _replay_directory: Path | None = None) -> dict[str, Any]:
    """Create one never-overwritten generation; manifest is its commit marker.

    A selection bound never changes the expected accession denominator. Missing
    retained records/objects and unsupported reader types are keyed diagnostics.
    A failed input identity leaves an uncommitted generation for inspection.
    """
    plan_path = _rooted(plan_path.absolute(), [plan_path.absolute().parent.resolve(strict=True)])
    if plan_path.stat().st_size > MAX_OBJECT:
        raise ValueError("native_plan_byte_bound")
    code_before = _reader_code()
    plan_identity = C.file_identity(plan_path)
    plan_hash = C.sha256_file(plan_path)
    plan = C.read_json(plan_path)
    if plan.get("schema") != "atx.thirteenf-native-plan/v1":
        raise ValueError("unsupported_native_plan")
    limit = plan["max_originals"]
    if not isinstance(limit, int) or not 1 <= limit <= 10000:
        raise ValueError("native_original_selection_bound")
    declared_roots = [Path(path) for path in plan["roots"]]
    if any(not path.is_absolute() for path in declared_roots):
        raise ValueError("declared_source_root_must_be_absolute")
    roots = [path.resolve(strict=True) for path in declared_roots]
    if not roots:
        raise ValueError("explicit_consumed_roots_required")
    products = plan["products"]
    required = {"expected_inventory", "retained_receipts", "rule_bundle"}
    if len(products) != len(required) or {row["role"] for row in products} != required:
        raise ValueError("complete_exact_reader_product_inventory_required")
    pins = {row["role"]: _pin(row, roots, MAX_METADATA) for row in products}
    bundle = C.read_json(Path(pins["rule_bundle"]["path"]))
    if bundle.get("schema") != "atx.thirteenf-documentary-proof/v1":
        raise ValueError("unsupported_documentary_bundle_schema")
    documents = bundle.get("documents", [])
    if not isinstance(documents, list) or len(documents) > 32:
        raise ValueError("documentary_source_inventory_bound")
    document_pins = []
    for document in documents:
        pin = _pin(document, roots, MAX_OBJECT)
        # Retention proves document identity only, not the claimed rule semantics.
        pin.update(source_url=document.get("source_url"),
                   publication_clock=document.get("publication_clock"),
                   proposed_rule_id=document.get("rule_id"))
        document_pins.append(pin)
    objects = plan["originals"]
    if not isinstance(objects, list) or len(objects) > 10000:
        raise ValueError("native_original_reader_inventory_bound")
    objects_by_accession = {}
    for item in objects:
        key = accession_key(item["accession"])
        if key in objects_by_accession:
            raise ValueError("duplicate_original_reader_key")
        objects_by_accession[key] = item
    generation = _replay_directory or Path(plan["generation"])
    if not generation.is_absolute():
        raise ValueError("generation_path_must_be_absolute")
    generation.mkdir(parents=True, exist_ok=False)
    con = duckdb.connect(str(generation / "attempt.duckdb"), config={
        "memory_limit": "128MB", "threads": "1", "preserve_insertion_order": "false",
        "temp_directory": str(generation / "spill")})
    consumed_objects = []
    try:
        con.execute("SET TimeZone='UTC'")
        con.execute("CREATE TABLE expected(accession VARCHAR PRIMARY KEY,payload VARCHAR)")
        con.execute("CREATE TABLE receipts(accession VARCHAR PRIMARY KEY,line_sha256 VARCHAR,payload VARCHAR)")
        for _, _, row in _rows(Path(pins["expected_inventory"]["path"])):
            con.execute("INSERT INTO expected VALUES (?,?)", [accession_key(row["accession"]), json.dumps(row)])
        for _, line, row in _rows(Path(pins["retained_receipts"]["path"])):
            con.execute("INSERT INTO receipts VALUES (?,?,?)", [accession_key(row["accession"]),
                        hashlib.sha256(line).hexdigest(), json.dumps(row)])
        con.execute("""CREATE TABLE admissions(accession VARCHAR PRIMARY KEY,form VARCHAR,filer_cik BIGINT,
            report_date DATE,available_at TIMESTAMPTZ,source_qualified BOOLEAN,qualified_original BOOLEAN,
            source_retained BOOLEAN,complete_container_observed BOOLEAN,cover_reconciled BOOLEAN,
            raw_sha256 VARCHAR,object_sha256 VARCHAR,source_url VARCHAR,retrieval_clock VARCHAR,
            header_acceptance_raw VARCHAR,value_unit VARCHAR,table_absence_qualified BOOLEAN,
            exceptions VARCHAR[],evidence VARCHAR)""")
        con.execute("""CREATE TABLE positions(accession VARCHAR,position_index BIGINT,cusip VARCHAR,
            shares_reported DOUBLE,principal_reported DOUBLE,amount_type VARCHAR,value_raw DOUBLE,
            value_usd DOUBLE,put_call VARCHAR,discretion VARCHAR,other_manager VARCHAR,
            available_at TIMESTAMPTZ,source_qualified BOOLEAN,PRIMARY KEY(accession,position_index))""")
        con.execute("CREATE TABLE rejects(accession VARCHAR,reason VARCHAR,source_kind VARCHAR)")
        unexpected = set(objects_by_accession) - {row[0] for row in con.execute("SELECT accession FROM expected").fetchall()}
        for key in sorted(unexpected):
            con.execute("INSERT INTO rejects VALUES (?,?,'original_inventory')", [key, "unexpected_original_accession"])
        con.execute("INSERT INTO rejects SELECT r.accession,'unexpected_receipt_accession','receipt' FROM receipts r ANTI JOIN expected e USING(accession)")
        cursor = con.cursor()
        cursor.execute("SELECT e.accession,e.payload,r.line_sha256,r.payload FROM expected e LEFT JOIN receipts r USING(accession) ORDER BY e.accession")
        processed = expected_count = 0
        while batch := cursor.fetchmany(32):
            for accession, expected_json, receipt_line_hash, receipt_json in batch:
                expected_count += 1
                expected = json.loads(expected_json)
                item = objects_by_accession.get(accession)
                receipt = json.loads(receipt_json) if receipt_json else {}
                reasons = []
                if expected.get("form") not in {"13F-HR","13F-HR/A","13F-NT","13F-NT/A"}:
                    reasons.append("expected_form_not_supported_13f")
                if not re.fullmatch(r"[0-9a-f]{64}", str(expected.get("inventory_sha256", ""))) or not str(expected.get("inventory_url", "")).startswith("https://www.sec.gov/Archives/"):
                    reasons.append("authoritative_inventory_record_provenance_unverified")
                result = {}
                source_retained = False
                object_pin = None
                if item is None:
                    reasons.append("retained_original_reader_missing")
                elif processed >= limit:
                    reasons.append("selected_original_bound_not_processed")
                else:
                    processed += 1
                    try:
                        supported_fields = {"accession", "path", "bytes", "sha256", "receipt_line_sha256", "reader_type"}
                        if set(item) - supported_fields:
                            raise ValueError("unsupported_original_reader_fields")
                        if not receipt_json or receipt_line_hash != item.get("receipt_line_sha256"):
                            raise ValueError("exact_retained_receipt_record_missing_or_mismatch")
                        object_pin = _pin(item, roots, MAX_OBJECT)
                        consumed_objects.append(object_pin)
                        receipt_path = Path(receipt.get("path", ""))
                        if not receipt_path.is_absolute() or receipt.get("sha256") != object_pin["sha256"] or receipt_path.resolve() != Path(object_pin["path"]):
                            raise ValueError("receipt_original_object_identity_mismatch")
                        kind = item.get("reader_type")
                        if kind not in {"sec_submission_text", "gzip_sec_submission_text"}:
                            raise ValueError("original_reader_type_unsupported")
                        path = Path(object_pin["path"])
                        if kind == "gzip_sec_submission_text":
                            with gzip.open(path, "rb") as stream:
                                raw = stream.read(MAX_OBJECT + 1)
                        else:
                            raw = path.read_bytes()
                        if len(raw) > MAX_OBJECT:
                            raise ValueError("decompressed_original_byte_bound")
                        decoded_receipt = dict(receipt, sha256=hashlib.sha256(raw).hexdigest())
                        result = original_submission_evidence(raw, expected, decoded_receipt)
                        source_retained = True
                        reasons.extend(result["exceptions"])
                        header = re.search(rb"CENTRAL INDEX KEY:\s*(\d+)", raw)
                        actual_cik = int(header.group(1)) if header else None
                        if actual_cik is None or actual_cik != int(expected.get("cik", expected.get("filer_cik", 0))):
                            reasons.append("original_filer_header_inventory_mismatch")
                        result["actual_filer_cik"] = actual_cik
                    except (ValueError, KeyError, OSError, EOFError) as exc:
                        reasons.append(str(exc))
                reasons.append("documentary_admission_rules_not_independently_established")
                evidence = dict(original=result,reader=item,receipt_line_sha256=receipt_line_hash,
                                inventory_record=expected,documentary_sources=document_pins)
                con.execute("INSERT INTO admissions VALUES(" + ",".join("?" for _ in range(19)) + ")", [
                    accession,result.get("form",expected.get("form")),result.get("actual_filer_cik"),result.get("report_date"),
                    None,False,False,source_retained,
                    source_retained and "complete_submission_container_missing" not in reasons,
                    result.get("cover_reconciled",False),result.get("original_sha256"),
                    object_pin["sha256"] if object_pin else None,receipt.get("url"),receipt.get("fetched_at"),
                    result.get("acceptance_header_raw"),None,False,sorted(set(reasons)),json.dumps(evidence)])
                for position in result.get("positions", []):
                    con.execute("INSERT INTO positions VALUES(" + ",".join("?" for _ in range(13)) + ")", [
                        accession,position["position_index"],position["cusip"],position["shares"],position["principal"],
                        position["amount_type"],position["value_raw"],None,position["put_call"],
                        position["discretion"],position["other_manager"],None,False])
                for reason in sorted(set(reasons)):
                    con.execute("INSERT INTO rejects VALUES (?,?,'original_admission')", [accession,reason])
        cursor.close()
        for pin in [*pins.values(), *document_pins, *consumed_objects]:
            _pin(pin, roots, MAX_METADATA)
        if _reader_code() != code_before:
            raise RuntimeError("native_reader_code_changed_during_computation")
        if C.file_identity(plan_path) != plan_identity or C.sha256_file(plan_path) != plan_hash:
            raise RuntimeError("native_plan_changed_during_computation")
        files = {}
        for table, order in (("admissions","accession"),("positions","accession,position_index"),("rejects","accession,reason,source_kind")):
            if _replay_directory is None:
                files[table+".parquet"] = _publish_file(con, f"SELECT * FROM {table} ORDER BY {order}", generation / (table+".parquet"))
        counts = {table: con.execute(f"SELECT count(*) FROM {table}").fetchone()[0] for table in ("admissions","positions","rejects")}
        bounded=con.execute("SELECT count(*) FROM admissions WHERE list_contains(exceptions,'selected_original_bound_not_processed')").fetchone()[0]>0
        manifest = dict(schema=SCHEMA,status="partial" if bounded or not expected_count else "complete",gate_status="UNQUALIFIED",plan_sha256=plan_hash,
            plan_identity=dict(plan_identity, sha256=plan_hash),reader_contract=plan,
            expected_accessions=expected_count,processed_originals=processed,counts=counts,files=files,
            bound_reached=bounded,
            source_completeness="UNMEASURED",contemporaneous_population_complete=False,
            source_rules_supported=False,reader_products=pins,documentary_sources=document_pins,
            consumed_originals=consumed_objects,code=code_before,
            required_future_release_bindings=["universe_release_v1", "fundamentals_release_v1/fundamentals_v11"],
            limits="Observed raw positions are not admitted economics/PIT; full source/rule/cohort/release gates remain open")
        if _replay_directory is not None:
            return manifest
        partial = generation / "manifest.json.partial"
        with partial.open("x", encoding="utf-8") as sink:
            json.dump(manifest,sink,sort_keys=True)
            sink.write("\n")
            sink.flush()
            os.fsync(sink.fileno())
        os.link(partial,generation / "manifest.json")
        partial.unlink()
        return manifest
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_mutually_exclusive_group(required=True)
    commands.add_argument("--plan",type=Path)
    commands.add_argument("--validate",type=Path)
    args=parser.parse_args(argv)
    if args.validate:
        result=validate(args.validate)
        print(json.dumps(result,sort_keys=True))
        return 0
    result=build(args.plan)
    print(json.dumps({key:result[key] for key in ("status","gate_status","counts","bound_reached")},sort_keys=True))
    return 2 if result["bound_reached"] else 0


def validate(generation: Path) -> dict[str, Any]:
    """Read-only structural/output validator; never an economic admission.

    A committed reproducible manifest and current exact source/code identities
    are required. Replay uses only private temporary scratch and compares every
    schema and row to native observations. No source rule is supported yet.
    """
    manifest_path=generation / "manifest.json"
    if manifest_path.stat().st_size > MAX_METADATA:
        raise ValueError("native_manifest_byte_bound")
    before=C.file_identity(manifest_path)
    manifest_hash=C.sha256_file(manifest_path)
    manifest=C.read_json(manifest_path)
    if manifest.get("schema")!=SCHEMA or manifest.get("status") not in {"complete","partial"}:
        raise ValueError("native_committed_manifest_required")
    if manifest.get("gate_status")!="UNQUALIFIED" or manifest.get("source_rules_supported") is not False or manifest.get("contemporaneous_population_complete") is not False:
        raise ValueError("unsupported_native_admission_claim")
    if set(manifest["reader_products"])!={"expected_inventory","retained_receipts","rule_bundle"}:
        raise ValueError("native_reader_inventory_incomplete")
    required={"admissions.parquet","positions.parquet","rejects.parquet"}
    if set(manifest["files"])!=required:
        raise ValueError("native_output_inventory_incomplete")
    contract = manifest.get("reader_contract")
    plan_pin = manifest.get("plan_identity")
    if not isinstance(contract, dict) or not isinstance(plan_pin, dict):
        raise ValueError("native_reproducible_plan_required")
    if manifest.get("code") != _reader_code():
        raise ValueError("native_reader_code_inventory_mismatch")
    plan_path = Path(plan_pin["path"])
    roots = [Path(path).resolve(strict=True) for path in contract["roots"]]
    # The plan may live outside source roots, but must retain its exact absolute identity.
    _pin(plan_pin, [plan_path.parent.resolve(strict=True)], MAX_OBJECT)
    if C.read_json(plan_path) != contract or manifest.get("plan_sha256") != plan_pin["sha256"]:
        raise ValueError("native_reader_contract_mismatch")
    if Path(contract["generation"]).resolve() != generation.resolve():
        raise ValueError("native_plan_generation_mismatch")
    pinned = [*manifest["reader_products"].values(), *manifest["documentary_sources"],
              *manifest["consumed_originals"]]
    for pin in pinned:
        _pin(pin, roots, MAX_METADATA)
    import tempfile
    with tempfile.TemporaryDirectory(prefix="t11-native-validate-") as temp:
        replay_directory = Path(temp) / "replay"
        replay = build(plan_path, _replay_directory=replay_directory)
        # Derive inventory, selection, observations, diagnostics and status from authenticated
        # native inputs, without reading any published rows or publishing replay products.
        for key in replay.keys() - {"files"}:
            if manifest.get(key) != replay[key]:
                raise ValueError("native_replay_manifest_mismatch:" + key)
        con=duckdb.connect(config={"memory_limit":"128MB","threads":"1","temp_directory":temp})
        try:
            con.execute("SET TimeZone='UTC'")
            replay_db = (replay_directory / "attempt.duckdb").as_posix().replace("'", "''")
            con.execute(f"ATTACH '{replay_db}' AS replay (READ_ONLY)")
            for name in sorted(required):
                path=generation / name
                identity=manifest["files"][name]
                if path.stat().st_size!=identity["bytes"] or C.sha256_file(path)!=identity["sha256"]:
                    raise ValueError("native_output_identity_mismatch")
                table=name.removesuffix(".parquet")
                escaped=path.as_posix().replace("'","''")
                con.execute(f"CREATE VIEW {table} AS SELECT * FROM read_parquet('{escaped}',hive_partitioning=false)")
                axes={row[0]:row[1] for row in con.execute(f"DESCRIBE {table}").fetchall()}
                expected_axes = {row[0]: row[1] for row in con.execute(f"DESCRIBE replay.{table}").fetchall()}
                if axes != expected_axes or list(axes) != list(expected_axes):
                    raise ValueError("native_exact_schema_mismatch")
                difference = con.execute(f"SELECT count(*) FROM ((SELECT * FROM {table} EXCEPT ALL SELECT * FROM replay.{table}) UNION ALL (SELECT * FROM replay.{table} EXCEPT ALL SELECT * FROM {table}))").fetchone()[0]
                if difference:
                    raise ValueError("native_replay_rows_mismatch:" + table)
                required_axes={"accession":"VARCHAR"}
                if table=="admissions":
                    required_axes.update(report_date="DATE",available_at="TIMESTAMP WITH TIME ZONE",
                                         filer_cik="BIGINT",source_qualified="BOOLEAN",exceptions="VARCHAR[]")
                elif table=="positions":
                    required_axes.update(position_index="BIGINT",shares_reported="DOUBLE",value_raw="DOUBLE",
                                         available_at="TIMESTAMP WITH TIME ZONE",source_qualified="BOOLEAN")
                if any(axes.get(axis)!=kind for axis,kind in required_axes.items()):
                    raise ValueError("native_typed_axes_invalid")
                if con.execute(f"SELECT count(*) FROM {table}").fetchone()[0]!=manifest["counts"][table]:
                    raise ValueError("native_output_count_mismatch")
            if con.execute("SELECT count(*) FROM admissions").fetchone()[0]!=manifest["expected_accessions"]:
                raise ValueError("native_expected_denominator_mismatch")
            if con.execute("SELECT count(*) FROM (SELECT accession FROM admissions GROUP BY 1 HAVING accession IS NULL OR count(*)!=1)").fetchone()[0]:
                raise ValueError("native_admission_key_invalid")
            if con.execute("SELECT count(*) FROM (SELECT accession,position_index FROM positions GROUP BY 1,2 HAVING accession IS NULL OR position_index IS NULL OR count(*)!=1)").fetchone()[0]:
                raise ValueError("native_position_key_invalid")
            if con.execute("SELECT count(*) FROM positions p ANTI JOIN admissions a USING(accession)").fetchone()[0]:
                raise ValueError("native_position_parent_missing")
            if con.execute("""SELECT count(*) FROM admissions WHERE available_at IS NOT NULL OR value_unit IS NOT NULL
                OR source_qualified IS DISTINCT FROM false OR qualified_original IS DISTINCT FROM false
                OR table_absence_qualified IS DISTINCT FROM false""").fetchone()[0]:
                raise ValueError("unsupported_native_admission_economics")
            if con.execute("SELECT count(*) FROM positions WHERE available_at IS NOT NULL OR value_usd IS NOT NULL OR source_qualified IS DISTINCT FROM false").fetchone()[0]:
                raise ValueError("unsupported_native_position_economics")
            for name,identity in manifest["files"].items():
                path=generation / name
                if path.stat().st_size!=identity["bytes"] or C.sha256_file(path)!=identity["sha256"]:
                    raise RuntimeError("native_output_changed_during_validation")
        finally:
            con.close()
    for pin in pinned:
        _pin(pin, roots, MAX_METADATA)
    _pin(plan_pin, [plan_path.parent.resolve(strict=True)], MAX_OBJECT)
    if manifest["code"] != _reader_code():
        raise RuntimeError("native_reader_code_changed_during_validation")
    if C.file_identity(manifest_path)!=before or C.sha256_file(manifest_path)!=manifest_hash:
        raise RuntimeError("native_manifest_changed_during_validation")
    return dict(structural_status="PASS",gate_status="UNQUALIFIED",manifest_sha256=manifest_hash,
                counts=manifest["counts"],status=manifest["status"])


if __name__ == "__main__":
    raise SystemExit(main())
