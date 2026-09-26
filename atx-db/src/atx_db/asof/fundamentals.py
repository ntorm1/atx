from __future__ import annotations

from .._fundamental_clock import EFFECTIVE_FUNDAMENTAL_POINTS_SQL
from ..derived_lineage import qualify_issuer_derived_page, registered_definition_hashes
from ._common import (
    DEFAULT_DB_PATH,
    Path,
    _normalize_strings,
    _normalize_symbols,
    _register_filter,
    connect,
    dt,
    end_of_day_asof_ts,
    pd,
)

# Ties within a filing date break on the FC1 clock, then the accession; fundamental_points
# has no row id, so the remaining row fields complete a total order (the load clock
# ``source_loaded_at`` never decides: a reload would change the pick).
FUNDAMENTALS_ASOF_SQL = f"""
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        f.*,
        row_number() OVER (
            PARTITION BY f.security_id, f.metric, f.period_start, f.period_end, f.unit
            ORDER BY f.as_of_date DESC,
                     f.available_at DESC,
                     f.accession_number DESC NULLS LAST,
                     f.source DESC,
                     f.taxonomy DESC NULLS LAST,
                     f.form DESC NULLS LAST,
                     f.fiscal_year DESC NULLS LAST,
                     f.fiscal_period DESC NULLS LAST,
                     f.value DESC NULLS LAST
        ) AS rn
    FROM {EFFECTIVE_FUNDAMENTAL_POINTS_SQL} f
    {{symbol_join}}
    {{metric_join}}
    CROSS JOIN params p
    WHERE f.period_end <= p.as_of_date
      AND f.as_of_date <= p.as_of_date
      AND f.available_at <= p.as_of_ts
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, metric, period_end
"""

FUNDAMENTAL_STATEMENTS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        p.*,
        row_number() OVER (
            PARTITION BY p.security_id, p.canonical_metric, p.period_start, p.period_end, p.unit
            ORDER BY p.as_of_date DESC,
                     p.available_at DESC NULLS LAST,
                     p.source_loaded_at DESC NULLS LAST,
                     p.statement_point_id DESC
        ) AS rn
    FROM fundamental_statement_points p
    {symbol_join}
    {metric_join}
    {statement_join}
    CROSS JOIN params prm
    WHERE p.period_end <= prm.as_of_date
      AND p.as_of_date <= prm.as_of_date
      AND p.available_at <= prm.as_of_ts
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, statement_type, canonical_metric, period_end
"""

FUNDAMENTAL_TTM_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        t.*,
        row_number() OVER (
            PARTITION BY t.security_id, t.canonical_metric, t.ttm_end_date, t.unit
            ORDER BY t.as_of_date DESC,
                     t.available_at DESC NULLS LAST,
                     t.source_loaded_at DESC NULLS LAST,
                     t.ttm_point_id DESC
        ) AS rn
    FROM fundamental_ttm_points t
    {symbol_join}
    {metric_join}
    {statement_join}
    CROSS JOIN params prm
    WHERE t.ttm_end_date <= prm.as_of_date
      AND t.as_of_date <= prm.as_of_date
      AND t.available_at <= prm.as_of_ts
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, statement_type, canonical_metric, ttm_end_date
"""

FUNDAMENTAL_PERIODS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        fp.*,
        row_number() OVER (
            PARTITION BY fp.period_group_id
            ORDER BY fp.as_of_date DESC,
                     fp.available_at DESC NULLS LAST,
                     fp.source_loaded_at DESC NULLS LAST,
                     fp.fundamental_period_id DESC
        ) AS rn
    FROM fundamental_periods fp
    {symbol_join}
    {period_type_join}
    CROSS JOIN params prm
    WHERE fp.period_end <= prm.as_of_date
      AND fp.as_of_date <= prm.as_of_date
      AND fp.available_at <= prm.as_of_ts
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, period_end, period_start
"""

SHARES_OUTSTANDING_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        s.*,
        row_number() OVER (
            PARTITION BY s.security_id, s.share_count_type
            ORDER BY s.effective_date DESC,
                     s.as_of_date DESC,
                     s.available_at DESC NULLS LAST,
                     s.source_loaded_at DESC NULLS LAST,
                     s.share_history_id DESC
        ) AS rn
    FROM shares_outstanding_history s
    {symbol_join}
    {share_type_join}
    CROSS JOIN params p
    WHERE s.effective_date <= p.as_of_date
      AND s.as_of_date <= p.as_of_date
      AND s.available_at <= p.as_of_ts
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, share_count_type
"""

def fundamentals_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            if _register_filter(store, "asof_fundamental_symbol_filter", "symbol", symbol_values):
                registered.append("asof_fundamental_symbol_filter")
                symbol_join = "JOIN asof_fundamental_symbol_filter sf ON sf.symbol = f.symbol"
            if _register_filter(store, "asof_fundamental_metric_filter", "metric", metric_values):
                registered.append("asof_fundamental_metric_filter")
                metric_join = "JOIN asof_fundamental_metric_filter mf ON mf.metric = upper(f.metric)"
            sql = FUNDAMENTALS_ASOF_SQL.format(symbol_join=symbol_join, metric_join=metric_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def fundamental_statements_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
    statement_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    statement_values = _normalize_strings(statement_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            statement_join = ""
            if _register_filter(store, "asof_statement_symbol_filter", "symbol", symbol_values):
                registered.append("asof_statement_symbol_filter")
                symbol_join = "JOIN asof_statement_symbol_filter sf ON sf.symbol = p.symbol"
            if _register_filter(store, "asof_statement_metric_filter", "canonical_metric", metric_values):
                registered.append("asof_statement_metric_filter")
                metric_join = "JOIN asof_statement_metric_filter mf ON mf.canonical_metric = upper(p.canonical_metric)"
            if _register_filter(store, "asof_statement_type_filter", "statement_type", statement_values):
                registered.append("asof_statement_type_filter")
                statement_join = "JOIN asof_statement_type_filter stf ON stf.statement_type = upper(p.statement_type)"
            sql = FUNDAMENTAL_STATEMENTS_ASOF_SQL.format(
                symbol_join=symbol_join,
                metric_join=metric_join,
                statement_join=statement_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def fundamental_ttm_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
    statement_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    statement_values = _normalize_strings(statement_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            statement_join = ""
            if _register_filter(store, "asof_ttm_symbol_filter", "symbol", symbol_values):
                registered.append("asof_ttm_symbol_filter")
                symbol_join = "JOIN asof_ttm_symbol_filter sf ON sf.symbol = t.symbol"
            if _register_filter(store, "asof_ttm_metric_filter", "canonical_metric", metric_values):
                registered.append("asof_ttm_metric_filter")
                metric_join = "JOIN asof_ttm_metric_filter mf ON mf.canonical_metric = upper(t.canonical_metric)"
            if _register_filter(store, "asof_ttm_statement_type_filter", "statement_type", statement_values):
                registered.append("asof_ttm_statement_type_filter")
                statement_join = "JOIN asof_ttm_statement_type_filter stf ON stf.statement_type = upper(t.statement_type)"
            sql = FUNDAMENTAL_TTM_ASOF_SQL.format(
                symbol_join=symbol_join,
                metric_join=metric_join,
                statement_join=statement_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def fundamental_periods_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    normalized_period_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    period_type_values = _normalize_strings(normalized_period_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            period_type_join = ""
            if _register_filter(store, "asof_period_symbol_filter", "symbol", symbol_values):
                registered.append("asof_period_symbol_filter")
                symbol_join = "JOIN asof_period_symbol_filter sf ON sf.symbol = fp.symbol"
            if _register_filter(store, "asof_period_type_filter", "normalized_period_type", period_type_values):
                registered.append("asof_period_type_filter")
                period_type_join = (
                    "JOIN asof_period_type_filter ptf "
                    "ON ptf.normalized_period_type = upper(fp.normalized_period_type)"
                )
            sql = FUNDAMENTAL_PERIODS_ASOF_SQL.format(
                symbol_join=symbol_join,
                period_type_join=period_type_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def shares_outstanding_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    share_count_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    share_type_values = _normalize_strings(share_count_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            share_type_join = ""
            if _register_filter(store, "asof_shares_symbol_filter", "symbol", symbol_values):
                registered.append("asof_shares_symbol_filter")
                symbol_join = "JOIN asof_shares_symbol_filter sf ON sf.symbol = s.symbol"
            if _register_filter(store, "asof_shares_type_filter", "share_count_type", share_type_values):
                registered.append("asof_shares_type_filter")
                share_type_join = (
                    "JOIN asof_shares_type_filter stf "
                    "ON stf.share_count_type = upper(s.share_count_type)"
                )
            sql = SHARES_OUTSTANDING_ASOF_SQL.format(
                symbol_join=symbol_join,
                share_type_join=share_type_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)


# Issuer-content reads are deliberately separate from symbol/security resolution.
# A CIK identifies Company Facts content; it does not prove a historical listing or
# share class.  The owner set is discovered from source rows at the content clock,
# because a CIK can legitimately have more than one raw owner across vintages.
def _normalize_cik(cik: str) -> str:
    value = cik.strip().upper().removeprefix("CIK").strip()
    if not value.isdigit():
        raise ValueError("cik must contain only digits")
    if len(value) > 10:
        raise ValueError("cik must contain at most 10 digits")
    return value.zfill(10)


def _normalized_cik_sql(column: str) -> str:
    """Normalize one-to-ten digit source CIKs without truncating malformed values."""
    return (
        f"CASE WHEN regexp_full_match(trim({column}), '^[0-9]{{1,10}}$') "
        f"THEN lpad(trim({column}), 10, '0') END"
    )


def issuer_owner_ciks_asof(
    cik: str,
    content_as_of: dt.datetime,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> dict[str, tuple[str, ...]]:
    """Return visible normalized CIKs for every owner selected by ``cik``.

    A CIK-less relation such as derived metrics is safe only after this complete
    content-clock owner set proves that the owner belongs to no other CIK.
    """
    normalized = _normalize_cik(cik)
    cik_sql = _normalized_cik_sql("cik")
    with connect(db_path, read_only=True) as store:
        rows = store.con.execute(
            f"""
            WITH visible AS (
                SELECT security_id, {cik_sql} AS normalized_cik
                FROM fundamental_fact_revisions
                WHERE coalesce(available_at, source_loaded_at) <= ?
                  AND coalesce(as_of_date, CAST(coalesce(available_at, source_loaded_at) AS DATE))
                      <= CAST(? AS DATE)
            ), selected AS (
                SELECT DISTINCT security_id FROM visible WHERE normalized_cik = ?
            )
            SELECT DISTINCT visible.security_id, visible.normalized_cik
            FROM visible JOIN selected USING (security_id)
            WHERE visible.normalized_cik IS NOT NULL
            ORDER BY visible.security_id, visible.normalized_cik
            """,
            [content_as_of, content_as_of, normalized],
        ).fetchall()
    owner_ciks: dict[str, list[str]] = {}
    for owner, owner_cik in rows:
        owner_ciks.setdefault(str(owner), []).append(str(owner_cik))
    return {owner: tuple(sorted(set(ciks))) for owner, ciks in owner_ciks.items()}


def issuer_owner_ids_asof(
    cik: str,
    content_as_of: dt.datetime,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> tuple[str, ...]:
    """Return actual visible source owners without inventing an owner namespace."""
    return tuple(issuer_owner_ciks_asof(cik, content_as_of, db_path))


def _issuer_content_asof(
    *,
    table: str,
    cik: str,
    content_as_of: dt.datetime,
    time_column: str,
    partition: str,
    revision_order: str | None = None,
    db_path: Path | str,
    owner_ids: tuple[str, ...] | None = None,
) -> pd.DataFrame:
    """Read one CIK-owned relation without consulting the current directory."""
    normalized = _normalize_cik(cik)
    owners = owner_ids if owner_ids is not None else issuer_owner_ids_asof(normalized, content_as_of, db_path)
    if not owners:
        return pd.DataFrame()
    placeholders = ",".join("?" for _ in owners)
    cik_sql = _normalized_cik_sql("b.cik")
    order = revision_order or "coalesce(b.available_at, b.source_loaded_at) DESC, b.source_loaded_at DESC"
    with connect(db_path, read_only=True) as store:
        return store.con.execute(
            f"""
            WITH visible AS (
                SELECT b.*,
                       row_number() OVER (
                           PARTITION BY {partition}
                           ORDER BY {order}
                       ) AS _revision_rank
                FROM {table} b
                WHERE {cik_sql} = ?
                  AND b.security_id IN ({placeholders})
                  AND coalesce(b.available_at, b.source_loaded_at) <= ?
                  AND coalesce(b.as_of_date, CAST(coalesce(b.available_at, b.source_loaded_at) AS DATE))
                      <= CAST(? AS DATE)
            )
            SELECT * EXCLUDE (_revision_rank)
            FROM visible
            WHERE _revision_rank = 1
            ORDER BY {time_column}, security_id
            """,
            [normalized, *owners, content_as_of, content_as_of],
        ).df()


def issuer_statements_asof(cik: str, content_as_of: dt.datetime, db_path: Path | str = DEFAULT_DB_PATH) -> pd.DataFrame:
    return _issuer_content_asof(table="fundamental_statement_points", cik=cik, content_as_of=content_as_of,
        time_column="period_end", partition="b.revision_group_id", db_path=db_path)


def issuer_ttm_asof(cik: str, content_as_of: dt.datetime, db_path: Path | str = DEFAULT_DB_PATH) -> pd.DataFrame:
    return _issuer_content_asof(table="fundamental_ttm_points", cik=cik, content_as_of=content_as_of,
        time_column="ttm_end_date", partition="b.ttm_revision_group_id", db_path=db_path)


def issuer_standardized_asof(cik: str, content_as_of: dt.datetime, db_path: Path | str = DEFAULT_DB_PATH) -> pd.DataFrame:
    return _issuer_content_asof(table="fundamental_standardized", cik=cik, content_as_of=content_as_of,
        time_column="period_end", partition="b.security_id, b.item_id, b.basis, b.period_end", db_path=db_path)


def issuer_ratios_asof(cik: str, content_as_of: dt.datetime, db_path: Path | str = DEFAULT_DB_PATH) -> pd.DataFrame:
    return _issuer_content_asof(table="fundamental_ratios", cik=cik, content_as_of=content_as_of,
        time_column="period_end", partition="b.security_id, b.ratio_code, b.basis, b.period_end", db_path=db_path)


def issuer_shares_asof(cik: str, content_as_of: dt.datetime, db_path: Path | str = DEFAULT_DB_PATH) -> pd.DataFrame:
    return _issuer_content_asof(table="shares_outstanding_history", cik=cik, content_as_of=content_as_of,
        time_column="effective_date", partition="b.security_id, b.share_count_type, b.effective_date, b.accession_number", db_path=db_path,
        revision_order=("b.effective_date DESC, b.as_of_date DESC, coalesce(b.available_at, b.source_loaded_at) DESC, "
                        "b.source_loaded_at DESC, b.share_history_id DESC"))


def issuer_derived_asof(cik: str, content_as_of: dt.datetime, db_path: Path | str = DEFAULT_DB_PATH) -> pd.DataFrame:
    """Read bounded visible states with selected source leaves owned by ``cik``."""
    normalized = _normalize_cik(cik)
    owner_ciks = issuer_owner_ciks_asof(normalized, content_as_of, db_path)
    owners = tuple(owner for owner, ciks in owner_ciks.items() if ciks == (normalized,))
    excluded_owners = tuple(owner for owner, ciks in owner_ciks.items() if ciks != (normalized,))
    if not owners:
        result = pd.DataFrame()
        result.attrs["issuer_owner_status"] = (
            "ambiguous_owner_cik_collision" if excluded_owners else "unresolved_no_visible_source_owner"
        )
        result.attrs["excluded_derived_owner_ids"] = excluded_owners
        return result
    placeholders = ",".join("?" for _ in owners)
    query = f"""
        WITH visible AS (
            SELECT d.*, row_number() OVER (
                PARTITION BY coalesce(d.revision_group_id, d.derived_value_id)
                ORDER BY coalesce(d.available_at, d.source_loaded_at) DESC,
                         d.derived_value_id DESC
            ) AS _revision_rank
            FROM derived_metric_values d
            WHERE d.security_id IN ({placeholders})
              AND coalesce(d.available_at, d.source_loaded_at) <= ?
              AND coalesce(d.as_of_date, CAST(coalesce(d.available_at, d.source_loaded_at) AS DATE))
                  <= CAST(? AS DATE)
        )
        SELECT * EXCLUDE (_revision_rank) FROM visible
        WHERE _revision_rank = 1
        ORDER BY period_end, security_id, metric_code, derived_value_id
        LIMIT ? OFFSET ?
    """
    common_params = [*owners, content_as_of, content_as_of]
    frames: list[pd.DataFrame] = []
    result_columns: list[str] = []
    diagnostics: list[dict[str, object]] = []
    rejected = 0
    scanned = 0
    retained_bytes = 0
    max_rows = 50_000
    max_scan = 200_000
    max_bytes = 16 * 1024 * 1024
    scan_limited = False
    with connect(db_path, read_only=True) as store:
        hashes = registered_definition_hashes(store.con)
        while scanned < max_scan and sum(len(frame) for frame in frames) < max_rows:
            size = min(64, max_scan - scanned)
            params = [*common_params, size, scanned]
            # SELECT * includes retained JSON refs. Bound each page in SQL before
            # converting it into Python or a DataFrame.
            lengths = store.con.execute(
                f"SELECT count(*), coalesce(max(octet_length(encode(to_json(page)))), 0), "
                f"coalesce(sum(octet_length(encode(to_json(page)))), 0) "
                f"FROM ({query}) page", params,
            ).fetchone()
            count, largest, page_bytes = (int(value or 0) for value in lengths)
            if not count:
                empty_page = store.con.execute(query, params)
                result_columns = [column[0] for column in empty_page.description]
                break
            if largest > 1_048_576 or retained_bytes + page_bytes > max_bytes:
                raise ValueError("issuer derived as-of output byte limit exceeded")
            retained_bytes += page_bytes
            page = store.con.execute(query, params).df()
            if not result_columns:
                result_columns = list(page.columns)
            candidates = [{
                "derived_value_id": row.derived_value_id,
                "period_end": row.period_end,
                "value": None if pd.isna(row.value) else row.value,
                "value_status": row.value_status,
            } for row in page.itertuples(index=False)]
            accepted, rejected_page = qualify_issuer_derived_page(
                store.con, candidates, expected_cik=normalized,
                expected_definition_hashes=hashes,
            )
            rejected += len(rejected_page)
            diagnostics.extend(rejected_page[:max(0, 128 - len(diagnostics))])
            if any(accepted):
                frames.append(page.loc[accepted])
            scanned += count
            if count < size:
                break
        else:
            # A cap alone does not prove truncation. Probe one SQL-bounded row
            # without materializing its potentially large JSON in Python.
            scan_limited = bool(store.con.execute(
                f"SELECT count(*) FROM ({query}) page",
                [*common_params, 1, scanned],
            ).fetchone()[0])
    result = pd.concat(frames, ignore_index=True) if frames else pd.DataFrame(columns=result_columns)
    if len(result) > max_rows:
        result = result.iloc[:max_rows].copy()
        scan_limited = True
    result.attrs["issuer_owner_status"] = (
        "ambiguous_owner_cik_collision" if excluded_owners else "resolved_single_owner"
    )
    result.attrs["excluded_derived_owner_ids"] = excluded_owners
    result.attrs["derived_lineage_rejected_count"] = rejected
    result.attrs["derived_lineage_diagnostics"] = diagnostics
    result.attrs["derived_lineage_diagnostics_truncated"] = rejected > len(diagnostics)
    result.attrs["derived_lineage_scanned_count"] = scanned
    result.attrs["derived_lineage_scan_limited"] = scan_limited
    return result
