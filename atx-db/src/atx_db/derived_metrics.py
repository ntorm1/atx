"""The quarterly derived-metric engine: one generated statement per metric."""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass
from typing import Any

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .derived_dsl import LowerContext, Lowered, compile_expression
from .derived_registry import (
    DERIVED_SOURCE_NAME,
    DerivedMetricDefinition,
    default_derived_definitions,
    known_item_codes,
    topological_order,
    validate_definitions,
)

__all__ = [
    "DerivedMetricsDataset",
    "DerivedMetricsOptions",
    "build_metric_sql",
    "quarterly_context",
    "refresh_derived_metrics",
    "select_security_batches",
]

#: Codes are spliced, unquoted, into double-quoted SQL identifiers (pivot column
#: aliases, generated column references). Only a character class DuckDB's
#: identifier-escape convention can't break out of is safe there; reject
#: anything else before any SQL is built rather than trying to escape it.
_IDENTIFIER_CODE_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def _validate_identifier_code(code: str) -> None:
    if not _IDENTIFIER_CODE_RE.fullmatch(code):
        raise ValueError(
            f"code {code!r} is not safe to use as a SQL identifier; must match {_IDENTIFIER_CODE_RE.pattern}"
        )


_VALUE_COLUMNS = (
    "derived_value_id",
    "source",
    "security_id",
    "metric_code",
    "metric_window",
    "period_end",
    "value",
    "available_at",
    "inputs_hash",
    "as_of_date",
    "is_latest_revision",
    "run_id",
)


@dataclass(frozen=True)
class DerivedMetricsOptions:
    source: str = DERIVED_SOURCE_NAME
    security_ids: tuple[str, ...] | None = None
    metric_codes: tuple[str, ...] | None = None
    batch_size: int = 500
    run_id: str | None = None


def quarterly_context(
    definition: DerivedMetricDefinition,
    *,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
) -> LowerContext:
    columns: dict[str, str] = {}
    availability: dict[str, str] = {}
    for code in item_codes:
        _validate_identifier_code(code)
        columns[code] = f'b."{code}"'
        availability[code] = f'b."{code}__at"'
    for code in metric_codes:
        _validate_identifier_code(code)
        columns[code] = f'd."{code}"'
        availability[code] = f'd."{code}__at"'
    _ = definition
    return LowerContext(
        grid="quarter",
        columns=columns,
        availability=availability,
        partition_sql="b.security_id",
        # Order by the dense, gap-free grid row number, not period_end: a
        # synthesized missing-quarter row carries a NULL period_end (see
        # build_metric_sql's grid CTE), and NULL doesn't sort into its correct
        # chronological position, which would corrupt every ROWS-based window.
        order_sql="b.rn",
    )


def _pivot_pairs(alias: str, codes: Iterable[str], key_column: str) -> str:
    fragments: list[str] = []
    for code in codes:
        _validate_identifier_code(code)
        literal = code.replace("'", "''")
        fragments.append(f"max(CASE WHEN {alias}.{key_column} = '{literal}' THEN {alias}.value END) AS \"{code}\"")
        fragments.append(
            f"max(CASE WHEN {alias}.{key_column} = '{literal}' THEN {alias}.available_at END) AS \"{code}__at\""
        )
    return ",\n           ".join(fragments) if fragments else "NULL AS __unused"


def _in_list(codes: Iterable[str]) -> str:
    quoted = ", ".join("'" + code.replace("'", "''") + "'" for code in codes)
    return quoted or "''"


def build_metric_sql(
    definition: DerivedMetricDefinition,
    *,
    lowered: Lowered,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
    security_count: int,
) -> str:
    """Return the full INSERT statement for one metric and one security batch.

    Placeholders, in order: the security ids for ``facts``, the source and
    security ids for ``deps`` (only when this metric has metric-code
    dependencies), the source for the metric half of ``hash_parts`` (same
    condition), then source, metric_code (id hash), source, metric_code,
    metric_window, run_id.
    """
    securities = ", ".join(["?"] * security_count)
    deps_cte = (
        f"""deps AS (
    SELECT security_id, period_end,
           {_pivot_pairs("m", metric_codes, "metric_code")}
    FROM derived_metric_values m
    WHERE m.source = ? AND m.metric_code IN ({_in_list(metric_codes)})
      AND m.security_id IN ({securities})
    GROUP BY 1, 2
)"""
        if metric_codes
        else """deps AS (
    SELECT CAST(NULL AS VARCHAR) AS security_id, CAST(NULL AS DATE) AS period_end
    WHERE false
)"""
    )
    metric_hash_branch = (
        f"""
    UNION ALL
    SELECT t.security_id, t.period_end,
           'metric:' || m.metric_code || '|' || CAST(m.period_end AS VARCHAR) || '|' || m.inputs_hash AS payload
    FROM grid t
    JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - {lowered.max_lag} AND t.rn
    JOIN derived_metric_values m
      ON m.security_id = lg.security_id AND m.period_end = lg.period_end
     AND m.source = ? AND m.metric_code IN ({_in_list(metric_codes)})"""
        if metric_codes
        else ""
    )
    return f"""
INSERT INTO derived_metric_values ({", ".join(_VALUE_COLUMNS)})
WITH facts AS (
    SELECT security_id, canonical_code, period_end, value, available_at, revision_sequence
    FROM fundamental_standardized
    WHERE is_latest_revision
      AND basis IN ('quarterly', 'instant')
      AND value IS NOT NULL
      AND available_at IS NOT NULL
      AND security_id IN ({securities})
), picked AS (
    SELECT security_id, canonical_code, period_end,
           arg_max(value, (available_at, revision_sequence)) AS value,
           arg_max(revision_sequence, (available_at, revision_sequence)) AS revision_sequence,
           max(available_at) AS available_at,
           -- Quarter-bucket a period_end onto a security-agnostic calendar axis so
           -- 52/53-week fiscal quarter ends (e.g. Dec 30 vs Jan 2, Jan 28 vs Feb 1)
           -- that straddle a month boundary land in the same bucket as a
           -- month-end reporter's quarter. Buckets are pure calendar math, not an
           -- observed-date union, so a security missing a whole quarter gets a
           -- synthesized empty bucket instead of silently shrinking every window.
           CAST(floor((year(period_end) * 12 + month(period_end) - 1 +
                CASE WHEN day(period_end) >= 15 THEN 1 ELSE 0 END) / 3.0) AS BIGINT) AS bucket
    FROM facts
    GROUP BY 1, 2, 3
), picked_bucketed AS (
    -- Two distinct period_ends for the same security+item can collide into one
    -- bucket (chiefly a fiscal-year-end change producing a stub period). Resolve
    -- deterministically by keeping the fact with the latest period_end; the
    -- collision is also counted separately for the refresh result detail.
    SELECT security_id, canonical_code, bucket,
           arg_max(value, period_end) AS value,
           arg_max(revision_sequence, period_end) AS revision_sequence,
           arg_max(available_at, period_end) AS available_at,
           arg_max(period_end, period_end) AS period_end
    FROM picked
    GROUP BY 1, 2, 3
), bucket_period_end AS (
    SELECT security_id, bucket, max(period_end) AS period_end
    FROM picked_bucketed
    GROUP BY 1, 2
), bucket_extent AS (
    SELECT security_id, min(bucket) AS min_bucket, max(bucket) AS max_bucket
    FROM picked_bucketed
    GROUP BY 1
), grid_buckets AS (
    -- Dense per-security bucket sequence: every calendar quarter between the
    -- security's first and last observed bucket gets a row, real or not.
    SELECT be.security_id, gs.bucket AS bucket, gs.bucket - be.min_bucket + 1 AS rn
    FROM bucket_extent be, LATERAL generate_series(be.min_bucket, be.max_bucket) AS gs(bucket)
), grid AS (
    -- A bucket with no observed fact for any item keeps a NULL period_end: it
    -- exists only so ROWS frames count a real calendar quarter, never so a
    -- synthesized date can be emitted as a derived_metric_values row.
    SELECT gb.security_id, gb.bucket, gb.rn, bpe.period_end AS period_end
    FROM grid_buckets gb
    LEFT JOIN bucket_period_end bpe ON bpe.security_id = gb.security_id AND bpe.bucket = gb.bucket
), base AS (
    SELECT g.security_id, g.period_end, g.rn,
           {_pivot_pairs("p", item_codes, "canonical_code")}
    FROM grid g
    LEFT JOIN picked_bucketed p ON p.security_id = g.security_id AND p.bucket = g.bucket
    GROUP BY 1, 2, 3
), {deps_cte}, hash_parts AS (
    SELECT t.security_id, t.period_end,
           l.canonical_code || '|' || CAST(l.period_end AS VARCHAR) || '|' ||
           CAST(l.revision_sequence AS VARCHAR) || '|' || CAST(l.value AS VARCHAR) AS payload
    FROM grid t
    JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - {lowered.max_lag} AND t.rn
    JOIN picked_bucketed l ON l.security_id = lg.security_id AND l.bucket = lg.bucket
     AND l.canonical_code IN ({_in_list(item_codes)}){metric_hash_branch}
), hashed AS (
    SELECT security_id, period_end,
           sha256(string_agg(payload, ';' ORDER BY payload)) AS inputs_hash
    FROM hash_parts
    GROUP BY 1, 2
), computed AS (
    SELECT b.security_id, b.period_end,
           {lowered.value_sql} AS value,
           {lowered.availability_sql} AS available_at
    FROM base b
    LEFT JOIN deps d ON d.security_id = b.security_id AND d.period_end = b.period_end
)
SELECT sha256(? || '|' || c.security_id || '|' || ? || '|' || CAST(c.period_end AS VARCHAR)),
       ?, c.security_id, ?, ?, c.period_end, c.value, c.available_at, h.inputs_hash,
       CAST(c.available_at AS DATE), true, ?
FROM computed c
JOIN hashed h ON h.security_id = c.security_id AND h.period_end = c.period_end
WHERE c.value IS NOT NULL AND isfinite(c.value) AND c.available_at IS NOT NULL
  AND c.period_end IS NOT NULL
ORDER BY c.security_id, c.period_end
"""


def select_security_batches(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> list[tuple[str, ...]]:
    options = options or DerivedMetricsOptions()
    if options.security_ids is not None:
        identifiers = sorted(options.security_ids)
    else:
        rows = store.con.execute(
            """
            SELECT DISTINCT security_id
            FROM fundamental_standardized
            WHERE is_latest_revision AND basis IN ('quarterly', 'instant')
            ORDER BY security_id
            """
        ).fetchall()
        identifiers = [str(row[0]) for row in rows]
    size = max(1, int(options.batch_size))
    return [tuple(identifiers[start : start + size]) for start in range(0, len(identifiers), size)]


def _expand_metric_codes(requested: tuple[str, ...], by_code: dict[str, DerivedMetricDefinition]) -> frozenset[str]:
    """Expand ``requested`` metric codes to their transitive ``metric:`` dependency closure.

    A caller asking for e.g. ``gross_margin`` without separately listing the
    metrics it is built from (``gross_profit_ttm``, ``revenue_ttm``, and
    transitively ``gross_profit_q``) would otherwise see those dependencies
    left un-refreshed for this run: the ``deps`` join in ``build_metric_sql``
    would find no matching rows and every ``gross_margin`` value would come
    out ``NULL`` and be silently dropped by the ``value IS NOT NULL`` filter,
    with no diagnostic. Rather than requiring the caller to pre-compute the
    closure or erroring out, the requested set is always auto-expanded here so
    every transitive dependency is computed in the same run, in topological
    order, ahead of the metrics that need it.
    """
    expanded: set[str] = set()
    stack = list(requested)
    while stack:
        code = stack.pop()
        if code in expanded:
            continue
        expanded.add(code)
        definition = by_code.get(code)
        if definition is not None:
            stack.extend(definition.metric_inputs)
    return frozenset(expanded)


def refresh_derived_metrics(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> int:
    """Recompute quarterly derived metrics for the selected securities and metrics.

    When ``options.metric_codes`` is given, it is auto-expanded to include the
    transitive ``metric:`` dependency closure of the requested metrics (see
    :func:`_expand_metric_codes`) before being intersected with the catalog and
    topologically ordered, so a dependency is never silently left uncomputed.
    """
    options = options or DerivedMetricsOptions()
    store.initialize()
    definitions = default_derived_definitions()
    validate_definitions(definitions, item_codes=known_item_codes())
    quarterly = tuple(d for d in definitions if d.window != "daily")
    by_code = {d.metric_code: d for d in quarterly}
    wanted_codes = None if options.metric_codes is None else _expand_metric_codes(options.metric_codes, by_code)
    ordered = tuple(d for d in topological_order(quarterly) if wanted_codes is None or d.metric_code in wanted_codes)
    batches = select_security_batches(store, options)
    inserted = 0
    for batch in batches:
        with store.transaction():
            predicates = ["source = ?", f"security_id IN ({', '.join(['?'] * len(batch))})"]
            params: list[Any] = [options.source, *batch]
            if options.metric_codes is not None:
                codes = [d.metric_code for d in ordered]
                predicates.append(f"metric_code IN ({', '.join(['?'] * len(codes))})")
                params.extend(codes)
            store.con.execute(f"DELETE FROM derived_metric_values WHERE {' AND '.join(predicates)}", params)
            for definition in ordered:
                item_codes = tuple(sorted(definition.item_inputs))
                metric_codes = tuple(sorted(definition.metric_inputs))
                lowered = compile_expression(
                    definition.expression,
                    quarterly_context(
                        definition,
                        item_codes=item_codes,
                        metric_codes=metric_codes,
                    ),
                )
                sql = build_metric_sql(
                    definition,
                    lowered=lowered,
                    item_codes=item_codes,
                    metric_codes=metric_codes,
                    security_count=len(batch),
                )
                bind: list[Any] = list(batch)
                if metric_codes:
                    bind.extend([options.source, *batch])
                    bind.append(options.source)
                bind.extend(
                    [
                        options.source,
                        definition.metric_code,
                        options.source,
                        definition.metric_code,
                        definition.window,
                        options.run_id,
                    ]
                )
                cursor = store.con.execute(sql, bind)
                affected = cursor.fetchall()
                inserted += int(affected[0][0]) if affected else 0
    return inserted


def _grid_bucket_collision_count(store: DuckDBStore, security_ids: tuple[str, ...] | None) -> int:
    """Count (security_id, canonical_code, bucket) groups with more than one observed period_end.

    Mirrors the ``picked`` -> ``picked_bucketed`` collapse in ``build_metric_sql``:
    a count above zero means at least one security had two distinct fiscal
    period-ends (typically a fiscal-year-end change producing a stub period)
    land in the same calendar quarter bucket, which ``picked_bucketed`` already
    resolves deterministically by keeping the fact with the later period_end.
    This is purely observability -- it does not change engine behavior -- and is
    surfaced via :class:`DerivedMetricsDataset`'s ``details``.
    """
    predicate = ""
    params: list[Any] = []
    if security_ids is not None:
        predicate = f"AND security_id IN ({', '.join(['?'] * len(security_ids))})"
        params = list(security_ids)
    row = store.con.execute(
        f"""
        SELECT count(*) FROM (
            SELECT security_id, canonical_code, bucket
            FROM (
                SELECT DISTINCT security_id, canonical_code, period_end,
                       CAST(floor((year(period_end) * 12 + month(period_end) - 1 +
                            CASE WHEN day(period_end) >= 15 THEN 1 ELSE 0 END) / 3.0) AS BIGINT) AS bucket
                FROM fundamental_standardized
                WHERE is_latest_revision AND basis IN ('quarterly', 'instant')
                  AND value IS NOT NULL AND available_at IS NOT NULL {predicate}
            )
            GROUP BY 1, 2, 3
            HAVING count(*) > 1
        )
        """,
        params,
    ).fetchone()
    return int(row[0]) if row else 0


class DerivedMetricsDataset(Dataset):
    dataset_id = "derived_metrics"
    source_name = DERIVED_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        resolved = options if isinstance(options, DerivedMetricsOptions) else DerivedMetricsOptions()
        rows = refresh_derived_metrics(store, resolved)
        collisions = _grid_bucket_collision_count(store, resolved.security_ids)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details={
                "metric_codes": list(resolved.metric_codes or ()),
                "grid_bucket_collisions": collisions,
            },
        )
