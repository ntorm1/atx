"""Ledgered INSERT-only Submissions replacement with explicit retained scope."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import uuid
from contextlib import suppress
from pathlib import Path

from . import submissions_stage as extract
from ._bulk_publication import _validate_shadow_contract
from .batch_runner import BatchResult, BatchSpec, BatchStage, mark_published, register_stage, run_record, store_for

STAGE_NAME = "submissions_rebuild"
TABLE = "sec_submissions"
STATE = "submissions_rebuild_state"
CHECKS = "submissions_rebuild_checks"
SLICE_ROWS = 150_000
MAX_WRITES = 2_000_000
RETIRED_INDEXES = {"idx_sec_submissions_accession", "idx_sec_submissions_security_date"}
LIVE_COLUMNS = {name for name, _ in extract.SOURCE_COLUMNS} | {"security_id", "run_id", "source_loaded_at"}


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def _sha(value):
    return hashlib.sha256(_json(value).encode()).hexdigest()


def _lit(value):
    return "'" + str(value).replace("'", "''") + "'"


def _request(conn, run_key):
    record = run_record(conn, STAGE_NAME, run_key)
    if record is None:
        raise ValueError("submissions stage needs a ledger run")
    return record["spec"]["spec"]


def _manifest(request):
    root = Path(request["staging_dir"]).resolve()
    plan = extract.load_plan(root)
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest["plan_sha256"] != extract._file_sha256(root / "plan.json"):
        raise ValueError("submissions extraction plan/manifest differs")
    for name in ("members", "ciks"):
        if extract._file_sha256(root / f"{name}.parquet") != manifest[name + "_sha256"]:
            raise ValueError("submissions member/disposition manifest drift")
    for index, expected in enumerate(manifest["batches"]):
        receipt = extract.batch_receipt(root, index, plan)
        if (receipt is None or receipt["files"] != expected["files"]
                or extract._file_sha256(root / expected["receipt"]) != expected["sha256"]):
            raise ValueError("submissions extraction batch proof differs")
    diagnostic = request.get("purpose") == "diagnostic_scratch"
    if not diagnostic and (not manifest["scope_complete"] or manifest["members_read"] != 991_042
                           or manifest["missing_history_members"] != 0
                           or manifest["item_202_candidates"] != 426_151):
        raise ValueError("full submissions source/Item 2.02 acceptance is incomplete")
    return root, plan, manifest


def _views(conn, root):
    conn.execute(f"CREATE OR REPLACE TEMP VIEW _ss_ciks AS SELECT * FROM read_parquet({_lit(root / 'ciks.parquet')})")


def _replaced(alias="t"):
    # Retain unknown/non-SEC families. The historical loader used both relative
    # and absolute archive paths, with the same canonical basename/member form.
    source = f"lower(replace({alias}.source_url, chr(92), '/'))"
    known_archive = r"(data/cache/submissions|c:/atx/atx-db/data/cache/submissions)\.zip!cik[0-9]{10}(-submissions-[0-9]+)?\.json"
    official = r"https://data\.sec\.gov/submissions/cik[0-9]{10}(-submissions-[0-9]+)?\.json"
    owned = (f"coalesce((regexp_full_match({source},{_lit(known_archive)}) OR "
             f"regexp_full_match({source},{_lit(official)})),false)")
    return (f"{owned} AND EXISTS (SELECT 1 FROM _ss_ciks m WHERE m.disposition IN ('loaded','empty') "
            f"AND regexp_full_match(trim({alias}.cik),'[0-9]+') "
            f"AND try_cast({alias}.cik AS BIGINT)=try_cast(m.cik AS BIGINT))")


def _catalog(conn):
    row = conn.execute("SELECT sql FROM duckdb_tables() WHERE database_name=current_database() "
                       "AND schema_name=current_schema() AND table_name=?", [TABLE]).fetchone()
    if not row or not row[0].startswith(f"CREATE TABLE {TABLE}("):
        raise ValueError("submissions live table catalog is absent")
    columns = conn.execute(f"DESCRIBE {TABLE}").fetchall()
    if {row[0] for row in columns} != LIVE_COLUMNS:
        raise ValueError("submissions raw column contract differs from reviewed 20-column catalog")
    indexes = conn.execute("SELECT index_name,sql FROM duckdb_indexes() WHERE database_name=current_database() "
                           "AND schema_name=current_schema() AND table_name=? ORDER BY index_name", [TABLE]).fetchall()
    if {row[0] for row in indexes} - RETIRED_INDEXES:
        raise ValueError("submissions has unreviewed indexes; no implicit retirement")
    if conn.execute("SELECT 1 FROM duckdb_constraints() WHERE table_name=? "
                    "AND constraint_type IN ('PRIMARY KEY','UNIQUE') LIMIT 1", [TABLE]).fetchone():
        raise ValueError("submissions bulk table has an unreviewed keyed constraint")
    return {"ddl": row[0], "columns": [list(row) for row in columns], "indexes": [list(row) for row in indexes]}


def _fingerprint(conn, relation):
    row = conn.execute(f"SELECT count(*),coalesce(sum(hash(t)::HUGEINT),0),coalesce(bit_xor(hash(t)),0) FROM {relation} t").fetchone()
    return [int(value) for value in row]


def _state(conn, run_key):
    row = conn.execute(f"SELECT state_json FROM {STATE} WHERE run_key=?", [run_key]).fetchall()
    record = run_record(conn, STAGE_NAME, run_key)
    if len(row) != 1 or _sha(json.loads(row[0][0])) != record["spec"]["plan"][0]["payload"]["state_sha256"]:
        raise ValueError("submissions frozen state differs")
    return json.loads(row[0][0])


def _check_source(conn, run_key, state, *, final=False):
    root, plan, manifest = _manifest(_request(conn, run_key))
    if (extract._file_sha256(root / "manifest.json") != state["manifest_sha256"]
            or _catalog(conn) != state["catalog"]
            or _fingerprint(conn, "sec_company_tickers") != state["identity_fingerprint"]):
        raise ValueError("submissions live catalog/identity/staged source drift")
    if final and _fingerprint(conn, TABLE) != state["live_fingerprint"]:
        raise ValueError("submissions live rows changed after planning")
    if final and extract._file_sha256(Path(plan["archive"]["path"])) != extract.ARCHIVE_SHA:
        raise ValueError("submissions retained archive changed before publication")
    _views(conn, root)
    return root, plan, manifest


def plan(conn, run_key):
    request = _request(conn, run_key)
    root, source_plan, manifest = _manifest(request)
    if extract._file_sha256(Path(source_plan["archive"]["path"])) != extract.ARCHIVE_SHA:
        raise ValueError("submissions retained archive changed before planning")
    if request.get("purpose") == "diagnostic_scratch":
        path = Path(conn.execute("SELECT path FROM duckdb_databases() WHERE database_name=current_database()").fetchone()[0]).resolve()
        scratch = Path("C:/atx/atx-db/data/staging/submissions").resolve()
        if not path.is_relative_to(scratch):
            raise ValueError("diagnostic submissions stage requires its isolated scratch directory")
    extract._disk(root, 512 * 1024**2)
    _views(conn, root)
    if conn.execute("SELECT 1 FROM _ss_ciks GROUP BY cik HAVING count(*)<>1 LIMIT 1").fetchone():
        raise ValueError("submissions dispositions have duplicate CIKs")
    catalog = _catalog(conn)
    if conn.execute("""WITH ranked AS (SELECT cik,security_id,dense_rank() OVER(
            PARTITION BY cik ORDER BY source_loaded_at DESC,ticker) rank FROM sec_company_tickers)
            SELECT cik FROM (SELECT DISTINCT cik,security_id FROM ranked WHERE rank=1)
            GROUP BY cik HAVING count(*)>1 LIMIT 1""").fetchone():
        raise ValueError("submissions latest current-identity candidates conflict")
    state = {"staging_dir": str(root), "manifest_sha256": extract._file_sha256(root / "manifest.json"),
        "catalog": catalog, "live_fingerprint": _fingerprint(conn, TABLE),
        "identity_fingerprint": _fingerprint(conn, "sec_company_tickers"),
        "created_at": str(conn.execute("SELECT created_at FROM build_runs WHERE stage=? AND run_key=?",
                                       [STAGE_NAME, run_key]).fetchone()[0]),
        "scope_complete": manifest["scope_complete"], "source_rows": manifest["rows"],
        "source_members": manifest["members_read"], "item_202_candidates": manifest["item_202_candidates"]}
    # Empty metadata and one small state row are the only prepare/open writes.
    conn.execute(f"CREATE TABLE IF NOT EXISTS {STATE}(run_key VARCHAR NOT NULL,state_json VARCHAR NOT NULL)")
    conn.execute(f"CREATE TABLE IF NOT EXISTS {CHECKS}(run_key VARCHAR NOT NULL,batch_id BIGINT NOT NULL,"
                 "rows BIGINT NOT NULL,hash_sum HUGEINT NOT NULL,hash_xor UBIGINT NOT NULL)")
    if conn.execute(f"SELECT 1 FROM {STATE} WHERE run_key=?", [run_key]).fetchone():
        raise ValueError("submissions state already exists without a matching ledger")
    payloads = []
    for batch in manifest["batches"]:
        item = batch["files"]["rows"]
        for offset in range(0, item["rows"], SLICE_ROWS):
            count = min(SLICE_ROWS, item["rows"]-offset)
            payloads.append({"kind": "source", "file": item, "offset": offset, "rows": count,
                             "aggregate_writes": 2*count+2})
    buckets = max(1, (state["live_fingerprint"][0] + SLICE_ROWS-1) // SLICE_ROWS)
    while True:
        counts = conn.execute(f"SELECT hash(cik,accession_number)%{buckets},count(*) FROM {TABLE} t "
                              f"WHERE NOT ({_replaced()}) GROUP BY 1 ORDER BY 1").fetchall()
        if max((row[1] for row in counts), default=0) <= SLICE_ROWS:
            break
        buckets *= 2
    for bucket, count in counts:
        payloads.append({"kind": "retained", "bucket": int(bucket), "buckets": buckets,
                         "rows": int(count), "aggregate_writes": int(count)+2})
    if not payloads:
        payloads.append({"kind": "empty", "rows": 0, "aggregate_writes": 2})
    state["retained_rows"] = sum(int(row[1]) for row in counts)
    conn.execute(f"INSERT INTO {STATE} VALUES (?,?)", [run_key, _json(state)])
    digest = _sha(state)
    return [BatchSpec(index, input_sha256=_sha(payload), payload={**payload, "state_sha256": digest})
            for index, payload in enumerate(payloads)]


def prepare(conn, run_key):
    state_row = conn.execute(f"SELECT state_json FROM {STATE} WHERE run_key=?", [run_key]).fetchone()
    state = json.loads(state_row[0])
    if conn.execute("SELECT 1 FROM duckdb_tables() WHERE table_name=?", [TABLE+"__next"]).fetchone():
        raise ValueError("submissions shadow already has an owner")
    conn.execute(state["catalog"]["ddl"].replace(f"CREATE TABLE {TABLE}(", f"CREATE TABLE {TABLE}__next(", 1))


def build(conn, spec, run_key):
    state = _state(conn, run_key)
    root, _, _ = _check_source(conn, run_key, state)
    payload = spec.payload
    semantic = {key: value for key, value in payload.items() if key != "state_sha256"}
    if (payload["state_sha256"] != _sha(state) or spec.input_sha256 != _sha(semantic)
            or not 0 <= payload["rows"] <= SLICE_ROWS or payload["aggregate_writes"] > MAX_WRITES):
        raise ValueError("submissions batch payload/count bound differs")
    target = TABLE + "__next"
    if payload["kind"] == "source":
        item = payload["file"]
        source = (root / item["file"]).resolve()
        if not source.is_relative_to(root) or extract._file_sha256(source) != item["sha256"]:
            raise ValueError("submissions batch file drift")
        conn.execute("CREATE OR REPLACE TEMP TABLE _ss_input AS SELECT * FROM read_parquet(?) "
                     "ORDER BY cik,accession_number LIMIT ? OFFSET ?", [str(source), payload["rows"], payload["offset"]])
        count = conn.execute("SELECT count(*) FROM _ss_input").fetchone()[0]
        if count != payload["rows"] or 2*count+2 != payload["aggregate_writes"]:
            raise ValueError("submissions source slice count changed")
        columns = [row[0] for row in state["catalog"]["columns"]]
        expressions = {name: f"s.{name}" for name, _ in extract.SOURCE_COLUMNS}
        expressions.update(security_id="coalesce(nullif(m.security_id,''),'SEC-CIK-' || s.cik)",
                           run_id=_lit(run_key), source_loaded_at="TIMESTAMP " + _lit(state["created_at"]))
        sql = ("SELECT " + ",".join(expressions[name] + " AS " + name for name in columns) +
               " FROM _ss_input s LEFT JOIN (SELECT cik,security_id FROM sec_company_tickers "
               "QUALIFY row_number() OVER(PARTITION BY cik ORDER BY source_loaded_at DESC,ticker)=1) m USING(cik)")
    elif payload["kind"] == "retained":
        if payload["aggregate_writes"] != payload["rows"]+2:
            raise ValueError("submissions retained write bound differs")
        sql = (f"SELECT t.* FROM {TABLE} t WHERE hash(cik,accession_number)%{int(payload['buckets'])}="
               f"{int(payload['bucket'])} AND NOT ({_replaced()})")
    elif payload["kind"] == "empty":
        sql = f"SELECT * FROM {TABLE} WHERE false"
    else:
        raise ValueError("unknown submissions batch kind")
    expected = _fingerprint(conn, "(" + sql + ")")
    if expected[0] != payload["rows"]:
        raise ValueError("submissions planned output changed")
    inserted = conn.execute(f"INSERT INTO {target} " + sql).fetchone()[0]
    if inserted != expected[0]:
        raise ValueError("submissions insert/output count differs")
    conn.execute(f"INSERT INTO {CHECKS} VALUES (?,?,?,?,?)", [run_key, spec.batch_id, *expected])
    return BatchResult(payload["rows"], inserted, _json({"kind": payload["kind"], "fingerprint": expected,
                       "aggregate_writes": payload["aggregate_writes"], "diagnostic": "noncryptographic multiset count/SUM/XOR"}))


def finalize(conn, run_key):
    state = _state(conn, run_key)
    root, _, manifest = _check_source(conn, run_key, state, final=True)
    target = TABLE+"__next"
    record = run_record(conn, STAGE_NAME, run_key)
    planned = record["spec"]["plan"]
    rows = conn.execute(f"SELECT batch_id,rows,hash_sum,hash_xor FROM {CHECKS} WHERE run_key=? ORDER BY batch_id", [run_key]).fetchall()
    ledger = conn.execute("SELECT batch_id,rows_out,note FROM build_batches WHERE stage=? AND run_key=? ORDER BY batch_id",
                          [STAGE_NAME, run_key]).fetchall()
    if len(rows) != len(planned) or len(ledger) != len(planned) or [row[0] for row in rows] != list(range(len(planned))):
        raise ValueError("submissions batch/check ledger is incomplete or duplicated")
    expected = [0, 0, 0]
    for checked, committed in zip(rows, ledger, strict=True):
        fingerprint = [int(value) for value in checked[1:]]
        if checked[0] != committed[0] or fingerprint != json.loads(committed[2])["fingerprint"] or fingerprint[0] != committed[1]:
            raise ValueError("submissions committed output proof drift")
        expected[0] += fingerprint[0]
        expected[1] += fingerprint[1]
        expected[2] ^= fingerprint[2]
    if _fingerprint(conn, target) != expected or expected[0] != state["source_rows"]+state["retained_rows"]:
        raise ValueError("submissions staged values/count differ from frozen batch output")
    if conn.execute(f"SELECT 1 FROM {target} GROUP BY cik,accession_number HAVING count(*)>1 LIMIT 1").fetchone():
        raise ValueError("submissions global (CIK,accession) uniqueness failed")
    global_candidates = conn.execute(f"SELECT count(*) FROM {target} WHERE upper(trim(form))='8-K' "
        "AND regexp_matches(coalesce(items,''),'(^|[^0-9])2\\.02([^0-9]|$)')").fetchone()[0]
    if state["scope_complete"] and global_candidates != 426_151:
        raise ValueError("submissions published Item 2.02 denominator differs from 426,151")
    receipt = {"stage": STAGE_NAME, "run_key": run_key, "scope_complete": state["scope_complete"],
        "rows": expected[0], "source_rows": state["source_rows"], "retained_rows": state["retained_rows"],
        "members_read": manifest["members_read"], "missing_history_members": manifest["missing_history_members"],
        "item_202_candidates": manifest["item_202_candidates"], "manifest_sha256": state["manifest_sha256"],
        "global_item_202_candidates": global_candidates,
        "dispositions": manifest["dispositions"], "duplicate_cik_accession_keys": 0,
        "retired_indexes": state["catalog"]["indexes"], "column_default_contract_preserved": True,
        "staged_fingerprint": expected, "fingerprint_basis": "noncryptographic multiset count/SUM/XOR",
        "max_aggregate_batch_writes": max(item["payload"]["aggregate_writes"] for item in planned)}
    source_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{STAGE_NAME}:{run_key}:source"))
    check_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{STAGE_NAME}:{run_key}:unique"))
    for table, field, key in (("dataset_runs", "run_id", run_key), ("raw_source_files", "source_id", source_id),
                              ("data_quality_checks", "check_id", check_id)):
        if conn.execute(f"SELECT 1 FROM {table} WHERE {field}=?", [key]).fetchone():
            raise ValueError("submissions publication metadata identity already exists")
    store = store_for(conn)
    try:
        with store.transaction():
            _validate_shadow_contract(store, live_table=TABLE, shadow_table=target)
            conn.execute("INSERT INTO raw_source_files(source_id,dataset_id,source_url,cache_path,sha256,byte_count,"
                         "fetched_at,status,metadata_json) VALUES (?,?,?,?,?,?,?,?,?)",
                         [source_id, "sec_submissions", manifest["archive"]["path"], manifest["archive"]["path"],
                          extract.ARCHIVE_SHA, manifest["archive"]["bytes"], dt.datetime.fromisoformat(extract.OBSERVED_AT),
                          "loaded", _json(receipt)])
            conn.execute("INSERT INTO dataset_runs(run_id,dataset_id,status,started_at,finished_at,rows_loaded,source,params_json,error_message) "
                         "VALUES (?,'sec_submissions','completed',?,?,?,'SEC submissions bulk archive',?,NULL)",
                         [run_key, dt.datetime.fromisoformat(state["created_at"]), dt.datetime.now(dt.UTC).replace(tzinfo=None),
                          state["source_rows"], _json(_request(conn, run_key))])
            conn.execute("INSERT INTO data_quality_checks(check_id,dataset_id,table_name,check_name,status,severity,"
                         "observed_value,threshold_value,details_json,checked_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
                         [check_id, "sec_submissions", TABLE, "global_cik_accession_uniqueness", "passed", "critical", 0, 0,
                          _json(receipt), dt.datetime.now(dt.UTC).replace(tzinfo=None)])
            mark_published(conn, STAGE_NAME, run_key, receipt)
            # DROP the old DEFAULT-now relation, never ALTER/UPDATE/DELETE it.
            # Its two reviewed secondary indexes retire with it; the INSERT-only
            # shadow retains the exact columns/defaults and has no analytic ART.
            conn.execute(f"DROP TABLE {TABLE}")
            conn.execute(f"ALTER TABLE {target} RENAME TO {TABLE}")
    except BaseException:
        with suppress(Exception):
            conn.execute("ROLLBACK")
        raise
    return receipt


def discard(conn, run_key):
    _request(conn, run_key)
    conn.execute(f"DROP TABLE IF EXISTS {TABLE}__next")


STAGE = BatchStage(STAGE_NAME, plan, prepare, build, finalize, memory_limit="256MB", threads=1,
    discard=discard, code_dependencies=("submissions_stage.py", "companyfacts_stage.py", "_submissions_archive.py", "_bulk_publication.py"))
register_stage(STAGE)
