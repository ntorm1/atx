"""Bounded SQL event frames for the quarterly engine (no Python fact panels).

Temporary relations are private to the connection. A security's entire requested
DAG is staged before its canonical rows are replaced in one transaction.
"""

from __future__ import annotations

import hashlib
import json
from typing import Any

from . import _derived_annual as annual
from . import _split_epochs
from .derived_dsl import BinOp, Call, LowerContext, Lowered, lower, lower_selected_refs, parse_expression
from .derived_registry import DerivedMetricDefinition

STATE_COLUMNS = (
    "derived_value_id", "source", "security_id", "metric_code", "metric_window",
    "period_end", "value", "available_at", "inputs_hash", "as_of_date",
    "is_latest_revision", "run_id", "value_status", "revision_group_id",
    "revision_sequence", "revision_count", "valid_to", "target_bucket",
    "definition_hash", "arithmetic_available_at", "history_status",
    "value_origin", "fiscal_period_start", "fiscal_period_end",
    "selected_input_refs_json", "selected_input_refs_hash",
)


def quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def selected_ref_sql(kind: str, code: str, state: str, bucket: str) -> str:
    """A direct operand, including a missing selected state, as one JSON scalar."""
    return ("to_json(struct_pack(" +
            f"kind := {quote(kind)}, code := {quote(code)}, bucket := {bucket}, " +
            f'"offset" := f.target_bucket - {bucket}, ' +
            f"status := CASE WHEN {state}.state_id IS NULL OR {state}.value IS NULL " +
            "THEN 'missing' ELSE 'selected' END, " +
            f"state_id := {state}.state_id, available_at := {state}.input_at, " +
            f"cik := {state}.cik, basis := {state}.basis, " +
            f"source := {state}.input_source, period_start := {state}.input_start, " +
            f"period_end := {state}.input_end, " +
            f"inputs_hash := {state}.dependency_inputs_hash, " +
            f"definition_hash := {state}.dependency_definition_hash))")


def bucket_sql(column: str) -> str:
    return (
        f"CAST(floor((year({column}) * 12 + month({column}) - 1 + "
        f"CASE WHEN day({column}) >= 15 THEN 1 ELSE 0 END) / 3.0) AS BIGINT)"
    )


def definition_hash(definition: DerivedMetricDefinition,
                    annual_plan: annual.AnnualPlan = annual.EMPTY_PLAN) -> str:
    payload = ["quarter-pit-v3-annual", definition.metric_code, definition.window,
               definition.expression, sorted(definition.inputs), definition.version,
               repr(annual_plan.alternative), annual_plan.weighted_codes]
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
        WHERE security_id = ? AND basis IN ('quarterly', 'instant', 'annual')
          AND available_at IS NOT NULL
    """, [security_id]).fetchone()[0])
    if count > input_limit:
        raise RuntimeError(f"derived PIT input limit for {security_id}: {count} > {input_limit}")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_raw AS
        SELECT standardized_id, cik, source, security_id, canonical_code, basis,
               period_start, period_end, value, available_at, rule_id, {bucket_sql('period_end')} AS bucket
        FROM fundamental_standardized
        WHERE security_id = ? AND basis IN ('quarterly', 'instant', 'annual')
          AND available_at IS NOT NULL
          AND (basis <> 'annual' OR date_diff('day', period_start, period_end) + 1 BETWEEN 330 AND 380)
    """, [security_id])
    # Latest visible period wins within a bucket; availability wins within a
    # period. Source/rule/basis/stable ID break equal-event ties, never load time
    # or mutable revision counters. Whole structs retain explicit NULL states.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _pit_items AS
        WITH events AS (
            SELECT security_id, canonical_code AS code, bucket, available_at AS event_at,
                   arg_max(struct_pack(value := value, available_at := available_at,
                       period_start := period_start, period_end := period_end, state_id := standardized_id,
                       source := source, rule_id := rule_id, basis := basis, cik := cik),
                       (period_end, available_at, source, rule_id, basis, standardized_id))
                   OVER (PARTITION BY security_id, canonical_code, bucket
                         ORDER BY available_at RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS state
            FROM _pit_raw WHERE basis <> 'annual'
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
    annual.prepare_security(con)
    _split_epochs.prepare_split_epochs(con, security_id)
    con.execute("CREATE OR REPLACE TEMP TABLE _pit_stage AS SELECT * FROM derived_metric_values WHERE false")


def prepare_metric(con: Any, definition: DerivedMetricDefinition, lag: int, candidate_limit: int,
                   annual_plan: annual.AnnualPlan = annual.EMPTY_PLAN) -> int:
    items = ",".join(quote(c) for c in definition.item_inputs) or "''"
    metrics = ",".join(quote(c) for c in definition.metric_inputs) or "''"
    # State relations stay complete across event chunks, including predecessor
    # intervals. There is no event x historical-quarter cross product.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_inputs AS
        SELECT security_id, 'item' AS kind, code, bucket, event_at, valid_to,
               state.value AS value, state.available_at AS input_at,
               to_json(state) AS lineage,
               state.state_id AS state_id, state.cik AS cik, state.basis AS basis,
               state.source AS input_source, state.period_start AS input_start,
               state.period_end AS input_end, NULL::VARCHAR AS dependency_inputs_hash,
               NULL::VARCHAR AS dependency_definition_hash,
               CASE WHEN state.basis = 'instant' THEN NULL ELSE state.period_start END AS fiscal_period_start,
               state.period_end AS fiscal_period_end, state.basis AS value_origin
        FROM _pit_items WHERE code IN ({items})
        UNION ALL
        SELECT security_id, 'metric', metric_code, target_bucket, available_at, valid_to,
               value, available_at,
               to_json(struct_pack(state_id := derived_value_id, inputs_hash := inputs_hash,
                   value := value, value_status := value_status, available_at := available_at,
                   period_end := period_end, source := source, definition_hash := definition_hash,
                   value_origin := value_origin, fiscal_period_start := fiscal_period_start,
                   fiscal_period_end := fiscal_period_end)),
               derived_value_id, NULL::VARCHAR, NULL::VARCHAR, source,
               fiscal_period_start, fiscal_period_end, inputs_hash, definition_hash,
               fiscal_period_start, fiscal_period_end, value_origin
        FROM _pit_stage WHERE metric_code IN ({metrics})
    """)
    annual_events = ""
    if annual_plan.codes:
        annual_codes = ",".join(quote(code) for code in annual_plan.codes)
        annual_events = f"""
            UNION SELECT security_id, bucket, event_at FROM _pit_annual_items WHERE code IN ({annual_codes})
            UNION SELECT security_id, bucket, event_at FROM _pit_annual_targets
        """
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_keys AS
        WITH affected AS (
            SELECT i.security_id, i.bucket + o.offset_value AS target_bucket, i.event_at
            FROM _pit_inputs i, range(0, {lag + 1}) o(offset_value)
            UNION
            SELECT security_id, bucket, event_at FROM _pit_targets
            {annual_events}
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


def frame_sql(definition: DerivedMetricDefinition, lowered: Lowered, context: LowerContext,
              annual_plan: annual.AnnualPlan = annual.EMPTY_PLAN) -> str:
    """Insert one bounded key chunk; bind start, end, source, run_id.

    Every window partition includes the target/event key. ASOF joins produce
    one whole state per code/frame bucket. A chunk never truncates input history.
    """
    joins: list[str] = []
    projections: list[str] = []
    lineage: list[str] = []
    refs: dict[str, str] = {}
    span_refs: dict[str, annual.Span] = {}
    exponents = annual.plan_share_exponents(annual_plan)
    for index, (kind, code) in enumerate(
        [("item", c) for c in sorted(definition.item_inputs)]
        + [("metric", c) for c in sorted(definition.metric_inputs)]
    ):
        alias = f"i{index}"
        joins.append(
            f"ASOF LEFT JOIN (SELECT * FROM _pit_inputs WHERE kind={quote(kind)} AND code={quote(code)}) {alias} "
            f"ON f.security_id={alias}.security_id AND f.bucket={alias}.bucket AND f.event_at >= {alias}.event_at"
        )
        # A share-basis operand is restated on the frame's split basis: the
        # filing's basis is fixed by its clock, and only splits known by the
        # frame's clock are applied (_split_epochs). A mixed unit cannot be rebased.
        value, clock, basis = f"{alias}.value", "NULL::TIMESTAMP", "true"
        exponent = exponents.get(code, 0)
        if exponent != 0:
            clock, basis = f'b."{code}__at"', "false"
        if exponent:
            factor = _split_epochs.rebase_factor_sql(f"{alias}.input_at", "f.event_at", exponent)
            known = _split_epochs.basis_known_sql(f"{alias}.input_at", "f.event_at")
            value, basis = f"({value} * {factor})", f'b."{code}__basis"'
            projections.append(f'{known} AS "{code}__basis"')
            # An operand of the frame's own filing is on its basis by construction;
            # any other proof rests on reconstructed vendor bars, never a verified record.
            events = _split_epochs.applied_events_sql(f"{alias}.input_at", "f.event_at")
            proof = (f"CASE WHEN {alias}.input_at = f.event_at THEN 'frame_filing' "
                     f"WHEN {known} THEN 'vendor_reconstructed' ELSE 'unproven' END")
            lineage.append(f"struct_pack(kind := 'split_basis', code := {quote(code)}, "
                           f"state := to_json(struct_pack(factor := {factor}, basis_known := {known}, "
                           f"proof := {proof}, events := {events})))")
        projections.extend([
            f'{value} AS "{code}"', f'{alias}.input_at AS "{code}__at"',
            f'{alias}.fiscal_period_start AS "{code}__start"',
            f'{alias}.fiscal_period_end AS "{code}__end"',
            f'{alias}.value_origin AS "{code}__origin"',
            f'{selected_ref_sql(kind, code, alias, "f.bucket")} AS "{code}__ref"',
        ])
        refs[code] = f'b."{code}__ref"'
        span_refs[code] = annual.Span(
            f'b."{code}__start"', f'b."{code}__end"',
            f'coalesce(b."{code}__origin" IN (\'annual_fallback\', \'annual_dependency\', \'annual\'), false)',
            f'coalesce(b."{code}__origin" <> \'incomparable\' AND '
            f'(b."{code}__origin" <> \'quarterly\' OR b."{code}__start" IS NOT NULL), true)', 0,
            False, clock, basis,
        )
        lineage.append(f"struct_pack(kind := {quote(kind)}, code := {quote(code)}, state := {alias}.lineage)")
    extra_joins, extra_columns, extra_lineage = annual.frame_annual_columns(annual_plan, quote)
    joins.extend(extra_joins)
    projections.extend(extra_columns)
    lineage.extend(extra_lineage)
    # Weighted annual averages are an endpoint denominator only. No quarterly
    # item or _q metric receives an annual numeric value.
    columns, availability = dict(context.columns), dict(context.availability)
    dependency_annual = " OR ".join(span_refs[code].annual for code in definition.metric_inputs) or "false"
    for code in annual_plan.weighted_codes:
        choose_annual = f"(({dependency_annual}) OR b.\"{code}\" IS NULL) AND b.annual_end IS NOT NULL"
        columns[code] = annual._case(choose_annual, f'b."annual_{code}"', columns[code])
        availability[code] = annual._case(choose_annual, f'b."annual_{code}__at"', availability[code])
        refs[code] = annual._case(choose_annual, f'b."annual_{code}__ref"', refs[code])
        old = span_refs[code]
        span_refs[code] = annual.Span(
            annual._case(choose_annual, "b.annual_start", old.start),
            annual._case(choose_annual, "b.annual_end", old.end),
            annual._case(choose_annual, "true", old.annual),
            annual._case(choose_annual, "true", old.coherent), 0,
            False, annual._case(choose_annual, f'b."annual_{code}__at"', old.clock),
            annual._case(choose_annual, f'coalesce(b."annual_{code}__basis", false)', old.basis),
        )
    context = LowerContext(context.grid, columns, availability, context.partition_sql, context.order_sql, refs)
    node = parse_expression(definition.expression)
    lowered = lower(node, context)
    selected_refs = lower_selected_refs(node, context)
    span = annual.lower_span(node, context, span_refs, exponents)
    coherent = f"coalesce(({span.coherent}), false)"
    # Only annual-backed arithmetic gets the new comparability gate. The
    # existing quarter-only arithmetic and its missing-start legacy inputs stay.
    annual_guard = f"NOT coalesce(({span.annual}), false) OR ({coherent} AND ({span.end}) = b.period_end)"
    quarter_result = f"CASE WHEN {annual_guard} THEN ({lowered.value_sql}) END"
    choose_annual = "false"
    result_sql, arithmetic_sql = quarter_result, lowered.availability_sql
    start_sql, end_sql = span.start, span.end
    if annual_plan.alternative is not None:
        annual_context = LowerContext(
            context.grid,
            {code: f'b."annual_{code}"' for code in annual_plan.item_codes},
            {code: f'b."annual_{code}__at"' for code in annual_plan.item_codes},
            context.partition_sql, context.order_sql,
            {code: f'b."annual_{code}__ref"' for code in annual_plan.item_codes},
        )
        alternative = lower(annual_plan.alternative, annual_context)
        annual_refs = lower_selected_refs(annual_plan.alternative, annual_context)
        comparable_quarters = (
            f"b.annual_start IS NULL OR ({coherent} AND ({span.start}) = b.annual_start "
            f"AND ({span.end}) = b.annual_end)"
        )
        valid_annual = (f"b.annual_start IS NOT NULL AND ({alternative.availability_sql}) IS NOT NULL "
                        f"AND isfinite(({alternative.value_sql}))")
        # An unrelated global annual span cannot veto this metric's complete
        # quarters when its own annual alternative is unusable. Keep the
        # exact endpoint and full-quarter coherence requirements on this path.
        independent_quarters = (
            f"NOT coalesce(({valid_annual}), false) AND ({coherent}) "
            f"AND ({span.end}) = b.period_end"
        )
        prefer_quarters = (
            f"isfinite(({quarter_result})) AND (({comparable_quarters}) OR ({independent_quarters}))"
        )
        choose_annual = f"NOT coalesce(({prefer_quarters}), false) AND ({valid_annual})"
        selected_refs = annual._case(choose_annual, annual_refs, selected_refs)
        result_sql = (f"CASE WHEN {prefer_quarters} THEN ({quarter_result}) "
                      f"WHEN {valid_annual} THEN ({alternative.value_sql}) "
                      f"WHEN {comparable_quarters} THEN ({quarter_result}) END")
        arithmetic_sql = annual._case(choose_annual, alternative.availability_sql, lowered.availability_sql)
        start_sql = annual._case(choose_annual, "b.annual_start", start_sql)
        end_sql = annual._case(choose_annual, "b.annual_end", end_sql)
    # An annual per-share value filed before the frame's clock is rebased; it is
    # comparable only when its split basis is known (or it is the frame's filing).
    annual_basis = " AND ".join(
        f'(b."annual_{code}__at" = b.event_at OR coalesce(b."annual_{code}__basis", false))'
        for code in annual_plan.codes if annual.item_share_exponent(code)) or "true"
    origin_sql = (
        f"CASE WHEN ({choose_annual}) AND NOT ({annual_basis}) THEN 'incomparable' "
        f"WHEN {choose_annual} THEN 'annual_fallback' WHEN {span.annual} THEN 'annual_dependency' "
        f"WHEN NOT ({coherent}) THEN 'incomparable' WHEN ({span.start}) IS NOT NULL THEN 'quarterly' "
        f"WHEN ({span.end}) IS NOT NULL THEN 'instant' ELSE 'scalar' END"
    )
    projection = ", " + ", ".join(projections) if projections else ""
    line = "to_json([" + ",".join(lineage) + "])" if lineage else "'[]'"
    if any(f"{_split_epochs.LISTS_ALIAS}." in part for part in (*projections, *lineage)):
        joins.insert(0, _split_epochs.LISTS_JOIN)
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
    fingerprint = quote(definition_hash(definition, annual_plan))
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
            SELECT b.*, {result_sql} AS result,
                   {arithmetic_sql} AS arithmetic_at, {reason} AS invalid_reason,
                   {origin_sql} AS selected_origin, {start_sql} AS selected_start, {end_sql} AS selected_end,
                   to_json(struct_pack(version := 1,
                       refs := list_sort(list_distinct(coalesce({selected_refs}, []::JSON[])))))
                       AS selected_input_refs_json
            FROM base b
        ), target AS (
            SELECT c.*, h.inputs_hash, sha256(c.selected_input_refs_json) AS selected_input_refs_hash,
                   ? AS output_source, ? AS output_run
            FROM computed c JOIN hashed h USING (key_number)
            WHERE c.bucket = c.target_bucket
        ), identified AS (
            SELECT *, sha256(to_json(struct_pack(contract := 'quarter-pit-v3-annual', source := output_source,
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
               group_id, 1, 1, NULL, target_bucket, {fingerprint}, arithmetic_at, 'event_reconstructed',
               CASE WHEN result IS NULL OR NOT isfinite(result) THEN 'unavailable' ELSE selected_origin END,
               selected_start, selected_end, selected_input_refs_json, selected_input_refs_hash
        FROM identified
    """


def finish_metric(con: Any) -> None:
    # Compress only identical complete frame lineage/status/value/period. Keep
    # same-value lineage transitions. Compression spans all event chunks.
    con.execute(f"""
        INSERT INTO _pit_stage ({', '.join(STATE_COLUMNS)})
        WITH changed AS (
            SELECT * FROM _pit_metric
            QUALIFY lag(struct_pack(inputs := inputs_hash, selected := selected_input_refs_hash, value := value,
                                    status := value_status, period := period_end, origin := value_origin,
                                    fiscal_start := fiscal_period_start, fiscal_end := fiscal_period_end))
                    OVER (PARTITION BY revision_group_id ORDER BY available_at)
                IS DISTINCT FROM struct_pack(inputs := inputs_hash, selected := selected_input_refs_hash, value := value,
                                             status := value_status, period := period_end, origin := value_origin,
                                             fiscal_start := fiscal_period_start, fiscal_end := fiscal_period_end)
        )
        SELECT derived_value_id, source, security_id, metric_code, metric_window,
               period_end, value, available_at, inputs_hash, as_of_date,
               lead(available_at) OVER w IS NULL, run_id, value_status, revision_group_id,
               row_number() OVER w, count(*) OVER (PARTITION BY revision_group_id),
               lead(available_at) OVER w, target_bucket, definition_hash, arithmetic_available_at, history_status,
               value_origin, fiscal_period_start, fiscal_period_end,
               selected_input_refs_json, selected_input_refs_hash
        FROM changed WINDOW w AS (PARTITION BY revision_group_id ORDER BY available_at)
    """)


def cleanup(con: Any) -> None:
    for table in ("_pit_metric", "_pit_keys", "_pit_inputs", "_pit_stage", "_pit_targets", "_pit_items",
                  "_pit_annual_targets", "_pit_annual_items", "_pit_raw"):
        con.execute(f"DROP TABLE IF EXISTS {table}")
    _split_epochs.cleanup_split_epochs(con)
