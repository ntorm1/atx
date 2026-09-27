"""C112 ledger materialization of immutable, unpublished identity producer output.

Producer computation and this consumer have separate source identities. This
version admits diagnostic scratch databases only: publishing a scratch ledger
run never grants identity, coverage, release, or production acceptance.
"""
from __future__ import annotations

import hashlib
import json
import shutil
from collections import Counter
from pathlib import Path

from . import identity_links as identity
from ._bulk_publication import publish_validated_shadows
from .batch_runner import BatchResult, BatchSpec, BatchStage, mark_published, register_stage, run_record, store_for

STAGE_NAME = "identity_links_rebuild"
ROWS_PER_BATCH = 20_000
MAX_SOURCE_ROWS = 1_000_000
SCRATCH_ROOT = Path(__file__).resolve().parents[2] / "data/research/identity_rehearsal"


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def _digest(value):
    return hashlib.sha256(_json(value).encode()).hexdigest()


def _checked(path, expected):
    path = Path(path).resolve()
    if identity.file_sha256(path) != expected:
        raise ValueError(f"identity stage content drift: {path}")
    return path


def _child(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError("identity producer artifact escaped its pinned directory")
    return path


def _request(conn, run_key):
    record = run_record(conn, STAGE_NAME, run_key)
    if record is None:
        raise ValueError("identity stage needs a ledger run")
    request = record["spec"]["spec"]
    if request.get("purpose") != "diagnostic_scratch":
        raise ValueError("release identity remains gated; this consumer accepts diagnostic scratch only")
    databases = [Path(row[2]).resolve() for row in conn.execute("PRAGMA database_list").fetchall() if row[2]]
    if len(databases) != 1 or not databases[0].is_relative_to(SCRATCH_ROOT.resolve()):
        raise ValueError("identity diagnostic stage requires an isolated identity rehearsal database")
    return request


def _manifest(request, *, transitive=False):
    path = _checked(request["manifest_path"], request["manifest_sha256"])
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if (manifest["schema_version"] != identity.SCHEMA_VERSION or
            manifest["allocation_version"] != identity.ALLOCATION_VERSION or
            manifest["snapshot_date"] != "2026-09-20" or not manifest["rehearsal"]):
        raise ValueError("identity producer contract differs")
    audit_path = _checked(request["audit_path"], request["audit_sha256"])
    audit = json.loads(audit_path.read_text(encoding="utf-8"))
    if (audit.get("audit_schema") != "identity_rehearsal_audit_v1" or
            audit.get("manifest_sha256") != request["manifest_sha256"] or
            audit["foreign_key_and_bitemporal_overlap_violations"] != 0 or
            audit["relational"]["duplicate_keys"] != 0 or
            audit["relational"]["allocation_violations"] != 0 or
            audit["relational"]["missing_or_future_evidence"] != 0 or
            audit["relational"]["names_covered_lines"] != 25_760):
        raise ValueError("identity structural audit is absent, mismatched, or failed")
    if not transitive:
        return path, manifest, audit
    root = path.parent
    build_path = root / "build.json"
    build = json.loads(build_path.read_text(encoding="utf-8"))
    pin = build["pin"]
    for key in ("allocation_version", "preparation_manifest_sha256", "retained_database_sha256",
                "ops_manifest_sha256", "code_files", "inputs", "document_review"):
        if manifest[key] != pin[key]:
            raise ValueError(f"identity build/manifest pin mismatch: {key}")
    for name, sha in manifest["code_files"].items():
        _checked(_child(root / "code_snapshot", name), sha)
    # Git's core.autocrlf checkout can change the byte representation after a
    # producer commit. Retained producer bytes remain exactly SHA-pinned above;
    # the separately pinned consumer may use only LF/CRLF-equivalent helpers.
    frozen_source = (root / "code_snapshot/identity_links.py").read_bytes()
    runtime_source = Path(identity.__file__).read_bytes()
    if frozen_source.replace(b"\r\n", b"\n") != runtime_source.replace(b"\r\n", b"\n"):
        raise ValueError("identity runtime helpers differ from the frozen producer")
    for item in manifest["inputs"].values():
        _checked(item["path"], item["sha256"])
    preparation = _checked(request["preparation_manifest_path"], manifest["preparation_manifest_sha256"])
    prepared = json.loads(preparation.read_text(encoding="utf-8"))
    _checked(preparation.parent / "retained.duckdb", manifest["retained_database_sha256"])
    if prepared["pin"]["inputs"] != manifest["inputs"]:
        raise ValueError("identity preparation inputs differ")
    for step in prepared["steps"].values():
        for item in step.get("files", []):
            _checked(_child(preparation.parent, item["file"]), item["sha256"])
    ops_path = _checked(request["ops_manifest_path"], manifest["ops_manifest_sha256"])
    for item in json.loads(ops_path.read_text(encoding="utf-8"))["exports"]:
        if item.get("file"):
            _checked(_child(ops_path.parent, item["file"]), item["sha256"])
    if manifest.get("document_review"):
        review = manifest["document_review"]
        if identity._document_review_pin(_checked(review["path"], review["sha256"])) != review:
            raise ValueError("identity document review pins differ")
    if set(manifest["phases"]) != set(identity.REHEARSAL_PHASES):
        raise ValueError("identity producer phase scope is incomplete")
    build_sha = identity.file_sha256(build_path)
    phases = {}
    for name in identity.REHEARSAL_PHASES:
        phase_path = _checked(root / "phases" / f"{name}.json", manifest["phases"][name])
        phase = json.loads(phase_path.read_text(encoding="utf-8"))
        if phase["phase"] != name or phase["build_sha256"] != build_sha:
            raise ValueError("identity producer phase/build mismatch")
        launch_path = _child(root, phase["launch"])
        launch = json.loads(launch_path.read_text(encoding="utf-8"))
        if launch["pin"] != pin or launch["build_sha256"] != build_sha:
            raise ValueError("identity phase launch pin drift")
        if launch["parents"] != {prior:manifest["phases"][prior] for prior in phases}:
            raise ValueError("identity phase parent chain differs")
        for item in phase["files"]:
            _checked(_child(root, item["file"]), item["sha256"])
        phases[name] = phase
    if manifest["files"] != phases["export"]["files"]:
        raise ValueError("identity final files differ from sealed export")
    counts = Counter()
    for item in manifest["files"]:
        if item["table"] not in identity.TABLE_COLUMNS or not 0 <= item["rows"] <= MAX_SOURCE_ROWS:
            raise ValueError("identity source partition exceeds bounded schema/row contract")
        counts[item["table"]] += item["rows"]
    if dict(counts) != audit["relational"]["rows"]:
        raise ValueError("identity audited row denominators differ")
    return path, manifest, audit


def plan(conn, run_key):
    request = _request(conn, run_key)
    path, manifest, _ = _manifest(request, transitive=True)
    budget = 4 * sum(item["bytes"] for item in manifest["files"]) + 512 * 1024**2
    if shutil.disk_usage(SCRATCH_ROOT).free < 35 * 1024**3 + budget:
        raise ValueError("identity consumer would cross the disk reserve")
    batches = []
    for item in manifest["files"]:
        for offset in range(0, item["rows"], ROWS_PER_BATCH):
            count = min(ROWS_PER_BATCH, item["rows"]-offset)
            payload = {"request":request, "file":item, "offset":offset, "rows":count,
                "path":str(_child(path.parent,item["file"])),
                "transaction_rows":count+1, "source_scan_rows":item["rows"]}
            if payload["transaction_rows"] + payload["source_scan_rows"] > 2_000_000:
                raise ValueError("identity aggregate batch bound exceeded")
            batches.append(BatchSpec(len(batches),str(offset),str(offset+count-1),_digest(payload),payload))
    return batches


def prepare(conn, run_key):
    _request(conn, run_key)
    for table, columns in identity.TABLE_COLUMNS.items():
        if conn.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name=?",[table+"__next"]).fetchone()[0]:
            raise ValueError(f"unowned identity shadow already exists: {table}__next")
        if conn.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name=?",[table]).fetchone()[0] != 1:
            raise ValueError("identity live schema must be created by its versioned migration")
        conn.execute(f"CREATE TABLE {table}__next ({columns})")


def build(conn, spec, run_key):
    request = _request(conn, run_key)
    payload = spec.payload
    if payload["request"] != request or spec.input_sha256 != _digest(payload):
        raise ValueError("identity frozen batch payload drift")
    path, manifest, _ = _manifest(request)
    item = payload["file"]
    if item not in manifest["files"] or str(_child(path.parent,item["file"])) != payload["path"]:
        raise ValueError("identity batch source is outside the manifest")
    source = _checked(payload["path"],item["sha256"])
    if (payload["rows"] > ROWS_PER_BATCH or payload["offset"] < 0 or
            payload["offset"]+payload["rows"] > item["rows"] or
            payload["source_scan_rows"] != item["rows"] or
            payload["transaction_rows"] != payload["rows"]+1 or
            payload["source_scan_rows"]+payload["transaction_rows"] > 2_000_000):
        raise ValueError("identity batch range/aggregate bound differs")
    table = item["table"]
    columns = ",".join(column.split()[0] for column in identity.TABLE_COLUMNS[table].split(", "))
    order = ",".join(identity.TABLE_KEYS[table])
    inserted = conn.execute(f"INSERT INTO {table}__next ({columns}) SELECT {columns} "
        f"FROM read_parquet(?) ORDER BY {order} LIMIT ? OFFSET ?",
        [str(source),payload["rows"],payload["offset"]]).fetchone()[0]
    if inserted != payload["rows"]:
        raise ValueError("identity batch source row denominator changed")
    return BatchResult(item["rows"],inserted,_json({"table":table,"rows":inserted,
        "source_sha256":item["sha256"],"transaction_rows":inserted+1}))


def finalize(conn, run_key):
    request = _request(conn, run_key)
    path, manifest, audit = _manifest(request, transitive=True)
    counts = {}
    for table, keys in identity.TABLE_KEYS.items():
        target = table+"__next"
        if conn.execute(f"SELECT 1 FROM {target} GROUP BY {','.join(keys)} HAVING count(*)>1 LIMIT 1").fetchone():
            raise ValueError(f"duplicate identity shadow key: {table}")
        sources = [_child(path.parent,item["file"]) for item in manifest["files"] if item["table"]==table]
        source_sql = "read_parquet(["+",".join(identity._literal(str(p)) for p in sources)+"])"
        for left,right in ((target,source_sql),(source_sql,target)):
            if conn.execute(f"SELECT 1 FROM (SELECT * FROM {left} EXCEPT ALL SELECT * FROM {right}) LIMIT 1").fetchone():
                raise ValueError(f"identity shadow differs from audited producer values: {table}")
        counts[table] = conn.execute(f"SELECT count(*) FROM {target}").fetchone()[0]
    if counts != audit["relational"]["rows"] or counts["security_permanent_ids"] != 25_760:
        raise ValueError("identity shadow denominator differs")
    for table, columns in (("security_permanent_ids","perm_security_id,security_id"),
                           ("company_permanent_ids","perm_company_id,cik")):
        if conn.execute(f"SELECT 1 FROM (SELECT {columns} FROM {table} EXCEPT SELECT {columns} FROM {table}__next) LIMIT 1").fetchone():
            raise ValueError("identity permanent ID removed or reused")
    for table in ("security_company_links","identity_unresolved_memberships"):
        if conn.execute(f"""SELECT 1 FROM {table}__next l
            LEFT JOIN company_permanent_ids__next c USING(perm_company_id)
            LEFT JOIN security_permanent_ids__next s USING(perm_security_id)
            WHERE c.perm_company_id IS NULL OR (l.perm_security_id IS NOT NULL AND s.perm_security_id IS NULL)
                OR l.link_start>l.link_end OR l.available_at>=l.valid_until LIMIT 1""").fetchone():
            raise ValueError("identity shadow foreign key/business/evidence interval failure")
        if conn.execute(f"""WITH refs AS (SELECT available_at,unnest(from_json(evidence_ids,'["VARCHAR"]')) evidence_id
            FROM {table}__next) SELECT 1 FROM refs r LEFT JOIN identity_link_evidence__next e USING(evidence_id)
            WHERE e.evidence_id IS NULL OR e.available_at>r.available_at LIMIT 1""").fetchone():
            raise ValueError("identity shadow missing or future evidence")
    record = run_record(conn, STAGE_NAME, run_key)
    planned = record["spec"]["plan"]
    totals = conn.execute("SELECT count(*),sum(rows_out) FROM build_batches WHERE stage=? AND run_key=?",
                          [STAGE_NAME,run_key]).fetchone()
    if totals != (len(planned),sum(counts.values())):
        raise ValueError("identity committed ledger/output denominator differs")
    receipt = {"stage":STAGE_NAME,"run_key":run_key,"rows":counts,"purpose":"diagnostic_scratch",
        "accepted":False,"manifest_sha256":request["manifest_sha256"],"audit_sha256":request["audit_sha256"],
        "max_transaction_rows":max(p["payload"]["transaction_rows"] for p in planned),
        "exact_producer_multiset_equality":True,"remaining_acceptance":manifest["remaining_acceptance"]}
    publish_validated_shadows(store_for(conn),tables=tuple((table,table+"__next") for table in identity.TABLE_COLUMNS),
        before_swap=lambda:mark_published(conn,STAGE_NAME,run_key,receipt))
    return receipt


def discard(conn, run_key):
    _request(conn, run_key)
    for table in identity.TABLE_COLUMNS:
        conn.execute(f"DROP TABLE IF EXISTS {table}__next")


STAGE = BatchStage(STAGE_NAME,plan,prepare,build,finalize,memory_limit="256MB",threads=1,
    discard=discard,code_dependencies=("identity_links.py","_bulk_publication.py"))
register_stage(STAGE)
