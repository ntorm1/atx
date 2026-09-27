"""Bounded CompanyFacts publication from the retained, complete CF-R archive.

The archive and the C87 comparison are deliberately pinned.  New concepts are
published, but only the 255 load-time concepts enter the 9,462-CIK comparison.
Unknown historical identity remains unknown; no current-ticker fallback exists.
``prepare`` writes only empty shadows and small metadata.  All retained rows,
point-ownership checks and archive outputs are ledgered, restartable batches.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import uuid
from collections.abc import Callable
from contextlib import suppress
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import duckdb

from ._bulk_publication import _validate_shadow_contract
from ._companyfacts_resume import _digest_sums, _lineage, _projection
from ._table_swap import replace_rows_by_swap, table_columns
from .batch_runner import (
    BatchResult,
    BatchSpec,
    BatchStage,
    mark_published,
    register_stage,
    run_record,
    store_for,
    utc_now,
)

STAGE_NAME = "companyfacts_rebuild"
ARCHIVE_SHA = "ee099c7394a357f1996c728b7362f25158613b34f2ab1ad270cffc1befb917b6"
MANIFEST_SHA = "50e018e1c26046f3eb3ec27d2f246c8492e60baddbc911ebfda50320f24ac186"
MEMBERS_SHA = "07943ae309b8a5d10c2fd3caf6fb7110e0830d6e90676d988515cf4465995c0c"
OLD_ALLOWLIST_SHA = "ccfe31cf84b2f8c7eac978cfb19b6fd1e0494410cfcd0f969f98f030a7f6cdaf"
ALLOWLIST_SHA = "ac3ea0b5fda6dbf2c1fd955eadb592b1cb9c0a7a79587a21d6d6993f7a246610"
LINEAGE_HEAD = "4c69bc32-60a7-4354-adea-f1ee6c096eec"
SOURCE = "SEC companyfacts"
SOURCE_URL = "https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip"
MATCH_METHOD = "companyfacts_cik_spine_asof"
FACTS, POINTS = "sec_company_facts", "fundamental_points"
CANDIDATES = "identifier_resolution_candidates"
MAX_WRITES = 2_000_000
MAX_INPUT_ROWS = 300_000
EXPECTED_CIKS, EXPECTED_OLD_ROWS = 9_462, 39_457_715
_META = "cf_rebuild_state"
_VERIFIED = "cf_rebuild_verified"
_CHECKED = "cf_rebuild_checked"
_CANDIDATES_NEXT = "cf_candidates__next"
_OUTPUT_TABLES = (FACTS + "__next", POINTS + "__next", _CANDIDATES_NEXT, _CHECKED)


def _json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def _sha(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def _literal(value: object) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def _inputs(root: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    if _sha(root / "manifest.json") != MANIFEST_SHA or _sha(root / "members.parquet") != MEMBERS_SHA:
        raise ValueError("CF-R manifest or member dispositions differ from the accepted source pins")
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if (manifest["archive"]["sha256"] != ARCHIVE_SHA or manifest["allowlist"]["allowlist_sha256"] != ALLOWLIST_SHA
            or manifest["totals"]["rows"] != 53_751_273 or manifest["totals"]["members"] != 20_390
            or manifest["totals"]["error"] != 0 or len(manifest["batches"]) != 85):
        raise ValueError("CF-R archive, allowlist or complete-member contract changed")
    members: list[dict[str, Any]] = []
    for batch in manifest["batches"]:
        path = root / batch["file"]
        if path.name != f"batch-{batch['batch_id']:04d}.parquet" or path.parent != root:
            raise ValueError("invalid CF-R batch path")
        sidecar = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
        if (sidecar["archive_sha256"] != ARCHIVE_SHA or sidecar["batch_id"] != batch["batch_id"]
                or sum(m["rows"] for m in sidecar["members"]) != batch["rows"]
                or len(sidecar["members"]) != batch["members"]
                or [m["cik"] for m in sidecar["members"]] != sorted(m["cik"] for m in sidecar["members"])
                or int(sidecar["members"][0]["cik"]) != batch["first_cik"]
                or int(sidecar["members"][-1]["cik"]) != batch["last_cik"]):
            raise ValueError(f"CF-R batch {batch['batch_id']} sidecar differs")
        for member in sidecar["members"]:
            members.append({**member, "batch_id": batch["batch_id"]})
    if len(members) != 20_390 or len({m["cik"] for m in members}) != len(members):
        raise ValueError("CF-R member inventory is incomplete or ambiguous")
    return manifest, members


def _member_view(conn: duckdb.DuckDBPyConnection, root: Path) -> None:
    conn.execute("CREATE OR REPLACE TEMP VIEW _cf_members AS SELECT * FROM read_parquet("
                 + _literal((root / "members.parquet").as_posix()) + ")")


def _replaced(alias: str) -> str:
    # Numeric equivalence handles historical non-padded CIKs; invalid identities
    # are retained, not silently coerced into a selected member.
    return (f"{alias}.source = {_literal(SOURCE)} AND EXISTS (SELECT 1 FROM _cf_members m "
            f"WHERE m.disposition IN ('loaded','empty') AND regexp_full_match(trim({alias}.cik), '[0-9]+') "
            f"AND try_cast({alias}.cik AS BIGINT) = try_cast(m.cik AS BIGINT))")


def _fingerprints(conn: duckdb.DuckDBPyConnection, table: str, key: str, buckets: int) -> dict[str, list[int]]:
    return {str(b): [int(n), int(s), int(x)] for b, n, s, x in conn.execute(
        f"SELECT hash({key}) % {buckets}, count(*), sum(hash(t)::HUGEINT), bit_xor(hash(t)) "
        f"FROM {table} t GROUP BY 1").fetchall()}


def _catalog_pin(conn: duckdb.DuckDBPyConnection, table: str) -> dict[str, object]:
    ddl = conn.execute("SELECT sql FROM duckdb_tables() WHERE database_name=current_database() "
                       "AND schema_name=current_schema() AND table_name=?", [table]).fetchone()
    if not ddl or not str(ddl[0]).startswith(f"CREATE TABLE {table}("):
        raise ValueError(f"cannot derive exact live DDL for {table}")
    indexes = conn.execute("SELECT index_name, sql FROM duckdb_indexes() WHERE database_name=current_database() "
                           "AND schema_name=current_schema() AND table_name=? ORDER BY index_name", [table]).fetchall()
    return {"ddl": str(ddl[0]), "indexes": [list(row) for row in indexes]}


def _receipt_inventory(conn: duckdb.DuckDBPyConnection, manifest: dict[str, Any]) -> dict[str, Any]:
    params_row = conn.execute("SELECT params_json FROM dataset_runs WHERE run_id=?", [LINEAGE_HEAD]).fetchone()
    if not params_row:
        raise ValueError("C87 lineage head is absent; the 9,462-CIK gate cannot be weakened")
    params = json.loads(params_row[0])
    concepts = sorted(set(params["concepts"]))
    fingerprint = hashlib.sha256(json.dumps({"concepts": concepts, "taxonomies": ("us-gaap", "dei")},
                                           sort_keys=True, default=str).encode()).hexdigest()
    if fingerprint != OLD_ALLOWLIST_SHA or len(concepts) != 255:
        raise ValueError("C87 lineage allowlist is not the frozen 255-concept definition")
    options = SimpleNamespace(**{**params, "companyfacts_zip": Path(params["companyfacts_zip"]),
                                "as_of_date": dt.date.fromisoformat(params["as_of_date"]) if params["as_of_date"] else None,
                                "run_id": str(uuid.uuid4()), "resume_from_run_id": LINEAGE_HEAD})
    lineage = _lineage(store_for(conn), options)
    receipts: list[dict[str, object]] = []
    rows = conn.execute("SELECT source_url, sha256, byte_count, fetched_at, status, metadata_json "
                        "FROM raw_source_files WHERE dataset_id='sec_company_facts' AND cache_path=?",
                        [params["companyfacts_zip"]]).fetchall()
    for url, sha, size, fetched, status, raw in rows:
        try:
            meta = json.loads(raw)
        except (ValueError, TypeError):
            continue
        if not isinstance(meta, dict) or meta.get("run_id") not in lineage or status not in ("loaded", "empty"):
            continue
        cik, count = meta.get("cik"), meta.get("rows")
        start, end = lineage[meta["run_id"]]
        if (not isinstance(cik, str) or not re.fullmatch(r"[0-9]{10}", cik)
                or url != f"{SOURCE_URL}#CIK{cik}.json" or meta.get("symbol") != f"CIK{cik}"
                or meta.get("source_mode") != "bulk_zip" or sha != ARCHIVE_SHA
                or meta.get("archive_sha256") != ARCHIVE_SHA or meta.get("allowlist_sha256") != OLD_ALLOWLIST_SHA
                or size != manifest["archive"]["bytes"] or fetched is None or not start <= fetched <= end
                or type(count) is not int or (count <= 0 if status == "loaded" else count != 0)):
            raise ValueError(f"C87 source receipt identity, scope, count or timing differs for {cik}")
        if status == "loaded":
            receipts.append({"cik": cik, "run_id": meta["run_id"], "rows": count})
    if (len(receipts) != EXPECTED_CIKS or len({r["cik"] for r in receipts}) != EXPECTED_CIKS
            or sum(int(r["rows"]) for r in receipts) != EXPECTED_OLD_ROWS):
        raise ValueError(f"C87 requires exactly {EXPECTED_CIKS} verified CIK receipts / {EXPECTED_OLD_ROWS} rows")
    return {"concepts": concepts, "receipts": sorted(receipts, key=lambda r: str(r["cik"])),
            "lineage": list(lineage), "lineage_head": LINEAGE_HEAD}


def _state(conn: duckdb.DuckDBPyConnection, run_key: str) -> dict[str, Any]:
    rows = conn.execute(f"SELECT state_json FROM {_META} WHERE run_key=?", [run_key]).fetchall()
    if len(rows) != 1:
        raise ValueError("CF-R has no unique frozen state")
    record = run_record(conn, STAGE_NAME, run_key)
    expected = record["spec"]["plan"][0]["payload"]["state_sha256"] if record else None
    if hashlib.sha256(str(rows[0][0]).encode()).hexdigest() != expected:
        raise ValueError("CF-R state differs from the frozen plan")
    return json.loads(rows[0][0])


def plan(conn: duckdb.DuckDBPyConnection, run_key: str) -> list[BatchSpec]:
    if run_key != ARCHIVE_SHA[:16]:
        raise ValueError("CF-R run_key must be the frozen archive SHA prefix")
    record = run_record(conn, STAGE_NAME, run_key)
    requested = record["spec"]["spec"] if record else {}
    if set(requested) - {"staging_dir", "as_of_date"}:
        raise ValueError("unknown CF-R spec keys; gates and source pins cannot be overridden")
    root = Path(str(requested.get("staging_dir", f"data/staging/companyfacts/{run_key}"))).resolve()
    manifest, members = _inputs(root)
    _member_view(conn, root)
    # The JSON sidecars must describe the authoritative, hash-pinned member file.
    expected_members = sorted((m["cik"], m["disposition"], m["rows"]) for m in members)
    if conn.execute("SELECT cik, disposition, rows FROM _cf_members ORDER BY cik").fetchall() != expected_members:
        raise ValueError("CF-R sidecar member dispositions differ from the pinned members file")
    proof = _receipt_inventory(conn, manifest)
    loaded = {m["cik"] for m in members if m["disposition"] == "loaded"}
    if any(r["cik"] not in loaded for r in proof["receipts"]):
        raise ValueError("C87 verified issuer is not loadable in the pinned archive")
    pins = {table: _catalog_pin(conn, table) for table in (FACTS, POINTS, CANDIDATES)}
    for table in (FACTS, POINTS):
        if pins[table]["indexes"]:
            raise ValueError(f"{table} cannot carry secondary indexes for the INSERT-only shadow publication")
    buckets = 16
    while True:
        f_counts = dict(conn.execute(f"SELECT hash(security_id)%{buckets}, count(*) FROM {FACTS} "
                                    "WHERE source=? GROUP BY 1", [SOURCE]).fetchall())
        p_counts = dict(conn.execute(f"SELECT hash(security_id)%{buckets}, count(*) FROM {POINTS} GROUP BY 1").fetchall())
        # A key row per old fact, a retained output per point, small proof groups
        # at most twice the fact count, plus one coupled build ledger row.
        if max((3 * f_counts.get(b, 0) + p_counts.get(b, 0) + 1 for b in range(buckets)), default=0) <= MAX_WRITES:
            break
        buckets *= 2
        if buckets > 4096:
            raise ValueError("one historical security exceeds the CF-R point ownership transaction bound")
    fact_buckets = 16
    while True:
        retained = dict(conn.execute(f"SELECT hash(cik)%{fact_buckets}, count(*) FROM {FACTS} f "
                                    f"WHERE NOT ({_replaced('f')}) GROUP BY 1").fetchall())
        if max(retained.values(), default=0) < MAX_WRITES:
            break
        fact_buckets *= 2
        if fact_buckets > 4096:
            raise ValueError("one retained CIK exceeds the CF-R transaction bound")
    state = {"root": str(root), "manifest_sha256": MANIFEST_SHA, "members_sha256": MEMBERS_SHA,
             "run_id": str(uuid.uuid4()), "loaded_at": utc_now().isoformat(),
             "as_of_date": str(requested.get("as_of_date") or "2026-09-27"),
             "catalog": pins, "proof": proof, "point_buckets": buckets, "fact_buckets": fact_buckets,
             "facts_pin": _fingerprints(conn, FACTS, "cik", fact_buckets),
             "points_pin": _fingerprints(conn, POINTS, "security_id", buckets),
             "identity_pin": _fingerprints(conn, "security_identifier_history", "security_id", 1),
             "candidate_pin": _fingerprints(conn, CANDIDATES, "candidate_id", 1)}
    dt.date.fromisoformat(state["as_of_date"])
    planned: list[BatchSpec] = []
    state_sha = hashlib.sha256(_json(state).encode()).hexdigest()

    def add(payload: dict[str, object]) -> None:
        payload["state_sha256"] = state_sha
        planned.append(BatchSpec(len(planned), input_sha256=hashlib.sha256(_json(payload).encode()).hexdigest(),
                                 payload=payload))

    for bucket in range(fact_buckets):
        add({"kind": "retained_facts", "bucket": bucket, "buckets": fact_buckets,
             "rows": retained.get(bucket, 0)})
    for bucket in range(buckets):
        add({"kind": "retained_points", "bucket": bucket, "buckets": buckets,
             "max_writes": 3 * f_counts.get(bucket, 0) + p_counts.get(bucket, 0) + 1})
    for batch in manifest["batches"]:
        path = root / batch["file"]
        if _sha(path) != batch["parquet_sha256"]:
            raise ValueError(f"CF-R input digest mismatch: {path.name}")
        group: list[dict[str, Any]] = []
        rows = 0
        for member in (m for m in members if m["batch_id"] == batch["batch_id"] and m["rows"]):
            if member["rows"] > MAX_INPUT_ROWS:
                raise ValueError(f"CIK {member['cik']} needs a reviewed smaller whole-CIK bound")
            if group and rows + member["rows"] > MAX_INPUT_ROWS:
                add({"kind": "archive", "source_batch": batch["batch_id"], "file": batch["file"],
                     "sha256": batch["parquet_sha256"], "lo": group[0]["cik"], "hi": group[-1]["cik"], "rows": rows})
                group, rows = [], 0
            group.append(member)
            rows += member["rows"]
        if group:
            add({"kind": "archive", "source_batch": batch["batch_id"], "file": batch["file"],
                 "sha256": batch["parquet_sha256"], "lo": group[0]["cik"], "hi": group[-1]["cik"], "rows": rows})
    # One temporary metadata row; no warehouse rows are copied in open_run.
    conn.execute("CREATE TEMP TABLE _cf_open_state AS SELECT ?::VARCHAR AS state_json", [_json(state)])
    conn.execute("DROP VIEW _cf_members")
    return planned


def prepare(conn: duckdb.DuckDBPyConnection, run_key: str) -> None:
    state = json.loads(conn.execute("SELECT state_json FROM _cf_open_state").fetchone()[0])
    for live, shadow in ((FACTS, FACTS + "__next"), (POINTS, POINTS + "__next"), (CANDIDATES, _CANDIDATES_NEXT)):
        ddl = str(state["catalog"][live]["ddl"])
        conn.execute(f"CREATE TABLE {shadow}" + ddl[len(f"CREATE TABLE {live}"):])
    conn.execute(f"CREATE TABLE {_META} (run_key VARCHAR, state_json VARCHAR)")
    conn.execute(f"INSERT INTO {_META} VALUES (?, ?)", [run_key, _json(state)])
    conn.execute(f"CREATE TABLE {_VERIFIED} (cik VARCHAR, run_id VARCHAR, rows BIGINT)")
    conn.execute(f"INSERT INTO {_VERIFIED} SELECT r.cik,r.run_id,r.rows FROM "
                 "UNNEST(from_json(?, '[{\"cik\":\"VARCHAR\",\"run_id\":\"VARCHAR\",\"rows\":\"BIGINT\"}]')) t(r)",
                 [_json(state["proof"]["receipts"])])
    conn.execute(f"CREATE TABLE {_CHECKED} (cik VARCHAR, rows BIGINT, digest_json VARCHAR)")
    conn.execute("DROP TABLE _cf_open_state")


def _duplicates(conn: duckdb.DuckDBPyConnection, selection: str) -> None:
    rows = conn.execute(f"SELECT cik, taxonomy, concept, unit, period_start, period_end, accession_number, filed_date, "
                        f"count(*) n FROM ({selection}) GROUP BY ALL HAVING count(*)>1 LIMIT 12").fetchall()
    if rows:
        raise ValueError(f"CF-R duplicate natural keys (no arbitrary winner): {rows}")


def _multiset(conn: duckdb.DuckDBPyConnection, selection: str) -> list[int]:
    """All-column noncryptographic drift diagnostic; C87 separately uses SHA-256."""
    row = conn.execute("SELECT count(*),coalesce(sum(hash(t)::HUGEINT),0),coalesce(bit_xor(hash(t)),0) "
                       f"FROM ({selection}) t").fetchone()
    return [int(value) for value in row]


def _assert_output_fingerprints(conn: duckdb.DuckDBPyConnection, notes: list[dict[str, Any]]) -> None:
    expected = {table: [0, 0, 0] for table in _OUTPUT_TABLES}
    for note in notes:
        for table, values in note["fingerprints"].items():
            if table not in expected or len(values) != 3:
                raise ValueError("invalid staged-output fingerprint in the build ledger")
            expected[table][0] += int(values[0])
            expected[table][1] += int(values[1])
            expected[table][2] ^= int(values[2])
    for table, frozen in expected.items():
        actual = _multiset(conn, f"SELECT * FROM {table}")
        if actual != frozen:
            raise ValueError(f"staged {table} all-column fingerprint differs from committed outputs: "
                             f"expected={frozen}, actual={actual}")


def _candidate_selection() -> str:
    predicate = (f"NOT (match_method={_literal(MATCH_METHOD)} AND source_dataset_id='sec_company_facts' "
                 "AND source_key_type='CIK' AND EXISTS (SELECT 1 FROM _cf_members m "
                 "WHERE m.disposition IN ('loaded','empty') "
                 "AND regexp_full_match(trim(source_key_value),'[0-9]+') "
                 "AND try_cast(source_key_value AS BIGINT)=try_cast(m.cik AS BIGINT)))")
    return f"SELECT * FROM {CANDIDATES} WHERE {predicate} UNION ALL SELECT * FROM {_CANDIDATES_NEXT}"


def _validate_candidate_selection(conn: duckdb.DuckDBPyConnection, selected: str, metadata_writes: int) -> int:
    duplicates = conn.execute(f"SELECT candidate_id,count(*) FROM ({selected}) GROUP BY 1 "
                              "HAVING count(*)>1 OR candidate_id IS NULL LIMIT 20").fetchall()
    if duplicates:
        raise ValueError(f"candidate publication union has invalid or duplicate keys: {duplicates}")
    count = int(conn.execute(f"SELECT count(*) FROM ({selected})").fetchone()[0])
    if count + metadata_writes > MAX_WRITES:
        raise ValueError("candidate publication plus metadata exceeds aggregate transaction row bound")
    return count


def _common(facts: bool, *, with_run: bool = False, alias: str | None = None) -> str:
    projection = _projection(facts=facts)
    needle = ", run_id := run_id"
    if projection.count(needle) != 1:
        raise ValueError("C87 shared projection changed; review its exact field contract")
    selected = projection if with_run else projection.replace(needle, "")
    return selected if alias is None else re.sub(r":= (\w+)", rf":= {alias}.\1", selected)


def _retain_facts(conn: duckdb.DuckDBPyConnection, payload: dict[str, Any]) -> dict[str, Any]:
    predicate = f"hash(f.cik)%{int(payload['buckets'])}={int(payload['bucket'])} AND NOT ({_replaced('f')})"
    selection = f"SELECT f.* FROM {FACTS} f WHERE {predicate}"
    _duplicates(conn, selection)
    count = int(conn.execute(f"SELECT count(*) FROM ({selection})").fetchone()[0])
    if count != payload["rows"] or count + 1 > MAX_WRITES:
        raise ValueError("retained facts differ from plan or exceed aggregate transaction bound")
    conn.execute(f"INSERT INTO {FACTS}__next {selection}")
    output = f"SELECT * FROM {FACTS}__next WHERE hash(cik)%{int(payload['buckets'])}={int(payload['bucket'])}"
    return {"facts": count, "points": 0, "candidates": 0, "writes": count + 1,
            "fingerprints": {FACTS + "__next": _multiset(conn, output)}}


def _retain_points(conn: duckdb.DuckDBPyConnection, payload: dict[str, Any]) -> dict[str, Any]:
    bucket, buckets = int(payload["bucket"]), int(payload["buckets"])
    facts = int(conn.execute(f"SELECT count(*) FROM {FACTS} WHERE source=? AND hash(security_id)%{buckets}={bucket}",
                             [SOURCE]).fetchone()[0])
    points = int(conn.execute(f"SELECT count(*) FROM {POINTS} WHERE hash(security_id)%{buckets}={bucket}").fetchone()[0])
    if 3 * facts + points + 1 > min(MAX_WRITES, int(payload["max_writes"])):
        raise ValueError("point ownership input grew beyond the frozen aggregate transaction bound")
    conn.execute(f"CREATE TEMP VIEW _cf_old AS SELECT f.*, ({_replaced('f')}) AS replaced "
                 f"FROM {FACTS} f WHERE source={_literal(SOURCE)} AND hash(security_id)%{buckets}={bucket}")
    conn.execute(f"CREATE TEMP TABLE _cf_keys AS SELECT security_id, sha256(to_json({_common(True, with_run=True)})) digest, "
                 "bool_and(replaced) AS replaced, bool_or(replaced) AS any_replaced, count(*) AS n "
                 "FROM _cf_old GROUP BY 1,2")
    if conn.execute("SELECT 1 FROM _cf_keys WHERE replaced<>any_replaced LIMIT 1").fetchone():
        raise ValueError("SEC point ownership overlaps retained and replaced CIKs; refusing ambiguous deletion")
    conn.execute(f"CREATE TEMP VIEW _cf_old_points AS SELECT * FROM {POINTS} "
                 f"WHERE hash(security_id)%{buckets}={bucket}")
    weighted = _digest_sums().replace(" AS HUGEINT))", " AS HUGEINT) * n)")
    aliases = "security_id,n," + ",".join(f"s{i}" for i in range(4))
    # Shared-security fingerprints include run_id, multiplicity and every common
    # field. A receipt cannot hide a missing point behind another CIK's rows.
    mismatches = conn.execute(f"""WITH expected({aliases}) AS (
        SELECT security_id, sum(n) n, {weighted} FROM _cf_keys GROUP BY 1
    ), actual({aliases}) AS (
        SELECT security_id, count(*) n, {_digest_sums()} FROM (
            SELECT *, sha256(to_json({_common(False, with_run=True)})) digest
            FROM _cf_old_points WHERE source={_literal(SOURCE)}) GROUP BY 1
    ), e AS (SELECT * FROM expected), a AS (SELECT * FROM actual)
    SELECT e.security_id FROM e LEFT JOIN a USING (security_id)
    WHERE EXISTS (SELECT 1 FROM _cf_old f JOIN {_VERIFIED} v USING(cik) WHERE f.security_id=e.security_id)
    AND to_json(e) IS DISTINCT FROM to_json(a) LIMIT 12""").fetchall()
    # Compare explicitly named columns, rather than relying on aggregate labels.
    # The SQL below also checks the archive's NULL-symbol contract.
    if mismatches:
        raise ValueError(f"C87 old fact/point multiset mismatch at securities: {mismatches}")
    if conn.execute(f"SELECT 1 FROM _cf_old_points p WHERE p.source=? AND p.symbol IS NOT NULL "
                    f"AND EXISTS (SELECT 1 FROM {_VERIFIED} v WHERE p.run_id=v.run_id) LIMIT 1", [SOURCE]).fetchone():
        raise ValueError("C87 archive points contain a current symbol")
    retained = (f"SELECT p.* FROM _cf_old_points p WHERE p.source IS DISTINCT FROM {_literal(SOURCE)} "
                f"OR NOT EXISTS (SELECT 1 FROM _cf_keys k WHERE k.replaced AND k.security_id=p.security_id "
                f"AND k.digest=sha256(to_json({_common(False, with_run=True, alias='p')})))")
    count = int(conn.execute(f"SELECT count(*) FROM ({retained})").fetchone()[0])
    keys = int(conn.execute("SELECT count(*) FROM _cf_keys").fetchone()[0])
    if keys + count + 1 > MAX_WRITES or int(payload["max_writes"]) > MAX_WRITES:
        raise ValueError("retained point transaction exceeds aggregate row bound")
    conn.execute(f"INSERT INTO {POINTS}__next {retained}")
    conn.execute("DROP TABLE _cf_keys")
    conn.execute("DROP VIEW _cf_old_points")
    conn.execute("DROP VIEW _cf_old")
    output = f"SELECT * FROM {POINTS}__next WHERE hash(security_id)%{buckets}={bucket}"
    return {"facts": 0, "points": count, "candidates": 0, "writes": keys + count + 1,
            "fingerprints": {POINTS + "__next": _multiset(conn, output)}}


_RESOLVE_SQL = """CREATE TEMP TABLE _cf_resolved AS
WITH clocks AS (SELECT cik, available_at, count(*) fact_rows FROM _cf_b GROUP BY 1,2),
security AS (
    SELECT c.cik,c.available_at,h.security_id,h.available_at mapping_clock
    FROM clocks c JOIN security_identifier_history h
      ON h.id_type='CIK' AND h.id_value=c.cik
     AND h.valid_from<=CAST(c.available_at AS DATE)
     AND coalesce(h.valid_to,DATE '9999-12-31')>CAST(c.available_at AS DATE)
     AND (h.available_at IS NULL OR h.available_at<=c.available_at)
    QUALIFY row_number() OVER(PARTITION BY c.cik,c.available_at
      ORDER BY h.available_at DESC NULLS LAST,h.valid_from DESC,h.source_loaded_at DESC NULLS LAST,h.security_id)=1
), entity AS (
    SELECT s.cik,s.available_at,h.id_value entity_id,h.available_at entity_clock
    FROM security s JOIN security_identifier_history h ON h.security_id=s.security_id AND h.id_type='ENTITY_ID'
     AND h.valid_from<=CAST(s.available_at AS DATE)
     AND coalesce(h.valid_to,DATE '9999-12-31')>CAST(s.available_at AS DATE)
     AND (h.available_at IS NULL OR h.available_at<=s.available_at)
    QUALIFY row_number() OVER(PARTITION BY s.cik,s.available_at
      ORDER BY h.available_at DESC NULLS LAST,h.valid_from DESC,h.source_loaded_at DESC NULLS LAST,h.id_value)=1
)
SELECT c.*,s.security_id,e.entity_id,s.mapping_clock,e.entity_clock FROM clocks c
LEFT JOIN security s USING(cik,available_at) LEFT JOIN entity e USING(cik,available_at)
"""


def _archive(conn: duckdb.DuckDBPyConnection, payload: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    path = Path(state["root"]) / str(payload["file"])
    if _sha(path) != payload["sha256"]:
        raise ValueError(f"CF-R input digest changed: {path.name}")
    conn.execute(f"CREATE TEMP VIEW _cf_b AS SELECT * FROM read_parquet({_literal(path.as_posix())}) "
                 f"WHERE cik BETWEEN {_literal(payload['lo'])} AND {_literal(payload['hi'])}")
    count = int(conn.execute("SELECT count(*) FROM _cf_b").fetchone()[0])
    if count != payload["rows"] or count > MAX_INPUT_ROWS:
        raise ValueError("CF-R whole-CIK batch input differs from plan")
    conn.execute(_RESOLVE_SQL)
    columns = [(str(row[0]), str(row[1])) for row in conn.execute(f"DESCRIBE {FACTS}").fetchall()]
    replacements = {"security_id": "coalesce(r.security_id,b.security_id)", "entity_id": "r.entity_id",
                    "is_latest_revision": "NULL::BOOLEAN", "as_of_date": "NULL::DATE",
                    "run_id": _literal(state["run_id"]), "source_loaded_at": _literal(state["loaded_at"]) + "::TIMESTAMP"}
    projection = ", ".join(f"CAST({replacements.get(c, 'b.' + c)} AS {kind}) AS {c}" for c, kind in columns)
    conn.execute(f"CREATE TEMP VIEW _cf_new AS SELECT {projection} FROM _cf_b b JOIN _cf_resolved r USING(cik,available_at)")
    if conn.execute("SELECT count(*) FROM _cf_new").fetchone()[0] != count:
        raise ValueError("CF-R identity join lost or multiplied source rows")
    # The retained surface is already built; include any non-SEC collision rather
    # than dropping an existing fact to satisfy uniqueness.
    _duplicates(conn, f"SELECT * FROM _cf_new UNION ALL SELECT * FROM {FACTS}__next "
                     f"WHERE cik BETWEEN {_literal(payload['lo'])} AND {_literal(payload['hi'])}")
    concepts = ",".join(_literal(c) for c in state["proof"]["concepts"])
    aliases = "cik,n," + ",".join(f"s{i}" for i in range(4))
    conn.execute(f"""CREATE TEMP TABLE _cf_comparison AS WITH old({aliases}) AS (
        SELECT cik,count(*),{_digest_sums()} FROM (
            SELECT f.*,sha256(to_json({_common(True)})) digest FROM {FACTS} f
            WHERE try_cast(cik AS BIGINT) BETWEEN {int(payload['lo'])} AND {int(payload['hi'])}
        ) WHERE cik IN (SELECT cik FROM {_VERIFIED}) GROUP BY cik
    ), fresh({aliases}) AS (
        SELECT cik,count(*),{_digest_sums()} FROM (
            SELECT *,sha256(to_json({_common(True)})) digest FROM _cf_new
            WHERE concept IN ({concepts}) AND cik IN (SELECT cik FROM {_VERIFIED})
        ) GROUP BY cik
    ) SELECT v.cik,v.rows, to_json(o) old_digest,to_json(fresh_row) new_digest
      FROM {_VERIFIED} v LEFT JOIN old o USING(cik) LEFT JOIN fresh fresh_row USING(cik)
      WHERE v.cik BETWEEN {_literal(payload['lo'])} AND {_literal(payload['hi'])}""")
    mismatch = conn.execute("SELECT * FROM _cf_comparison WHERE old_digest IS DISTINCT FROM new_digest "
                            "OR try_cast(json_extract_string(old_digest,'$.n') AS BIGINT) IS DISTINCT FROM rows LIMIT 20").fetchall()
    if mismatch:
        raise ValueError(f"C87 per-CIK equality failed; publication refused: {mismatch}")
    ownership = conn.execute(f"""SELECT v.cik FROM {_VERIFIED} v LEFT JOIN {FACTS} f
        ON try_cast(f.cik AS BIGINT)=try_cast(v.cik AS BIGINT)
        WHERE v.cik BETWEEN {_literal(payload['lo'])} AND {_literal(payload['hi'])}
        GROUP BY v.cik,v.run_id,v.rows HAVING count(f.cik)<>v.rows OR
          count(*) FILTER(WHERE f.cik=v.cik AND f.run_id=v.run_id AND f.source={_literal(SOURCE)}
              AND f.source_url={_literal(SOURCE_URL)} || '#CIK' || v.cik || '.json')<>v.rows LIMIT 20""").fetchall()
    if ownership:
        raise ValueError(f"C87 canonical CIK/owner/source-url counts differ: {ownership}")
    conn.execute("CREATE TEMP TABLE _cf_unresolved AS SELECT cik, "
                 "arg_min(coalesce(security_id,'SEC-COMPANYFACTS-UNRESOLVED-CIK-'||cik),available_at) security_id, "
                 "min(available_at) available_at, sum(fact_rows) FILTER(WHERE security_id IS NULL) security_missing, "
                 "sum(fact_rows) FILTER(WHERE entity_id IS NULL) entity_missing FROM _cf_resolved "
                 "WHERE security_id IS NULL OR entity_id IS NULL GROUP BY cik")
    # UUID creation is a small per-CIK metadata operation, never a fact-row loop.
    candidate_rows = conn.execute("SELECT cik,security_id FROM _cf_unresolved").fetchall()
    conn.execute("CREATE TEMP TABLE _cf_candidate_ids(cik VARCHAR,candidate_id VARCHAR)")
    for cik, security in candidate_rows:
        candidate = str(uuid.uuid5(uuid.NAMESPACE_URL, f"sec_company_facts||CIK|{cik}|{security}|{MATCH_METHOD}"))
        conn.execute("INSERT INTO _cf_candidate_ids VALUES (?,?)", [cik, candidate])
    resolved_count = int(conn.execute("SELECT count(*) FROM _cf_resolved").fetchone()[0])
    checked_count = int(conn.execute("SELECT count(*) FROM _cf_comparison").fetchone()[0])
    candidate_count = len(candidate_rows)
    writes = 2 * count + resolved_count + 2 * checked_count + 3 * candidate_count + 1
    if writes > MAX_WRITES:
        raise ValueError(f"CF-R transaction would write {writes:,} aggregate rows")
    conn.execute(f"INSERT INTO {FACTS}__next SELECT * FROM _cf_new")
    point_columns = [(str(row[0]), str(row[1])) for row in conn.execute(f"DESCRIBE {POINTS}").fetchall()]
    point_projection = {"metric": "concept", "as_of_date": "filed_date", "symbol": "NULL::VARCHAR",
                        "is_latest_revision": "NULL::BOOLEAN", "item_id": "NULL::INTEGER"}
    conn.execute("CREATE TEMP VIEW _cf_new_points AS SELECT "
                 + ",".join(f"CAST({point_projection.get(c, c)} AS {kind}) AS {c}" for c, kind in point_columns)
                 + " FROM _cf_new")
    conn.execute(f"INSERT INTO {POINTS}__next SELECT * FROM _cf_new_points")
    candidate_columns = ("candidate_id,source_dataset_id,source_table,source_period,source_key_type,source_key_value,"
                         "source_security_id,source_name,source_normalized_name,target_security_id,target_id_type,"
                         "target_id_value,target_name,target_normalized_name,match_method,confidence,candidate_status,"
                         "as_of_date,available_at,details_json,run_id,source_loaded_at")
    conn.execute(f"""INSERT INTO {_CANDIDATES_NEXT} ({candidate_columns})
        SELECT i.candidate_id,'sec_company_facts','sec_company_facts',NULL,
        'CIK',u.cik,u.security_id,NULL,NULL,u.security_id,'CIK',u.cik,NULL,NULL,{_literal(MATCH_METHOD)},0.0,'proposed',
        {_literal(state['as_of_date'])}::DATE,{_literal(state['as_of_date'])}::DATE + INTERVAL 22 HOUR,
        json_object('status_reason','unresolved_cik_or_entity_at_fact_available_at',
          'unresolved_security_fact_rows',coalesce(u.security_missing,0),
          'unresolved_entity_fact_rows',coalesce(u.entity_missing,0),'fact_available_at',u.available_at)::VARCHAR,
        {_literal(state['run_id'])},{_literal(state['loaded_at'])}::TIMESTAMP
        FROM _cf_unresolved u JOIN _cf_candidate_ids i USING(cik)""")
    conn.execute(f"INSERT INTO {_CHECKED} SELECT cik,rows,new_digest FROM _cf_comparison")
    scope = f"BETWEEN {_literal(payload['lo'])} AND {_literal(payload['hi'])}"
    fingerprints = {FACTS + "__next": _multiset(conn, "SELECT * FROM _cf_new"),
                    POINTS + "__next": _multiset(conn, "SELECT * FROM _cf_new_points"),
                    _CANDIDATES_NEXT: _multiset(conn, f"SELECT * FROM {_CANDIDATES_NEXT} "
                                               f"WHERE source_key_value {scope} AND run_id={_literal(state['run_id'])}"),
                    _CHECKED: _multiset(conn, f"SELECT * FROM {_CHECKED} WHERE cik {scope}")}
    null_mapping_clocks = int(conn.execute("SELECT coalesce(sum(fact_rows) FILTER (WHERE security_id IS NOT NULL "
                                          "AND mapping_clock IS NULL),0) FROM _cf_resolved").fetchone()[0])
    for table in ("_cf_resolved", "_cf_comparison", "_cf_unresolved", "_cf_candidate_ids"):
        conn.execute(f"DROP TABLE {table}")
    conn.execute("DROP VIEW _cf_new")
    conn.execute("DROP VIEW _cf_new_points")
    conn.execute("DROP VIEW _cf_b")
    return {"facts": count, "points": count, "candidates": candidate_count, "writes": writes,
            "verified_ciks": checked_count, "security_mappings_with_null_clock_rows": null_mapping_clocks,
            "fingerprints": fingerprints}


def build(conn: duckdb.DuckDBPyConnection, spec: BatchSpec, run_key: str) -> BatchResult:
    payload = dict(spec.payload)
    if hashlib.sha256(_json(payload).encode()).hexdigest() != spec.input_sha256:
        raise ValueError("CF-R frozen batch payload digest differs")
    state = _state(conn, run_key)
    root = Path(state["root"])
    if _sha(root / "manifest.json") != MANIFEST_SHA or _sha(root / "members.parquet") != MEMBERS_SHA:
        raise ValueError("CF-R frozen source metadata changed")
    if _fingerprints(conn, "security_identifier_history", "security_id", 1) != state["identity_pin"]:
        raise ValueError("historical identifier evidence changed during the staged rebuild")
    _member_view(conn, root)
    if payload["kind"] == "retained_facts":
        note = _retain_facts(conn, payload)
    elif payload["kind"] == "retained_points":
        note = _retain_points(conn, payload)
    elif payload["kind"] == "archive":
        note = _archive(conn, payload, state)
    else:
        raise ValueError(f"unknown CF-R batch kind {payload['kind']}")
    conn.execute("DROP VIEW _cf_members")
    return BatchResult(rows_in=int(payload.get("rows", note["points"])), rows_out=note["facts"] + note["points"],
                       note=_json({"kind": payload["kind"], **note}))


def _publish_shadows(conn: duckdb.DuckDBPyConnection, before_swap: Callable[[], None]) -> None:
    """Coupled DROP/RENAME of INSERT-only shadows, preserving raw DEFAULTs.

    The raw SEC tables were not rebuilt by 0329 and still have DEFAULT now().
    Never rename or modify the old live tables. The guarded WAL-replay proof
    covers these exact live contracts and shadows filled in earlier commits.
    """
    store = store_for(conn)
    try:
        with store.transaction():
            for table in (FACTS, POINTS):
                _validate_shadow_contract(store, live_table=table, shadow_table=table + "__next")
            before_swap()
            for table in (FACTS, POINTS):
                conn.execute(f"DROP TABLE {table}")
                conn.execute(f"ALTER TABLE {table}__next RENAME TO {table}")
    except BaseException:
        with suppress(Exception):
            conn.execute("ROLLBACK")
        raise


def finalize(conn: duckdb.DuckDBPyConnection, run_key: str) -> dict[str, object]:
    state = _state(conn, run_key)
    manifest, _members = _inputs(Path(state["root"]))
    for batch in manifest["batches"]:
        if _sha(Path(state["root"]) / batch["file"]) != batch["parquet_sha256"]:
            raise ValueError(f"CF-R retained input changed before publication: {batch['file']}")
    for table, key, buckets, pin in ((FACTS, "cik", state["fact_buckets"], "facts_pin"),
                                    (POINTS, "security_id", state["point_buckets"], "points_pin"),
                                    ("security_identifier_history", "security_id", 1, "identity_pin"),
                                    (CANDIDATES, "candidate_id", 1, "candidate_pin")):
        if _fingerprints(conn, table, key, buckets) != state[pin]:
            raise ValueError(f"live {table} changed after planning; publication refused")
    for table, expected in state["catalog"].items():
        if _catalog_pin(conn, table) != expected:
            raise ValueError(f"live {table} catalog changed during rebuild")
    notes = [json.loads(row[0]) for row in conn.execute("SELECT note FROM build_batches WHERE stage=? AND run_key=?",
                                                     [STAGE_NAME, run_key]).fetchall()]
    _assert_output_fingerprints(conn, notes)
    frozen_verified = ("SELECT r.cik,r.run_id,r.rows FROM UNNEST(from_json(?, "
                       "'[{\"cik\":\"VARCHAR\",\"run_id\":\"VARCHAR\",\"rows\":\"BIGINT\"}]')) t(r)")
    for left, right in ((f"SELECT * FROM {_VERIFIED}", frozen_verified),
                        (frozen_verified, f"SELECT * FROM {_VERIFIED}")):
        if conn.execute(f"SELECT 1 FROM ({left} EXCEPT ALL {right}) LIMIT 1",
                        [_json(state["proof"]["receipts"])]).fetchone():
            raise ValueError("verified CIK metadata differs from frozen lineage receipts")
    for key, table in (("facts", FACTS + "__next"), ("points", POINTS + "__next"), ("candidates", _CANDIDATES_NEXT)):
        expected = sum(n[key] for n in notes)
        if conn.execute(f"SELECT count(*) FROM {table}").fetchone()[0] != expected:
            raise ValueError(f"CF-R {table} count differs from committed batch outputs")
    counts = conn.execute(f"SELECT count(*),count(DISTINCT cik),sum(rows) FROM {_CHECKED}").fetchone()
    if counts != (EXPECTED_CIKS, EXPECTED_CIKS, EXPECTED_OLD_ROWS):
        raise ValueError(f"C87 checked CIK/row totals differ: {counts}")
    if conn.execute(f"SELECT 1 FROM (SELECT cik,rows FROM {_VERIFIED} EXCEPT ALL "
                    f"SELECT cik,rows FROM {_CHECKED}) LIMIT 1").fetchone():
        raise ValueError("C87 checked CIK set differs from the frozen verified receipt set")
    if _receipt_inventory(conn, manifest) != state["proof"]:
        raise ValueError("C87 source receipt or lineage authority changed before publication")
    # Complete whole-CIK batches checked uniqueness and exact per-member rows;
    # this validates loaded coverage and all source dispositions after resume.
    _member_view(conn, Path(state["root"]))
    coverage = conn.execute(f"""SELECT m.cik,m.disposition,m.rows,count(f.cik) FROM _cf_members m
        LEFT JOIN {FACTS}__next f ON f.cik=m.cik AND f.run_id=?
        GROUP BY m.cik,m.disposition,m.rows HAVING count(f.cik)<>m.rows LIMIT 20""", [state["run_id"]]).fetchall()
    if coverage:
        raise ValueError(f"CF-R member disposition/row coverage differs: {coverage}")
    conn.execute("DROP VIEW _cf_members")
    receipt: dict[str, object] = {"mode": "staged_rebuild", "archive_sha256": ARCHIVE_SHA,
        "manifest_sha256": MANIFEST_SHA, "members_sha256": MEMBERS_SHA, "totals": manifest["totals"],
        "run_id": state["run_id"], "c87_verified_ciks": EXPECTED_CIKS, "c87_verified_rows": EXPECTED_OLD_ROWS,
        "c87_allowlist_sha256": OLD_ALLOWLIST_SHA, "published_allowlist_sha256": ALLOWLIST_SHA,
        "retained_facts": sum(n["facts"] for n in notes if n["kind"] == "retained_facts"),
        "retained_points": sum(n["points"] for n in notes if n["kind"] == "retained_points"),
        "max_transaction_rows": max(n["writes"] for n in notes),
        "staged_output_drift_check": "all-column count/SUM/XOR of DuckDB hashes (noncryptographic diagnostic)",
        "security_mappings_with_null_clock_rows": sum(n.get("security_mappings_with_null_clock_rows", 0) for n in notes),
        "identity_rule": "historical_asof_including_legacy_null_clocks_no_current_fallback",
        "nullable_metadata_policy": "facts.as_of_date, points.item_id, latest-revision flags remain NULL (unknown)",
        "exact_literal_evidence": "value_exact retained in hash-pinned Parquet; warehouse value remains DOUBLE"}

    def before_swap() -> None:
        # This table retains DEFAULT now() and ART indexes. INSERT-only copy /
        # drop / rename keeps its contract, avoiding unsafe DELETE WAL replay.
        columns = table_columns(conn, CANDIDATES)
        # Reserve catalog work in addition to explicit receipt/ledger row writes.
        metadata_writes = len(manifest["batches"]) + 2 + 256
        _member_view(conn, Path(state["root"]))
        selected = _candidate_selection()
        _validate_candidate_selection(conn, selected, metadata_writes)
        replace_rows_by_swap(conn, CANDIDATES, columns, selected, max_rows=MAX_WRITES - metadata_writes)
        conn.execute("DROP VIEW _cf_members")
        stamp = utc_now()
        conn.execute("INSERT INTO dataset_runs(run_id,dataset_id,status,started_at,finished_at,rows_loaded,source,params_json) "
                     "VALUES (?,'sec_company_facts','succeeded',?,?,?,?,?)",
                     [state["run_id"], dt.datetime.fromisoformat(state["loaded_at"]), stamp,
                      manifest["totals"]["rows"], SOURCE, _json(receipt)])
        for batch in manifest["batches"]:
            path = Path(state["root"]) / batch["file"]
            source_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{state['run_id']}:{batch['file']}"))
            conn.execute("INSERT INTO raw_source_files VALUES (?,'sec_company_facts',?,?,?,?,?,'loaded',?)",
                         [source_id, f"{SOURCE_URL}#staged/{batch['file']}", str(path), batch["parquet_sha256"],
                          batch["parquet_bytes"], stamp, _json({**batch, "archive_sha256": ARCHIVE_SHA,
                                                             "run_id": state["run_id"], "manifest_sha256": MANIFEST_SHA})])
        mark_published(conn, STAGE_NAME, run_key, receipt)

    _publish_shadows(conn, before_swap)
    return receipt


def discard(conn: duckdb.DuckDBPyConnection, run_key: str) -> None:
    for table in (FACTS + "__next", POINTS + "__next", _CANDIDATES_NEXT, _META, _VERIFIED, _CHECKED):
        conn.execute(f"DROP TABLE IF EXISTS {table}")


register_stage(BatchStage(STAGE_NAME, plan, prepare, build, finalize, discard=discard,
                         code_dependencies=("_companyfacts_resume.py", "_bulk_publication.py", "_table_swap.py")))
