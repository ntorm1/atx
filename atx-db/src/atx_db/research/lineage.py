"""Set-based selected-lineage qualification for research panels (R2a fix round 1, absorbs R2c).

:func:`atx_db.derived_lineage.qualify_selected_lineage` walks each root in Python.
At research scale (tens of millions of selected states, most with several
leaves) that per-root walk is the dominant cost. This module computes the same
proof rows for whole root sets with DuckDB joins:

* one set-based fetch per dependency level (no per-root queries), with leaves
  fetched once per chunk no matter how many roots share them;
* **exact replication** of the Python outcome, including the first-failure
  status and reason in the resolver's order, for roots whose operands are all
  standardized items (depth 0: every ``q``/``ttm`` rollup and most ratios);
* **certification** of roots with metric dependencies (depth > 0) when every
  node, edge and leaf in the reachable graph passes every check the resolver
  makes, with all its budgets bounded by additive upper bounds;
* every other root, and any root where the SQL checks cannot be sure to agree
  with Python (unusual JSON types, non-canonical clock strings, duplicate keys,
  text that JSON would escape), goes to the Python resolver itself.

So every row is either the Python resolver's own output or provably equal to
it. The digest is the resolver's digest recipe computed in SQL. Proofs remain
formation-independent (each root is proved at its own event clock), which is
what makes one proof per ``derived_value_id`` per run reusable across formations.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any

from ..derived_registry import DERIVED_SOURCE_NAME

MAX_DEPTH = 16
MAX_NODES = 512
MAX_BYTES = 1_048_576
MAX_BATCH_NODES = 4096
MAX_BATCH_BYTES = 8_388_608
IN_LIST_BATCH = 2048
# Measured on this host (DuckDB 1.5, 1 thread): a 2048-id parameter IN list
# costs ~0.29 s (~140 us per id, random ids); a hash semi-join reads every
# projected column of every row: ~2 us per derived row (refs JSON) and ~0.6 us
# per standardized row. Callers should submit roots owner-ordered: an owner's
# states and leaves are then stored together and a chunk's children are mostly
# its own roots, which makes the index path several times cheaper.
IN_LIST_SECONDS_PER_ID = 1.4e-4
SCAN_SECONDS_PER_ROW = {"derived_metric_values": 2e-6, "fundamental_standardized": 6e-7}
_REF_KEYS = ("kind", "state_id", "status", "bucket", "offset", "code", "cik", "basis", "source",
             "period_start", "period_end", "available_at", "inputs_hash", "definition_hash")
METHOD_EXACT = "set_exact"
METHOD_CERTIFIED = "set_certified"
METHOD_FALLBACK = "python_fallback"

# JSON-safe text (json.dumps would not escape any of these characters).
_SAFE_TEXT = r"[A-Za-z0-9_.:/ +=-]*"
_CANONICAL_CLOCK = (r"[0-9]{4}-[0-9]{2}-[0-9]{2}[T ](?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]"
                    r"(?:\.[0-9]{1,6})?")
_INT_TYPES = "('UBIGINT','BIGINT')"
# The resolver's two terminal success reasons; every other outcome is ``_failed``.
_SUCCESS = "'selected leaves verified','selected source ownership verified; value invalid'"


def _eq_str(json_type: str, value: str, stored: str) -> str:
    """Python ``ref.get(k) != stored`` is False (stored is a str column or NULL)."""
    return (f"(CASE WHEN {stored} IS NULL THEN coalesce({json_type},'NULL')='NULL' "
            f"ELSE coalesce({json_type}='VARCHAR' AND {value}={stored}, false) END)")


def _same_date(json_type: str, value: str, stored: str) -> str:
    return (f"(CASE WHEN {stored} IS NULL THEN coalesce({json_type},'NULL')='NULL' "
            f"ELSE coalesce({json_type}='VARCHAR' AND {value}=strftime({stored},'%Y-%m-%d'), false) END)")


def _same_clock(json_type: str, value: str, stored: str) -> str:
    return (f"(CASE WHEN {stored} IS NULL THEN coalesce({json_type},'NULL')='NULL' "
            f"WHEN coalesce({json_type},'NULL')='NULL' THEN false "
            f"ELSE coalesce({json_type}='VARCHAR' AND regexp_full_match({value},'{_CANONICAL_CLOCK}') "
            f"AND TRY_CAST({value} AS TIMESTAMP)={stored}, false) END)")


def _clock_uncertain(json_type: str, value: str) -> str:
    """A string clock the resolver might parse (offsets, other ISO forms) but SQL does not."""
    return (f"coalesce({json_type}='VARCHAR' AND NOT regexp_full_match({value},'{_CANONICAL_CLOCK}'), false)")


def _norm_cik(json_type: str, value: str) -> str:
    """``derived_lineage._normalize_cik`` for JSON/str values (NULL when it returns None)."""
    return (f"(CASE WHEN {json_type} IN ('VARCHAR','UBIGINT','BIGINT') "
            f"AND regexp_full_match({value},'[0-9]+') "
            f"THEN CASE WHEN length({value})<=10 THEN lpad({value},10,'0') ELSE {value} END END)")


def _cik_uncertain(json_type: str, value: str) -> str:
    """Whitespace or non-ASCII: ``str.strip``/``isdecimal`` semantics differ from SQL."""
    return (f"coalesce({json_type}='VARCHAR' AND NOT regexp_full_match({value},'[0-9]+') "
            f"AND (regexp_matches({value},'\\s') OR regexp_matches({value},'[^\\x00-\\x7f]')), false)")


def _clock_text(column: str) -> str:
    """``str(datetime)`` (isoformat with a space; microseconds only when nonzero)."""
    micros = f"((epoch_us({column}) % 1000000) + 1000000) % 1000000"
    return (f"(strftime({column},'%Y-%m-%d %H:%M:%S') || CASE WHEN {micros}<>0 "
            f"THEN '.' || lpad(CAST({micros} AS VARCHAR),6,'0') ELSE '' END)")


def _bucket(column: str) -> str:
    return f"((year({column})*12+month({column})-1+CASE WHEN day({column})>=15 THEN 1 ELSE 0 END)//3)"


def _stage_expected(con: Any, expected_hashes: Mapping[tuple[str, str], str]) -> None:
    con.execute("CREATE OR REPLACE TEMP TABLE _lp_expected "
                "(metric_code VARCHAR, metric_window VARCHAR, expected_hash VARCHAR)")
    rows = [[code, window, value] for (code, window), value in sorted(expected_hashes.items())]
    if rows:
        con.executemany("INSERT INTO _lp_expected VALUES (?,?,?)", rows)


def _fetch_levels(con: Any) -> int:
    """Fetch roots and every metric dependency level (at most MAX_DEPTH+1 levels)."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _lp_node (
          node_id VARCHAR, level INTEGER, metric_code VARCHAR, metric_window VARCHAR,
          target_bucket BIGINT, available_at TIMESTAMP, valid_to TIMESTAMP, source VARCHAR,
          history_status VARCHAR, value_status VARCHAR, value_null BOOLEAN,
          definition_hash VARCHAR, inputs_hash VARCHAR, fiscal_period_start DATE,
          fiscal_period_end DATE, payload VARCHAR, stored_hash VARCHAR,
          payload_bytes BIGINT, row_bytes BIGINT, pj VARCHAR)
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _lp_ref (
          node_id VARCHAR, i BIGINT, rt VARCHAR, keys_unique BOOLEAN,
          kt VARCHAR, kind VARCHAR, st VARCHAR, sid VARCHAR, ust VARCHAR, status VARCHAR,
          bt VARCHAR, bucket_text VARCHAR, ot VARCHAR, offset_text VARCHAR,
          ct VARCHAR, code VARCHAR, cikt VARCHAR, cik VARCHAR, bat VARCHAR, basis VARCHAR,
          sot VARCHAR, source VARCHAR, pst VARCHAR, period_start VARCHAR,
          pet VARCHAR, period_end VARCHAR, att VARCHAR, available_at VARCHAR,
          iht VARCHAR, inputs_hash VARCHAR, dht VARCHAR, definition_hash VARCHAR)
    """)
    con.execute("CREATE OR REPLACE TEMP TABLE _lp_frontier AS SELECT DISTINCT root_id AS id FROM _lp_roots")
    paths = "[" + ",".join(f"'$.{key}'" for key in _REF_KEYS) + "]"
    typed = ", ".join(f"t[{n}], s[{n}]" for n in range(1, len(_REF_KEYS) + 1))
    levels = 0
    for level in range(MAX_DEPTH + 1):
        if not con.execute("SELECT count(*) FROM _lp_frontier").fetchone()[0]:
            break
        levels = level + 1
        _insert_by_ids(con, "derived_metric_values", "_lp_frontier", """
            INSERT INTO _lp_node
            SELECT d.derived_value_id, ?, d.metric_code, d.metric_window, d.target_bucket,
                   d.available_at, d.valid_to, d.source, d.history_status, d.value_status,
                   d.value IS NULL, d.definition_hash, d.inputs_hash, d.fiscal_period_start,
                   d.fiscal_period_end, d.selected_input_refs_json, d.selected_input_refs_hash,
                   coalesce(octet_length(encode(d.selected_input_refs_json)),0),
                   coalesce(octet_length(encode(d.selected_input_refs_json)),0)
                   + coalesce(octet_length(encode(d.inputs_hash)),0)
                   + coalesce(octet_length(encode(d.definition_hash)),0),
                   -- JSON functions raise on malformed text: they only ever see pj
                   CASE WHEN json_valid(d.selected_input_refs_json) THEN d.selected_input_refs_json END
            FROM derived_metric_values d
            WHERE d.derived_value_id IN ({ids})
        """, [level])
        # One parse per ref: every key's JSON type and text in two list calls.
        con.execute(f"""
            INSERT INTO _lp_ref
            SELECT node_id, i, rt, keys_unique, {typed}
            FROM (
              SELECT node_id, i, json_type(r) AS rt,
                     -- json_keys of a non-object is []; a CASE around it makes
                     -- DuckDB 1.5 hold every JSON arena of the statement (OOM)
                     len(json_keys(r))=len(list_distinct(json_keys(r))) AS keys_unique,
                     json_type(r, {paths}) AS t, json_extract_string(r, {paths}) AS s
              FROM (
                SELECT node_id, unnest(refs) AS r, unnest(range(len(refs))) AS i
                FROM (SELECT node_id, CAST(json_extract(pj,'$.refs') AS JSON[]) AS refs
                      FROM _lp_node
                      WHERE level=? AND json_type(pj)='OBJECT' AND json_type(pj,'$.refs')='ARRAY')
              )
            )
        """, [level])
        # The resolver expands metric refs only from nodes whose payload fits.
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _lp_frontier AS
            SELECT DISTINCT r.sid AS id FROM _lp_ref r
            JOIN _lp_node n ON n.node_id=r.node_id AND n.level=?
            WHERE n.payload_bytes<={MAX_BYTES} AND r.rt='OBJECT' AND r.kt='VARCHAR'
              AND r.kind='metric' AND r.sid IS NOT NULL
              AND r.sid NOT IN (SELECT node_id FROM _lp_node)
        """, [level])
    return levels


def _table_rows(con: Any, table: str) -> int:
    row = con.execute("SELECT max(estimated_size) FROM duckdb_tables() WHERE table_name=? AND NOT temporary",
                      [table]).fetchone()
    return int(row[0] or 0) if row else 0


def _insert_by_ids(con: Any, table: str, ids_table: str, statement: str, leading: list[Any] | None = None) -> None:
    """Run ``statement`` with ``{ids}`` bound to the ids of ``ids_table`` (column ``id``).

    Two exact strategies, chosen by estimated cost: parameter IN lists of at
    most ``IN_LIST_BATCH`` ids (primary-key index lookups, cost per id), or one
    hash semi-join (a scan of ``table``, cost per table row). Large chunks over
    a large warehouse take the scan; small ones the index.
    """
    count = int(con.execute(f"SELECT count(*) FROM {ids_table}").fetchone()[0])
    if not count:
        return
    if count * IN_LIST_SECONDS_PER_ID <= _table_rows(con, table) * SCAN_SECONDS_PER_ROW.get(table, 1e-6):
        ids = [row[0] for row in con.execute(f"SELECT id FROM {ids_table} ORDER BY id").fetchall()]
        for start in range(0, len(ids), IN_LIST_BATCH):
            batch = ids[start:start + IN_LIST_BATCH]
            con.execute(statement.replace("{ids}", ",".join("?" * len(batch))), [*(leading or []), *batch])
    else:
        con.execute(statement.replace("{ids}", f"SELECT id FROM {ids_table}"), leading or [])


def _stage_checks(con: Any) -> None:
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _lp_leaf (
          leaf_id VARCHAR, canonical_code VARCHAR, cik VARCHAR, basis VARCHAR, source VARCHAR,
          period_start DATE, period_end DATE, available_at TIMESTAMP, value_null BOOLEAN,
          leaf_bytes BIGINT)
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _lp_leaf_ids AS
        SELECT DISTINCT sid AS id FROM _lp_ref
        WHERE rt='OBJECT' AND kt='VARCHAR' AND kind='item' AND sid IS NOT NULL
    """)
    _insert_by_ids(con, "fundamental_standardized", "_lp_leaf_ids", """
        INSERT INTO _lp_leaf
        SELECT s.standardized_id, s.canonical_code, s.cik, s.basis, s.source,
               s.period_start, s.period_end, s.available_at, s.value IS NULL,
               coalesce(octet_length(encode(s.standardized_id)),0)
               + coalesce(octet_length(encode(s.canonical_code)),0)
               + coalesce(octet_length(encode(s.cik)),0)
               + coalesce(octet_length(encode(s.basis)),0)
               + coalesce(octet_length(encode(s.source)),0)
        FROM fundamental_standardized s
        WHERE s.standardized_id IN ({ids})
    """)
    leaf_cik = _norm_cik("CASE WHEN l.cik IS NULL THEN NULL ELSE 'VARCHAR' END", "l.cik")
    leaf_match = " AND ".join((
        _eq_str("r.ct", "r.code", "l.canonical_code"),
        f"l.period_end IS NOT NULL AND r.bucket_i={_bucket('l.period_end')}",
        f"{_norm_cik('r.cikt', 'r.cik')} IS NOT DISTINCT FROM {leaf_cik}",
        _eq_str("r.bat", "r.basis", "l.basis"),
        _eq_str("r.sot", "r.source", "l.source"),
        _same_date("r.pst", "r.period_start", "l.period_start"),
        _same_date("r.pet", "r.period_end", "l.period_end"),
        _same_clock("r.att", "r.available_at", "l.available_at"),
    ))
    child_match = " AND ".join((
        _eq_str("r.ct", "r.code", "c.metric_code"),
        "c.target_bucket IS NOT NULL AND r.bucket_i=c.target_bucket",
        "c.source IS NOT DISTINCT FROM n.source",
        _eq_str("r.sot", "r.source", "c.source"),
        _same_date("r.pst", "r.period_start", "c.fiscal_period_start"),
        _same_date("r.pet", "r.period_end", "c.fiscal_period_end"),
        _eq_str("r.iht", "r.inputs_hash", "c.inputs_hash"),
        _eq_str("r.dht", "r.definition_hash", "c.definition_hash"),
        _same_clock("r.att", "r.available_at", "c.available_at"),
    ))
    # First failing check per ref, in the resolver's order (0 = passes).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _lp_refchk AS
        WITH typed AS (
          SELECT r.*, TRY_CAST(r.bucket_text AS BIGINT) AS bucket_i,
                 TRY_CAST(r.offset_text AS BIGINT) AS offset_i
          FROM _lp_ref r
        )
        SELECT r.node_id, r.i, r.kind, r.sid,
               CASE WHEN NOT coalesce(r.rt='OBJECT' AND r.kt='VARCHAR' AND r.kind IN ('item','metric'), false) THEN 1
                    WHEN NOT coalesce(r.ust='VARCHAR' AND r.status IN ('selected','missing') AND r.st='VARCHAR', false) THEN 2
                    WHEN NOT coalesce(r.bt IN {_INT_TYPES} AND r.ot IN {_INT_TYPES}
                                      AND r.bucket_i IS NOT NULL AND r.offset_i IS NOT NULL
                                      AND r.offset_i>=0, false) THEN 3
                    WHEN n.target_bucket IS NULL OR r.bucket_i<>n.target_bucket-r.offset_i THEN 4
                    WHEN r.kind='item' AND l.leaf_id IS NULL THEN 5
                    WHEN r.kind='item' AND NOT coalesce({leaf_match}, false) THEN 6
                    WHEN r.kind='item' AND (l.available_at IS NULL OR l.available_at>n.available_at) THEN 7
                    WHEN r.kind='item' AND {leaf_cik} IS NULL THEN 8
                    WHEN r.kind='metric' AND c.node_id IS NULL THEN 5
                    WHEN r.kind='metric' AND NOT coalesce({child_match}, false) THEN 6
                    ELSE 0 END AS fail_rank,
               coalesce(NOT r.keys_unique, false)
               OR r.bt='BOOLEAN' OR r.ot='BOOLEAN'
               OR (r.bt IN {_INT_TYPES} AND r.bucket_i IS NULL)
               OR (r.ot IN {_INT_TYPES} AND r.offset_i IS NULL)
               OR {_clock_uncertain('r.att', 'r.available_at')}
               OR {_cik_uncertain('r.cikt', 'r.cik')}
               OR (r.kind='item' AND {_cik_uncertain("CASE WHEN l.cik IS NULL THEN NULL ELSE 'VARCHAR' END", 'l.cik')})
               OR (r.st IS NOT NULL AND r.st NOT IN ('VARCHAR','NULL'))
               OR NOT coalesce(regexp_full_match(r.sid,'{_SAFE_TEXT}'), true) AS uncertain,
               coalesce(r.status='missing', false) AS missing_status,
               CASE WHEN r.kind='item' THEN l.value_null END AS leaf_value_null,
               CASE WHEN r.kind='item' THEN {leaf_cik} END AS leaf_cik10,
               -- the resolver's visit() checks on the child at the parent's clock
               CASE WHEN r.kind='metric' THEN coalesce(c.available_at IS NOT NULL
                    AND c.available_at<=n.available_at
                    AND (c.valid_to IS NULL OR c.valid_to>n.available_at), false) END AS edge_visit_ok
        FROM typed r JOIN _lp_node n ON n.node_id=r.node_id
        -- Pure equi-joins (a kind test in ON turns them into nested loops);
        -- every use of l / c above is guarded by the ref kind.
        LEFT JOIN _lp_leaf l ON l.leaf_id=r.sid
        LEFT JOIN _lp_node c ON c.node_id=r.sid
    """)
    # JSON facts first, outside any CASE (a JSON function under CASE makes
    # DuckDB 1.5 hold every JSON arena of the statement); the ranks use them.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _lp_nodechk AS
        WITH facts AS (
          SELECT n.*, json_type(n.pj) AS pj_type,
                 json_type(n.pj, ['$.version', '$.refs']) AS jt,
                 json_extract_string(n.pj, '$.version') AS version_text,
                 json_array_length(n.pj, '$.refs') AS ref_count,
                 len(json_keys(n.pj)) AS key_count, len(list_distinct(json_keys(n.pj))) AS distinct_keys
          FROM _lp_node n
        )
        SELECT n.node_id,
               CASE WHEN n.payload IS NULL OR n.stored_hash IS NULL THEN 10
                    WHEN n.history_status IS DISTINCT FROM 'event_reconstructed' THEN 11
                    WHEN n.source IS DISTINCT FROM '{DERIVED_SOURCE_NAME}' THEN 12
                    WHEN e.expected_hash IS NULL OR e.expected_hash IS DISTINCT FROM n.definition_hash THEN 13
                    WHEN n.payload_bytes>{MAX_BYTES} THEN 14
                    WHEN sha256(n.payload)<>n.stored_hash THEN 15
                    WHEN n.pj IS NULL THEN 16
                    WHEN n.pj_type<>'OBJECT'
                         OR NOT coalesce(n.jt[1] IN {_INT_TYPES} AND n.version_text='1', false)
                         OR n.jt[2] IS DISTINCT FROM 'ARRAY' THEN 17
                    WHEN n.ref_count>{MAX_NODES} THEN 18
                    ELSE 0 END AS fail_rank,
               (n.payload IS NOT NULL AND n.pj IS NULL)
               OR coalesce(n.jt[1] IN ('BOOLEAN','DOUBLE'), false)
               OR coalesce(n.pj_type='OBJECT' AND n.key_count<>n.distinct_keys, false)
               OR coalesce(n.jt[2]='ARRAY' AND n.ref_count>{MAX_NODES}, false)
               OR NOT coalesce(regexp_full_match(n.node_id,'{_SAFE_TEXT}')
                               AND regexp_full_match(coalesce(n.stored_hash,''),'{_SAFE_TEXT}')
                               AND regexp_full_match(n.metric_code,'{_SAFE_TEXT}')
                               AND regexp_full_match(n.metric_window,'{_SAFE_TEXT}'), false)
               AS uncertain,
               (n.value_status IS DISTINCT FROM 'valid' OR n.value_null) AS value_flag
        FROM facts n
        LEFT JOIN _lp_expected e ON e.metric_code=n.metric_code AND e.metric_window=n.metric_window
    """)


_NODE_REASONS = {
    10: ("legacy_unverifiable", "legacy metric {node}"),
    11: ("invalid", "nonreconstructed metric {node}"),
    12: ("invalid", "unsupported metric source {node}"),
    13: ("definition_unverified", "definition hash mismatch: {key}"),
    14: ("limit_exceeded", "payload byte bound"),
    15: ("invalid", "selected-ref hash mismatch: {node}"),
    16: ("invalid", "invalid refs JSON: {node}"),
    17: ("invalid", "invalid refs version: {node}"),
    18: ("limit_exceeded", "direct ref count bound"),
}
_REF_REASONS = {
    1: ("invalid", "invalid ref kind: {node}"),
    2: ("missing", "missing selected operand identity: {node}"),
    3: ("invalid", "invalid frame offset: {node}"),
    4: ("invalid", "ref bucket differs from consuming frame: {node}"),
    5: ("missing", "missing standardized ref {sid}"),
    6: ("invalid", "mismatched standardized ref {sid}"),
    7: ("invalid", "invisible standardized ref {sid}"),
    8: ("missing", "missing standardized CIK {sid}"),
}


def _reason_sql(table: dict[int, tuple[str, str]], rank: str, node: str, sid: str, key: str) -> tuple[str, str]:
    status = "CASE " + " ".join(f"WHEN {rank}={code} THEN '{value[0]}'" for code, value in table.items()) + " END"
    parts = []
    for code, (_, template) in table.items():
        text = "'" + template.replace("'", "''") + "'"
        text = (text.replace("{node}", f"' || {node} || '").replace("{sid}", f"' || {sid} || '")
                .replace("{key}", f"' || {key} || '"))
        parts.append(f"WHEN {rank}={code} THEN {text}")
    return status, "CASE " + " ".join(parts) + " END"


def _stage_results(con: Any) -> None:
    node_status, node_reason = _reason_sql(
        _NODE_REASONS, "nc.fail_rank", "n.node_id", "NULL",
        "'(''' || n.metric_code || ''', ''' || n.metric_window || ''')'")
    ref_status, ref_reason = _reason_sql(_REF_REASONS, "f.fail_rank", "f.node_id", "f.sid", "NULL")
    digest_leaf = ("'[\"' || leaf_id || '\",\"' || cik10 || '\",\"' || " + _clock_text("leaf_at")
                   + " || '\",\"' || strftime(leaf_end,'%Y-%m-%d') || '\"]'")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _lp_result AS
        WITH roots AS (
          SELECT r.root_id, n.node_id IS NOT NULL AS found,
                 exists(SELECT 1 FROM _lp_ref f WHERE f.node_id=r.root_id AND f.rt='OBJECT'
                        AND f.kt='VARCHAR' AND f.kind='metric') AS deep
          FROM (SELECT DISTINCT root_id FROM _lp_roots) r
          LEFT JOIN _lp_node n ON n.node_id=r.root_id
        ),
        -- Depth 0: replicate the resolver exactly, first failure wins.
        flat AS (
          SELECT r.root_id, n.node_id, n.available_at, n.valid_to, n.row_bytes, n.payload_bytes,
                 nc.fail_rank AS node_rank, nc.uncertain AS node_uncertain, nc.value_flag,
                 {node_status} AS node_status, {node_reason} AS node_reason
          FROM roots r JOIN _lp_node n ON n.node_id=r.root_id
          JOIN _lp_nodechk nc ON nc.node_id=n.node_id
          WHERE r.found AND NOT r.deep
        ),
        flat_refs AS (
          SELECT f.node_id,
                 arg_min(struct_pack(rank := f.fail_rank, status := {ref_status}, reason := {ref_reason}),
                         f.i) FILTER (WHERE f.fail_rank>0) AS first_failure,
                 bool_or(f.uncertain) AS uncertain,
                 bool_or(f.missing_status) AS missing_status,
                 bool_or(coalesce(f.leaf_value_null, false)) AS leaf_value_null,
                 count(DISTINCT f.sid) FILTER (WHERE f.kind='item' AND f.sid IS NOT NULL) AS leaf_ids,
                 count(DISTINCT f.leaf_cik10) FILTER (WHERE f.kind='item') AS cik_count,
                 min(f.leaf_cik10) FILTER (WHERE f.kind='item') AS cik10,
                 count(*) FILTER (WHERE f.kind='item') AS item_refs
          FROM _lp_refchk f JOIN flat ON flat.node_id=f.node_id
          GROUP BY f.node_id
        ),
        flat_leaves AS (
          SELECT flat.node_id, sum(l.leaf_bytes) AS leaf_bytes, max(l.leaf_bytes) AS max_leaf_bytes
          FROM flat JOIN (SELECT DISTINCT node_id, sid FROM _lp_ref
                          WHERE rt='OBJECT' AND kt='VARCHAR' AND kind='item' AND sid IS NOT NULL) f
            ON f.node_id=flat.node_id
          JOIN _lp_leaf l ON l.leaf_id=f.sid
          GROUP BY flat.node_id
        ),
        flat_outcome AS (
          SELECT flat.root_id,
                 flat.node_uncertain OR coalesce(fr.uncertain, false)
                   OR NOT regexp_full_match(flat.root_id,'{_SAFE_TEXT}') AS uncertain,
                 CASE WHEN flat.row_bytes>{MAX_BYTES} THEN 'limit_exceeded'
                      WHEN coalesce(fr.leaf_ids,0)+1>{MAX_BATCH_NODES} THEN 'limit_exceeded'
                      WHEN coalesce(fl.max_leaf_bytes,0)>{MAX_BYTES}
                           OR flat.row_bytes+coalesce(fl.leaf_bytes,0)>{MAX_BATCH_BYTES} THEN 'limit_exceeded'
                      WHEN flat.available_at IS NULL THEN 'invalid'
                      WHEN flat.valid_to IS NOT NULL AND flat.valid_to<=flat.available_at THEN 'invalid'
                      WHEN flat.node_rank>0 THEN flat.node_status
                      WHEN fr.first_failure IS NOT NULL THEN fr.first_failure.status
                      WHEN coalesce(fr.item_refs,0)=0 THEN 'missing'
                      WHEN fr.cik_count<>1 THEN 'mismatch'
                      WHEN flat.value_flag OR fr.missing_status OR fr.leaf_value_null THEN 'invalid'
                      ELSE 'qualified' END AS status,
                 CASE WHEN flat.row_bytes>{MAX_BYTES} THEN 'selected-lineage batch byte bound'
                      WHEN coalesce(fr.leaf_ids,0)+1>{MAX_BATCH_NODES} THEN 'leaf node bound'
                      WHEN coalesce(fl.max_leaf_bytes,0)>{MAX_BYTES}
                           OR flat.row_bytes+coalesce(fl.leaf_bytes,0)>{MAX_BATCH_BYTES}
                           THEN 'selected-lineage batch byte bound'
                      WHEN flat.available_at IS NULL THEN 'root after decision cutoff: ' || flat.root_id
                      WHEN flat.valid_to IS NOT NULL AND flat.valid_to<=flat.available_at
                           THEN 'root expired by decision cutoff: ' || flat.root_id
                      WHEN flat.node_rank>0 THEN flat.node_reason
                      WHEN fr.first_failure IS NOT NULL THEN fr.first_failure.reason
                      WHEN coalesce(fr.item_refs,0)=0 THEN 'no selected source leaves'
                      WHEN fr.cik_count<>1 THEN 'selected leaves have mixed CIKs'
                      WHEN flat.value_flag OR fr.missing_status OR fr.leaf_value_null
                           THEN 'selected source ownership verified; value invalid'
                      ELSE 'selected leaves verified' END AS reason,
                 fr.cik10
          FROM flat LEFT JOIN flat_refs fr ON fr.node_id=flat.node_id
          LEFT JOIN flat_leaves fl ON fl.node_id=flat.node_id
        ),
        -- Depth > 0: certify only when the whole reachable graph passes.
        walk AS (
          WITH RECURSIVE w(root_id, node_id, depth) AS (
            SELECT root_id, root_id, 0 FROM roots WHERE found AND deep
            UNION
            SELECT w.root_id, r.sid, w.depth+1 FROM w JOIN _lp_ref r ON r.node_id=w.node_id
            WHERE r.rt='OBJECT' AND r.kt='VARCHAR' AND r.kind='metric' AND r.sid IS NOT NULL
              AND w.depth<{MAX_DEPTH + 1}
          )
          SELECT * FROM w
        ),
        reach AS (
          SELECT root_id, node_id, max(depth) AS depth FROM walk GROUP BY root_id, node_id
        ),
        deep_nodes AS (
          SELECT h.root_id, count(*) AS nodes, max(h.depth) AS depth,
                 bool_and(n.node_id IS NOT NULL) AS all_found,
                 bool_or(coalesce(nc.fail_rank,1)>0 OR coalesce(nc.uncertain, true)) AS node_bad,
                 bool_or(coalesce(nc.value_flag, false)) AS value_flag,
                 sum(n.row_bytes) AS row_bytes, max(n.row_bytes) AS max_row_bytes,
                 list_sort(list(n.stored_hash)) AS hashes
          FROM reach h LEFT JOIN _lp_node n ON n.node_id=h.node_id
          LEFT JOIN _lp_nodechk nc ON nc.node_id=h.node_id
          GROUP BY h.root_id
        ),
        deep_refs AS (
          SELECT h.root_id,
                 bool_or(f.fail_rank>0 OR f.uncertain OR NOT coalesce(f.edge_visit_ok, true)) AS ref_bad,
                 bool_or(f.missing_status) AS missing_status,
                 sum(CASE WHEN f.kind='metric' THEN c.payload_bytes ELSE 0 END) AS edge_bytes
          FROM reach h JOIN _lp_refchk f ON f.node_id=h.node_id
          LEFT JOIN _lp_node c ON c.node_id=f.sid
          GROUP BY h.root_id
        ),
        deep_leaf_ids AS (
          SELECT DISTINCT h.root_id, f.sid AS leaf_id, f.leaf_cik10 AS cik10
          FROM reach h JOIN _lp_refchk f ON f.node_id=h.node_id AND f.kind='item'
        ),
        deep_leaves AS (
          SELECT d.root_id, count(*) AS leaves, count(DISTINCT d.cik10) AS cik_count,
                 min(d.cik10) AS cik10, bool_or(l.value_null) AS leaf_value_null,
                 sum(l.leaf_bytes) AS leaf_bytes, max(l.leaf_bytes) AS max_leaf_bytes,
                 bool_or(NOT regexp_full_match(d.leaf_id,'{_SAFE_TEXT}')) AS unsafe
          FROM deep_leaf_ids d JOIN _lp_leaf l ON l.leaf_id=d.leaf_id
          GROUP BY d.root_id
        ),
        deep_outcome AS (
          SELECT n.root_id, r.available_at AS root_at, r.valid_to AS root_valid_to,
                 NOT coalesce(dn.all_found AND NOT dn.node_bad AND NOT dr.ref_bad
                      AND dn.depth<={MAX_DEPTH}
                      AND dn.nodes<{MAX_NODES} AND dn.max_row_bytes<={MAX_BYTES}
                      AND r.payload_bytes+coalesce(dr.edge_bytes,0)<={MAX_BYTES}
                      AND dl.leaves>=1 AND dl.cik_count=1 AND NOT dl.unsafe
                      AND dl.max_leaf_bytes<={MAX_BYTES}
                      AND dn.nodes+dl.leaves<={MAX_BATCH_NODES}
                      AND dn.row_bytes+dl.leaf_bytes<={MAX_BATCH_BYTES}
                      AND r.available_at IS NOT NULL
                      AND (r.valid_to IS NULL OR r.valid_to>r.available_at)
                      AND regexp_full_match(n.root_id,'{_SAFE_TEXT}'), false) AS uncertain,
                 CASE WHEN dn.value_flag OR dr.missing_status OR dl.leaf_value_null
                      THEN 'invalid' ELSE 'qualified' END AS status,
                 CASE WHEN dn.value_flag OR dr.missing_status OR dl.leaf_value_null
                      THEN 'selected source ownership verified; value invalid'
                      ELSE 'selected leaves verified' END AS reason,
                 dl.cik10, dn.hashes
          FROM (SELECT root_id FROM roots WHERE found AND deep) n
          JOIN _lp_node r ON r.node_id=n.root_id
          LEFT JOIN deep_nodes dn ON dn.root_id=n.root_id
          LEFT JOIN deep_refs dr ON dr.root_id=n.root_id
          LEFT JOIN deep_leaves dl ON dl.root_id=n.root_id
        ),
        success_leaves AS (
          SELECT o.root_id, l.leaf_id, o.cik10, l.available_at AS leaf_at, l.period_end AS leaf_end
          FROM flat_outcome o
          JOIN (SELECT DISTINCT node_id, sid FROM _lp_ref WHERE kind='item') f ON f.node_id=o.root_id
          JOIN _lp_leaf l ON l.leaf_id=f.sid
          WHERE NOT o.uncertain AND o.reason IN ({_SUCCESS})
          UNION ALL
          SELECT o.root_id, l.leaf_id, o.cik10, l.available_at, l.period_end
          FROM deep_outcome o JOIN deep_leaf_ids d ON d.root_id=o.root_id
          JOIN _lp_leaf l ON l.leaf_id=d.leaf_id
          WHERE NOT o.uncertain
        ),
        summaries AS (
          SELECT root_id, count(*) AS leaf_count, min(leaf_end) AS oldest_end, max(leaf_end) AS newest_end,
                 max(leaf_at) AS latest_clock,
                 string_agg({digest_leaf}, ',' ORDER BY leaf_id) AS leaf_json
          FROM success_leaves GROUP BY root_id
        ),
        hashes AS (
          SELECT o.root_id, [n.stored_hash] AS hashes FROM flat_outcome o
          JOIN _lp_node n ON n.node_id=o.root_id
          UNION ALL
          SELECT root_id, hashes FROM deep_outcome
        ),
        decided AS (
          SELECT root_id, status, reason, cik10, '{METHOD_EXACT}' AS method FROM flat_outcome
          WHERE NOT uncertain
          UNION ALL
          SELECT root_id, status, reason, cik10, '{METHOD_CERTIFIED}' FROM deep_outcome
          WHERE NOT uncertain
        )
        SELECT d.root_id,d.status,d.reason,
               CASE WHEN d.reason IN ({_SUCCESS}) THEN d.cik10 END AS selected_cik,
               CASE WHEN d.reason IN ({_SUCCESS}) THEN sha256(
                 '["' || d.root_id || '",[' ||
                 array_to_string(list_transform(h.hashes, x -> '"' || x || '"'), ',') ||
                 '],[' || s.leaf_json || ']]') END AS proof_digest,
               CASE WHEN d.reason IN ({_SUCCESS}) THEN s.oldest_end END AS oldest_fiscal_end,
               CASE WHEN d.reason IN ({_SUCCESS}) THEN s.newest_end END AS newest_fiscal_end,
               CASE WHEN d.reason IN ({_SUCCESS}) THEN s.latest_clock END AS latest_input_clock,
               CASE WHEN d.reason IN ({_SUCCESS}) THEN s.leaf_count ELSE 0 END AS leaf_count,
               d.method
        FROM decided d LEFT JOIN hashes h ON h.root_id=d.root_id
        LEFT JOIN summaries s ON s.root_id=d.root_id
    """)


def prove_roots(
    con: Any, expected_hashes: Mapping[tuple[str, str], str], *,
    roots_table: str = "_lp_roots",
    fallback: Callable[[Any, list[str]], dict[str, Any]] | None = None,
) -> dict[str, int]:
    """Stage ``_lp_result`` with one proof row per distinct root in ``roots_table``.

    ``roots_table`` has a ``root_id`` column. ``fallback(con, ids)`` returns the
    resolver's ``LineageQualification`` per id for every root the set path does
    not decide; by default it is the FQ1 batch resolver.
    """
    if roots_table != "_lp_roots":
        con.execute(f"CREATE OR REPLACE TEMP TABLE _lp_roots AS SELECT DISTINCT root_id FROM {roots_table}")
    _stage_expected(con, expected_hashes)
    levels = _fetch_levels(con)
    _stage_checks(con)
    _stage_results(con)
    pending = [row[0] for row in con.execute("""
        SELECT DISTINCT r.root_id FROM _lp_roots r
        LEFT JOIN _lp_result p ON p.root_id=r.root_id WHERE p.root_id IS NULL ORDER BY 1
    """).fetchall()]
    if pending:
        if fallback is None:
            from ..derived_lineage import qualify_selected_lineage
            from ..fundamental_signal_research import _qualify_proof_batch

            def fallback(connection: Any, ids: list[str]) -> dict[str, Any]:
                return _qualify_proof_batch(connection, ids, dict(expected_hashes), qualify_selected_lineage)

        for start in range(0, len(pending), 256):
            ids = pending[start:start + 256]
            answers = fallback(con, ids)
            payload = []
            for root in ids:
                proof = answers[root]
                clocks = [clock for clock in proof.input_clocks if clock is not None]
                ends = [end for end in proof.fiscal_ends if end is not None]
                payload.append([root, proof.status, proof.reason, proof.selected_cik, proof.digest,
                                min(ends) if ends else None, max(ends) if ends else None,
                                max(clocks) if clocks else None, len(proof.leaf_ids), METHOD_FALLBACK])
            con.executemany("INSERT INTO _lp_result VALUES (?,?,?,?,?,?,?,?,?,?)", payload)
    counts = dict(con.execute("SELECT method, count(*) FROM _lp_result GROUP BY 1").fetchall())
    counts["levels"] = levels
    return {str(key): int(value) for key, value in counts.items()}


def result_rows(con: Any) -> list[tuple[Any, ...]]:
    """Debug/test helper: the staged result ordered by root."""
    return con.execute("SELECT * FROM _lp_result ORDER BY root_id").fetchall()


__all__ = [
    "MAX_BATCH_BYTES",
    "MAX_BATCH_NODES",
    "MAX_BYTES",
    "MAX_DEPTH",
    "MAX_NODES",
    "METHOD_CERTIFIED",
    "METHOD_EXACT",
    "METHOD_FALLBACK",
    "prove_roots",
    "result_rows",
]
