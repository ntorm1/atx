"""The quarterly derived-metric engine: one generated statement per metric."""

from __future__ import annotations

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
        columns[code] = f'b."{code}"'
        availability[code] = f'b."{code}__at"'
    for code in metric_codes:
        columns[code] = f'd."{code}"'
        availability[code] = f'd."{code}__at"'
    _ = definition
    return LowerContext(
        grid="quarter",
        columns=columns,
        availability=availability,
        partition_sql="b.security_id",
        order_sql="b.period_end",
    )


def _pivot_pairs(alias: str, codes: Iterable[str], key_column: str) -> str:
    fragments: list[str] = []
    for code in codes:
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
           max(available_at) AS available_at
    FROM facts
    GROUP BY 1, 2, 3
), grid AS (
    SELECT security_id, period_end,
           row_number() OVER (PARTITION BY security_id ORDER BY period_end) AS rn
    FROM (SELECT DISTINCT security_id, period_end FROM picked)
), base AS (
    SELECT g.security_id, g.period_end, g.rn,
           {_pivot_pairs("p", item_codes, "canonical_code")}
    FROM grid g
    LEFT JOIN picked p ON p.security_id = g.security_id AND p.period_end = g.period_end
    GROUP BY 1, 2, 3
), {deps_cte}, hash_parts AS (
    SELECT t.security_id, t.period_end,
           l.canonical_code || '|' || CAST(l.period_end AS VARCHAR) || '|' ||
           CAST(l.revision_sequence AS VARCHAR) || '|' || CAST(l.value AS VARCHAR) AS payload
    FROM grid t
    JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - {lowered.max_lag} AND t.rn
    JOIN picked l ON l.security_id = lg.security_id AND l.period_end = lg.period_end
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


def _wanted(definition: DerivedMetricDefinition, options: DerivedMetricsOptions) -> bool:
    return options.metric_codes is None or definition.metric_code in set(options.metric_codes)


def refresh_derived_metrics(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> int:
    options = options or DerivedMetricsOptions()
    store.initialize()
    definitions = default_derived_definitions()
    validate_definitions(definitions, item_codes=known_item_codes())
    quarterly = tuple(d for d in definitions if d.window != "daily")
    ordered = tuple(d for d in topological_order(quarterly) if _wanted(d, options))
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


class DerivedMetricsDataset(Dataset):
    dataset_id = "derived_metrics"
    source_name = DERIVED_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        resolved = options if isinstance(options, DerivedMetricsOptions) else DerivedMetricsOptions()
        rows = refresh_derived_metrics(store, resolved)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details={"metric_codes": list(resolved.metric_codes or ())},
        )
