"""Bounded SQL event frames for the quarterly engine (no Python fact panels).

Temporary relations are private to the connection. A security's entire requested
DAG is staged before its canonical rows are replaced in one transaction.
"""

from __future__ import annotations

import hashlib
import json
from typing import Any

from .derived_dsl import BinOp, Call, LowerContext, Lowered, lower, parse_expression
from .derived_registry import DerivedMetricDefinition

STATE_COLUMNS = (
    "derived_value_id", "source", "security_id", "metric_code", "metric_window",
    "period_end", "value", "available_at", "inputs_hash", "as_of_date",
    "is_latest_revision", "run_id", "value_status", "revision_group_id",
    "revision_sequence", "revision_count", "valid_to", "target_bucket",
    "definition_hash", "arithmetic_available_at", "history_status",
)


def quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def bucket_sql(column: str) -> str:
    return (
        f"CAST(floor((year({column}) * 12 + month({column}) - 1 + "
        f"CASE WHEN day({column}) >= 15 THEN 1 ELSE 0 END) / 3.0) AS BIGINT)"
    )


def definition_hash(definition: DerivedMetricDefinition) -> str:
    payload = ["quarter-pit-v2", definition.metric_code, definition.window,
               definition.expression, sorted(definition.inputs), definition.version]
    return hashlib.sha256(json.dumps(payload, separators=(",", ":")).encode()).hexdigest()


def enforce_count(con: Any, table: str, limit: int, label: str) -> int:
    count = int(con.execute(f"SELECT count(*) FROM {table}").fetchone()[0])
    if count > limit:
        raise RuntimeError(f"derived PIT {label} has {count} rows, exceeding bounded limit {limit}")
    return count


def prepare_security(con: Any, security_id: str, input_limit: int) -> None:
    # Check before materialization; no assumption that one issuer is small.
    count = int(con.execute("""
        SELECT count(*) FROM fundamental_standardized
        WHERE security_id = ? AND basis IN ('quarterly', 'instant')
          AND available_at IS NOT NULL
    """, [security_id]).fetchone()[0])
    if count > input_limit:
        raise RuntimeError(f"derived PIT input limit for {security_id}: {count} > {input_limit}")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_raw AS
        SELECT standardized_id, source, security_id, canonical_code, basis,
               period_end, value, available_at, rule_id, {bucket_sql('period_end')} AS bucket
        FROM fundamental_standardized
        WHERE security_id = ? AND basis IN ('quarterly', 'instant')
          AND available_at IS NOT NULL
    """, [security_id])
    # Latest visible period wins within a bucket; availability wins within a
    # period. Source/rule/basis/stable ID break equal-event ties, never load time
    # or mutable revision counters. Whole structs retain explicit NULL states.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _pit_items AS
        WITH events AS (
            SELECT security_id, canonical_code AS code, bucket, available_at AS event_at,
                   arg_max(struct_pack(value := value, available_at := available_at,
                       period_end := period_end, state_id := standardized_id,
                       source := source, rule_id := rule_id, basis := basis),
                       (period_end, available_at, source, rule_id, basis, standardized_id))
                   OVER (PARTITION BY security_id, canonical_code, bucket
                         ORDER BY available_at RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS state
            FROM _pit_raw
            QUALIFY row_number() OVER (
                PARTITION BY security_id, canonical_code, bucket, available_at
                ORDER BY period_end DESC, source DESC, rule_id DESC, basis DESC, standardized_id DESC
            ) = 1
        ), changed AS (
            SELECT * FROM events
            QUALIFY lag(state) OVER (PARTITION BY security_id, code, bucket ORDER BY event_at)
                    IS DISTINCT FROM state
        )
        SELECT *, lead(event_at) OVER (
            PARTITION BY security_id, code, bucket ORDER BY event_at) AS valid_to
        FROM changed
    """)
    # Output period identity is observed as of the event. Future stub dates
    # cannot rename historical states or create target periods prematurely.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _pit_targets AS
        WITH events AS (
            SELECT security_id, bucket, available_at AS event_at,
                   max(period_end) OVER (PARTITION BY security_id, bucket
                       ORDER BY available_at RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS period_end
            FROM _pit_raw
            QUALIFY row_number() OVER (
                PARTITION BY security_id, bucket, available_at ORDER BY period_end DESC) = 1
        ), changed AS (
            SELECT * FROM events
            QUALIFY lag(period_end) OVER (PARTITION BY security_id, bucket ORDER BY event_at)
                    IS DISTINCT FROM period_end
        )
        SELECT *, lead(event_at) OVER (
            PARTITION BY security_id, bucket ORDER BY event_at) AS valid_to
        FROM changed
    """)
    con.execute("CREATE OR REPLACE TEMP TABLE _pit_stage AS SELECT * FROM derived_metric_values WHERE false")


def prepare_metric(con: Any, definition: DerivedMetricDefinition, lag: int, candidate_limit: int) -> int:
    items = ",".join(quote(c) for c in definition.item_inputs) or "''"
    metrics = ",".join(quote(c) for c in definition.metric_inputs) or "''"
    # State relations stay complete across event chunks, including predecessor
    # intervals. There is no event x historical-quarter cross product.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_inputs AS
        SELECT security_id, 'item' AS kind, code, bucket, event_at, valid_to,
               state.value AS value, state.available_at AS input_at,
               to_json(state) AS lineage
        FROM _pit_items WHERE code IN ({items})
        UNION ALL
        SELECT security_id, 'metric', metric_code, target_bucket, available_at, valid_to,
               value, available_at,
               to_json(struct_pack(state_id := derived_value_id, inputs_hash := inputs_hash,
                   value := value, value_status := value_status, available_at := available_at,
                   period_end := period_end, source := source, definition_hash := definition_hash))
        FROM _pit_stage WHERE metric_code IN ({metrics})
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_keys AS
        WITH affected AS (
            SELECT i.security_id, i.bucket + o.offset_value AS target_bucket, i.event_at
            FROM _pit_inputs i, range(0, {lag + 1}) o(offset_value)
            UNION
            SELECT security_id, bucket, event_at FROM _pit_targets
        ), visible AS (
            SELECT a.*, t.period_end FROM affected a
            ASOF JOIN _pit_targets t
              ON a.security_id = t.security_id AND a.target_bucket = t.bucket AND a.event_at >= t.event_at
        )
        SELECT *, row_number() OVER (ORDER BY security_id, target_bucket, event_at) AS key_number
        FROM visible
    """)
    count = enforce_count(con, "_pit_keys", candidate_limit, "candidate events")
    con.execute("CREATE OR REPLACE TEMP TABLE _pit_metric AS SELECT * FROM _pit_stage WHERE false")
    return count


def frame_sql(definition: DerivedMetricDefinition, lowered: Lowered, context: LowerContext) -> str:
    """Insert one bounded key chunk; bind start, end, source, run_id.

    Every window partition includes the target/event key. ASOF joins produce
    one whole state per code/frame bucket. A chunk never truncates input history.
    """
    joins: list[str] = []
    projections: list[str] = []
    lineage: list[str] = []
    for index, (kind, code) in enumerate(
        [("item", c) for c in sorted(definition.item_inputs)]
        + [("metric", c) for c in sorted(definition.metric_inputs)]
    ):
        alias = f"i{index}"
        joins.append(
            f"ASOF LEFT JOIN (SELECT * FROM _pit_inputs WHERE kind={quote(kind)} AND code={quote(code)}) {alias} "
            f"ON f.security_id={alias}.security_id AND f.bucket={alias}.bucket AND f.event_at >= {alias}.event_at"
        )
        projections.extend([
            f'{alias}.value AS "{code}"', f'{alias}.input_at AS "{code}__at"',
        ])
        lineage.append(f"struct_pack(kind := {quote(kind)}, code := {quote(code)}, state := {alias}.lineage)")
    projection = ", " + ", ".join(projections) if projections else ""
    line = "to_json([" + ",".join(lineage) + "])" if lineage else "'[]'"
    node = parse_expression(definition.expression)
    denominator = None
    if isinstance(node, BinOp) and node.op == "/":
        denominator = node.right
    elif isinstance(node, Call) and node.name == "safe_div":
        denominator = node.args[1]
    reason = (
        f"CASE WHEN ({lower(denominator, context).value_sql}) = 0 THEN 'zero_denominator' "
        "ELSE 'missing_input_or_domain' END"
        if denominator is not None else "'missing_input_or_domain'"
    )
    fingerprint = quote(definition_hash(definition))
    return f"""
        INSERT INTO _pit_metric ({', '.join(STATE_COLUMNS)})
        WITH frame AS (
            SELECT k.*, gs.bucket, gs.bucket - (k.target_bucket - {lowered.max_lag}) AS rn
            FROM (SELECT * FROM _pit_keys WHERE key_number BETWEEN ? AND ?) k,
                 LATERAL range(k.target_bucket - {lowered.max_lag}, k.target_bucket + 1) gs(bucket)
        ), base AS (
            SELECT f.*{projection}, {line} AS lineage
            FROM frame f {' '.join(joins)}
        ), hashed AS (
            SELECT key_number, sha256(to_json(struct_pack(definition := {fingerprint}, frame := list(
                struct_pack(bucket := bucket, inputs := lineage) ORDER BY bucket)))) AS inputs_hash
            FROM base GROUP BY key_number
        ), computed AS (
            SELECT b.*, {lowered.value_sql} AS result,
                   {lowered.availability_sql} AS arithmetic_at, {reason} AS invalid_reason
            FROM base b
        ), target AS (
            SELECT c.*, h.inputs_hash, ? AS output_source, ? AS output_run
            FROM computed c JOIN hashed h USING (key_number)
            WHERE c.bucket = c.target_bucket
        ), identified AS (
            SELECT *, sha256(to_json(struct_pack(contract := 'quarter-pit-v2', source := output_source,
                security := security_id, metric := {quote(definition.metric_code)},
                metric_window := {quote(definition.window)}, bucket := target_bucket,
                definition := {fingerprint}))) AS group_id
            FROM target
        )
        SELECT sha256(to_json(struct_pack(group_id := group_id, event_at := event_at,
                   period_end := period_end))),
               output_source, security_id, {quote(definition.metric_code)}, {quote(definition.window)},
               period_end, CASE WHEN isfinite(result) THEN result END, event_at,
               inputs_hash, CAST(event_at AS DATE), true, output_run,
               CASE WHEN result IS NULL THEN invalid_reason WHEN NOT isfinite(result) THEN 'nonfinite'
                    ELSE 'valid' END,
               group_id, 1, 1, NULL, target_bucket, {fingerprint}, arithmetic_at, 'event_reconstructed'
        FROM identified
    """


def finish_metric(con: Any) -> None:
    # Compress only identical complete frame lineage/status/value/period. Keep
    # same-value lineage transitions. Compression spans all event chunks.
    con.execute(f"""
        INSERT INTO _pit_stage ({', '.join(STATE_COLUMNS)})
        WITH changed AS (
            SELECT * FROM _pit_metric
            QUALIFY lag(struct_pack(inputs := inputs_hash, value := value,
                                    status := value_status, period := period_end))
                    OVER (PARTITION BY revision_group_id ORDER BY available_at)
                IS DISTINCT FROM struct_pack(inputs := inputs_hash, value := value,
                                             status := value_status, period := period_end)
        )
        SELECT derived_value_id, source, security_id, metric_code, metric_window,
               period_end, value, available_at, inputs_hash, as_of_date,
               lead(available_at) OVER w IS NULL, run_id, value_status, revision_group_id,
               row_number() OVER w, count(*) OVER (PARTITION BY revision_group_id),
               lead(available_at) OVER w, target_bucket, definition_hash, arithmetic_available_at, history_status
        FROM changed WINDOW w AS (PARTITION BY revision_group_id ORDER BY available_at)
    """)


def cleanup(con: Any) -> None:
    for table in ("_pit_metric", "_pit_keys", "_pit_inputs", "_pit_stage", "_pit_targets", "_pit_items", "_pit_raw"):
        con.execute(f"DROP TABLE IF EXISTS {table}")
