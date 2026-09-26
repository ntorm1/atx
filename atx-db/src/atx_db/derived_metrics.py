"""Quarterly filing-event states evaluated in bounded local SQL frames."""

from __future__ import annotations

import re
from collections.abc import Generator
from contextlib import closing
from dataclasses import dataclass
from typing import Any
from uuid import uuid4

from . import _derived_annual as annual
from . import _derived_pit as pit
from . import _split_epochs
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

# Bound retained publication/index state across complete security commits,
# matching the conservative companyfacts cadence at the production budget.
_DERIVED_REOPEN_SECURITIES = 10


def _validate_identifier_code(code: str) -> None:
    if not _IDENTIFIER_CODE_RE.fullmatch(code):
        raise ValueError(
            f"code {code!r} is not safe to use as a SQL identifier; must match {_IDENTIFIER_CODE_RE.pattern}"
        )


@dataclass(frozen=True)
class DerivedMetricsOptions:
    source: str = DERIVED_SOURCE_NAME
    security_ids: tuple[str, ...] | None = None
    metric_codes: tuple[str, ...] | None = None
    batch_size: int = 1
    event_chunk_size: int = 1024
    max_frame_rows: int = 8192
    max_input_rows: int = 250000
    max_candidate_rows: int = 250000
    max_scope_rows: int = 100000
    max_selected_ref_bytes: int = 32768
    max_selected_ref_count: int = 128
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
        columns[code] = f'b."{code}"'
        availability[code] = f'b."{code}__at"'
    _ = definition
    return LowerContext(
        grid="quarter",
        columns=columns,
        availability=availability,
        partition_sql="b.security_id, b.key_number",
        # Calendar-dense local frame position; gaps occupy real window rows.
        order_sql="b.rn",
    )


def build_metric_sql(
    definition: DerivedMetricDefinition,
    *,
    lowered: Lowered,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
    security_count: int,
    annual_plan: annual.AnnualPlan = annual.EMPTY_PLAN,
) -> str:
    """Build a bounded event-frame INSERT into private staging.

    The caller prepares _pit_inputs/_pit_keys. Bind key start/end, source, run_id.
    ``security_count`` remains in the public builder signature for compatibility;
    publication deliberately handles one complete security scope at a time.
    """
    _ = security_count
    context = quarterly_context(definition, item_codes=item_codes, metric_codes=metric_codes)
    return pit.frame_sql(definition, lowered, context, annual_plan)

def select_security_batches(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> Generator[tuple[str, ...], None, None]:
    """Yield bounded pages; close an unfinished iterator to release its snapshot."""
    options = options or DerivedMetricsOptions()
    size = max(1, min(500, int(options.batch_size)))
    if options.security_ids is not None:
        identifiers = sorted(set(options.security_ids))
        for start in range(0, len(identifiers), size):
            yield tuple(identifiers[start:start + size])
    else:
        # Enumerate fact/output IDs once. Only this small ID relation is paged;
        # its persistent form survives configured publication reopens. Other
        # callers retain a session-local snapshot, including in-memory stores.
        persistent = _can_reopen_derived_store(store)
        catalog = "temp"
        if persistent:
            database = store.con.execute("SELECT current_database()").fetchone()
            assert database is not None
            catalog = str(database[0]).replace('"', '""')
        name = f"_derived_security_ids_{uuid4().hex}"
        relation = f'"{catalog}"."main"."{name}"'
        create_name = relation if persistent else f'"{name}"'
        kind = "" if persistent else "TEMP "
        created = False
        try:
            # No REPLACE/IF NOT EXISTS: a name collision cannot claim ownership
            # of another caller's table. Cleanup starts only after CREATE works.
            store.con.execute(
                f"""CREATE {kind}TABLE {create_name} AS
                SELECT security_id
                FROM fundamental_standardized
                WHERE basis IN ('quarterly', 'instant', 'annual')
                UNION
                SELECT security_id FROM derived_metric_values WHERE source = ?
                ORDER BY security_id
                """, [options.source]
            )
            created = True
            after: str | None = None
            while True:
                boundary = "" if after is None else "WHERE security_id > ?"
                boundary_params = [] if after is None else [after]
                rows = store.con.execute(
                    f"SELECT security_id FROM {relation} {boundary} ORDER BY security_id LIMIT ?",
                    [*boundary_params, size],
                ).fetchall()
                if not rows:
                    return
                after = str(rows[-1][0])
                yield tuple(str(row[0]) for row in rows)
        finally:
            if created:
                store.con.execute(f"DROP TABLE {relation}")


def _can_reopen_derived_store(store: DuckDBStore) -> bool:
    # Match CF5 eligibility: reopening an in-memory database destroys it, and
    # unrecorded analytical settings cannot safely be replayed by the store.
    return (
        not str(store.path).startswith(":memory:") and store.path.is_file()
        and store.analytical_memory_limit is not None and store.analytical_threads is not None
    )


def _validate_derived_reopen_state(store: DuckDBStore) -> None:
    temporary = store.con.execute("""SELECT EXISTS (
        SELECT 1 FROM duckdb_tables() WHERE temporary AND NOT internal
    ) OR EXISTS (SELECT 1 FROM duckdb_views() WHERE temporary AND NOT internal)""").fetchone()
    if temporary is None or temporary[0]:
        raise RuntimeError("derived metrics cannot bound connection lifetime with caller-owned temporary objects")


def _reopen_derived_store(store: DuckDBStore) -> None:
    # PIT cleanup and the security COMMIT must have completed before this call.
    # Recheck so unexpected temporary state is never silently discarded.
    _validate_derived_reopen_state(store)
    store.close()  # CHECKPOINT, then release retained index state.
    store.reopen()  # Replays the store's recorded analytical configuration.


def _expand_metric_codes(requested: tuple[str, ...], by_code: dict[str, DerivedMetricDefinition]) -> frozenset[str]:
    """Close prerequisites and descendants until no rebuilt dependency is stale.

    Rebuilding a prerequisite can change its history, so its other consumers
    must also be rebuilt. A connected metric family may consequently be larger
    than the requested set. Values still execute in bounded per-metric frames.
    """
    unknown = set(requested) - by_code.keys()
    if unknown:
        raise ValueError(f"unknown quarterly metric codes: {sorted(unknown)}")
    expanded = set(requested)
    while True:
        descendants = {d.metric_code for d in by_code.values() if expanded.intersection(d.metric_inputs)}
        prerequisites = {code for name in expanded for code in by_code[name].metric_inputs}
        additions = descendants | prerequisites
        if additions <= expanded:
            break
        expanded.update(additions)
    return frozenset(expanded)


def refresh_derived_metrics(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> int:
    """Reconstruct modeled filing-event history; atomically replace each security.

    Each security is the publication/rollback scope. On failure earlier complete
    securities remain committed, the failing security retains its old complete
    scope, and the call raises (never reports a partial run as success). Staging
    and candidate limits fail before deleting canonical rows. SQL owns all data;
    Python holds definitions, bounded identifier pages and counts only. Configured
    persistent stores recycle every ten complete scopes and at the final partial
    group. In-memory and unconfigured callers retain their existing session.
    """
    options = options or DerivedMetricsOptions()
    for name in ("event_chunk_size", "max_frame_rows", "max_input_rows", "max_candidate_rows", "max_scope_rows",
                 "max_selected_ref_bytes", "max_selected_ref_count"):
        if getattr(options, name) < 1:
            raise ValueError(f"{name} must be positive")
    recycle = _can_reopen_derived_store(store)
    if recycle:
        # Refuse before initialize/staging can mutate or replace caller state,
        # including objects whose names overlap our owned PIT relations.
        _validate_derived_reopen_state(store)
    store.initialize()
    definitions = default_derived_definitions()
    validate_definitions(definitions, item_codes=known_item_codes())
    quarterly = tuple(d for d in definitions if d.window != "daily")
    by_code = {d.metric_code: d for d in quarterly}
    wanted = None if options.metric_codes is None else _expand_metric_codes(options.metric_codes, by_code)
    ordered = tuple(d for d in topological_order(quarterly) if wanted is None or d.metric_code in wanted)
    if not ordered:
        return 0
    codes = tuple(d.metric_code for d in ordered)
    predicate = "source = ? AND security_id = ?"
    if options.metric_codes is not None:
        predicate += f" AND metric_code IN ({', '.join('?' for _ in codes)})"
    inserted = committed_since_reopen = 0
    # Split epochs are staged in one pass over the bars for the whole refresh, keyed by
    # accounting id through the owner bridge's single-class links (R1e).
    links = _split_epochs.bridge_links(store, item_codes=known_item_codes(), derived_source=options.source)
    with _split_epochs.refresh_scope(store, options.security_ids, persistent=recycle, links=links), \
            closing(select_security_batches(store, options)) as batches:
        for batch in batches:
            for security_id in batch:
                params: list[Any] = [options.source, security_id]
                if options.metric_codes is not None:
                    params.extend(codes)
                try:
                    pit.prepare_security(store.con, security_id, options.max_input_rows)
                    for definition in ordered:
                        annual_plan = annual.plan_for(definition, by_code)
                        item_codes = tuple(sorted(definition.item_inputs))
                        metric_codes = tuple(sorted(definition.metric_inputs))
                        context = quarterly_context(definition, item_codes=item_codes, metric_codes=metric_codes)
                        lowered = compile_expression(definition.expression, context)
                        width = lowered.max_lag + 1
                        if width > options.max_frame_rows:
                            raise RuntimeError(f"derived PIT frame width {width} exceeds limit {options.max_frame_rows}")
                        # Bound candidate generation before constructing its offset
                        # expansion, even for unusually large user-defined windows.
                        input_count_row = store.con.execute(
                            "SELECT count(*) FROM _pit_items WHERE code IN (" +
                            ",".join(pit.quote(c) for c in item_codes or ("",)) + ")"
                        ).fetchone()
                        assert input_count_row is not None
                        input_events = int(input_count_row[0])
                        dependency_count_row = store.con.execute(
                            "SELECT count(*) FROM _pit_stage WHERE metric_code IN (" +
                            ",".join(pit.quote(c) for c in metric_codes or ("",)) + ")"
                        ).fetchone()
                        assert dependency_count_row is not None
                        dependency_events = int(dependency_count_row[0])
                        target_count_row = store.con.execute("SELECT count(*) FROM _pit_targets").fetchone()
                        assert target_count_row is not None
                        targets = int(target_count_row[0])
                        annual_events = 0
                        if annual_plan.codes:
                            annual_count_row = store.con.execute(
                                "SELECT count(*) FROM _pit_annual_items WHERE code IN (" +
                                ",".join(pit.quote(c) for c in annual_plan.codes) + ")"
                            ).fetchone()
                            assert annual_count_row is not None
                            annual_events = int(annual_count_row[0])
                            annual_target_count_row = store.con.execute("SELECT count(*) FROM _pit_annual_targets").fetchone()
                            assert annual_target_count_row is not None
                            annual_events += int(annual_target_count_row[0])
                        if (input_events + dependency_events) * width + targets + annual_events > options.max_candidate_rows:
                            raise RuntimeError(f"derived PIT candidate upper bound exceeded for {security_id}/{definition.metric_code}")
                        count = pit.prepare_metric(store.con, definition, lowered.max_lag,
                                                   options.max_candidate_rows, annual_plan)
                        chunk_size = min(options.event_chunk_size, options.max_frame_rows // width)
                        sql = build_metric_sql(definition, lowered=lowered, item_codes=item_codes,
                                               metric_codes=metric_codes, security_count=1, annual_plan=annual_plan)
                        for start in range(1, count + 1, chunk_size):
                            store.con.execute(sql, [start, start + chunk_size - 1, options.source, options.run_id])
                        pit.finish_metric(store.con)
                        pit.enforce_count(store.con, "_pit_stage", options.max_scope_rows, "publication scope")
                    staged = pit.enforce_count(store.con, "_pit_stage", options.max_scope_rows, "publication scope")
                    oversize = store.con.execute("""
                        SELECT derived_value_id, octet_length(encode(selected_input_refs_json)),
                               json_array_length(selected_input_refs_json, '$.refs')
                        FROM _pit_stage
                        WHERE selected_input_refs_json IS NULL OR selected_input_refs_hash IS NULL
                           OR octet_length(encode(selected_input_refs_json)) > ?
                           OR json_array_length(selected_input_refs_json, '$.refs') > ?
                        LIMIT 1
                    """, [options.max_selected_ref_bytes, options.max_selected_ref_count]).fetchone()
                    if oversize is not None:
                        raise RuntimeError(f"derived PIT selected-ref row exceeds bounded limit: {oversize}")
                    old_count_row = store.con.execute(
                        f"SELECT count(*) FROM derived_metric_values WHERE {predicate}", params
                    ).fetchone()
                    assert old_count_row is not None
                    old_count = int(old_count_row[0])
                    if old_count > options.max_scope_rows:
                        raise RuntimeError(f"derived PIT old publication scope exceeds limit: {old_count}")
                    store.con.execute("BEGIN TRANSACTION")
                    try:
                        store.con.execute(f"DELETE FROM derived_metric_values WHERE {predicate}", params)
                        columns = ', '.join(pit.STATE_COLUMNS)
                        store.con.execute(f"INSERT INTO derived_metric_values ({columns}) SELECT {columns} "
                                          "FROM _pit_stage ORDER BY derived_value_id")
                        store.con.execute("COMMIT")
                    except Exception:
                        # Include COMMIT failures: DuckDB can exhaust its bounded
                        # allocation budget while publishing required PK entries.
                        store.con.execute("ROLLBACK")
                        raise
                    inserted += staged
                finally:
                    pit.cleanup(store.con)
                committed_since_reopen += 1
                if recycle and committed_since_reopen >= _DERIVED_REOPEN_SECURITIES:
                    _reopen_derived_store(store)
                    committed_since_reopen = 0
    if recycle and committed_since_reopen:
        _reopen_derived_store(store)
    return inserted

def _grid_bucket_collision_count(store: DuckDBStore, security_ids: tuple[str, ...] | None) -> int:
    """Count (security_id, canonical_code, bucket) groups with more than one observed period_end.

    Mirrors the historical bucket precedence in the event-state writer:
    a count above zero means at least one security had two distinct fiscal
    period-ends (typically a fiscal-year-end change producing a stub period)
    land in the same calendar quarter bucket. At each event the writer selects
    the latest period_end visible then, without using future stub dates.
    This is purely observability -- it does not change engine behavior -- and is
    surfaced via :class:`DerivedMetricsDataset`'s ``details``.
    """
    predicate = ""
    params: list[Any] = []
    if security_ids == ():
        return 0
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
                WHERE basis IN ('quarterly', 'instant')
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
