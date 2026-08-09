"""PF2-S6: fiscal-to-calendar mapping and calendar-aligned TTM surfaces."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "derived_calendarization_v1"
FISCAL_TTM_METHOD = "sum_four_visible_quarter_like_statement_points_with_ytd_quarter_derivations"
STITCHED_TTM_METHOD = "stitched_quarterly_ttm"


CALENDAR_MAP_COLUMNS = [
    "calendar_map_id",
    "source",
    "upstream_source",
    "fundamental_period_id",
    "period_group_id",
    "security_id",
    "symbol",
    "cik",
    "accession_number",
    "period_start",
    "period_end",
    "normalized_period_type",
    "fyr",
    "period_length_days",
    "week_count",
    "is_53_week",
    "reported_fiscal_year",
    "reported_fiscal_period",
    "fiscal_scheme_year",
    "fiscal_scheme_quarter",
    "fiscal_scheme_period",
    "containing_calendar_year",
    "containing_calendar_quarter",
    "containing_calendar_period",
    "greatest_overlap_calendar_year",
    "greatest_overlap_calendar_quarter",
    "greatest_overlap_calendar_period",
    "as_of_date",
    "available_at",
    "is_latest_revision",
    "run_id",
    "source_loaded_at",
]


@dataclass(frozen=True)
class CalendarizationOptions:
    source: str = SOURCE_NAME
    run_id: str | None = None


def _stable_id(*parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _as_date(value: object) -> dt.date | None:
    if value is None or value is pd.NA:
        return None
    try:
        if pd.isna(value):
            return None
    except TypeError:
        pass
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _first_json_value(value: object) -> object | None:
    if value is None or value is pd.NA:
        return None
    try:
        if pd.isna(value):
            return None
    except TypeError:
        pass
    if isinstance(value, (list, tuple)):
        return value[0] if value else None
    text = str(value).strip()
    if not text:
        return None
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return text
    if isinstance(parsed, list):
        return parsed[0] if parsed else None
    return parsed


def calendar_quarter(month: int) -> int:
    if month < 1 or month > 12:
        raise ValueError(f"month must be 1..12, got {month!r}")
    return ((month - 1) // 3) + 1


def calendar_period_label(year: int, quarter: int) -> str:
    return f"{int(year)}Q{int(quarter)}"


def fyr_from_period_end(period_end: dt.date) -> int:
    return int(period_end.month)


def fiscal_year_label(period_end: dt.date, fyr: int | None = None) -> int:
    """Compustat FYR rule: Jan-May fiscal year ends label the prior calendar year."""

    resolved_fyr = int(fyr if fyr is not None else fyr_from_period_end(period_end))
    if not 1 <= resolved_fyr <= 12:
        raise ValueError(f"fyr must be 1..12, got {fyr!r}")
    return int(period_end.year - 1 if resolved_fyr <= 5 else period_end.year)


def period_length_days(period_start: dt.date | None, period_end: dt.date) -> int | None:
    if period_start is None:
        return None
    return int((period_end - period_start).days + 1)


def period_week_count(length_days: int | None) -> int | None:
    if length_days is None:
        return None
    return int(round(length_days / 7.0))


def is_53_week_period(
    length_days: int | None,
    normalized_period_type: str | None = None,
) -> bool:
    if length_days is None:
        return False
    period_type = (normalized_period_type or "").lower()
    if period_type == "quarter":
        return length_days > 91
    if period_type == "semiannual_ytd":
        return length_days > 182
    if period_type == "multi_quarter_ytd":
        return length_days > 273
    if period_type == "annual":
        return length_days > 364
    return length_days >= 371


def _midpoint_date(period_start: dt.date | None, period_end: dt.date) -> dt.date:
    if period_start is None:
        return period_end
    length = period_length_days(period_start, period_end)
    if length is None:
        return period_end
    return period_start + dt.timedelta(days=max((length - 1) // 2, 0))


def _reported_fiscal_quarter(reported_period: object, fallback_end: dt.date) -> int:
    text = "" if reported_period is None else str(reported_period).upper()
    for quarter in (1, 2, 3, 4):
        if f"Q{quarter}" in text:
            return quarter
    if text in {"FY", "CY", "Y", "YEAR", "ANNUAL"}:
        return 4
    return calendar_quarter(fallback_end.month)


def compute_calendar_map_rows(
    periods: pd.DataFrame,
    *,
    source: str = SOURCE_NAME,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Return deterministic calendar-map rows for fundamental_periods-like input."""

    records: list[dict[str, object]] = []
    for raw in periods.to_dict("records"):
        period_end = _as_date(raw.get("period_end"))
        if period_end is None:
            continue
        period_start = _as_date(raw.get("period_start"))
        try:
            fyr = int(raw.get("fyr") or fyr_from_period_end(period_end))
        except (TypeError, ValueError):
            fyr = fyr_from_period_end(period_end)
        length_days = period_length_days(period_start, period_end)
        weeks = period_week_count(length_days)
        normalized_period_type = raw.get("normalized_period_type")
        reported_year_value = _first_json_value(
            raw.get("reported_fiscal_years_json", raw.get("fiscal_year"))
        )
        try:
            reported_year = int(reported_year_value) if reported_year_value is not None else None
        except (TypeError, ValueError):
            reported_year = None
        reported_period = _first_json_value(
            raw.get("reported_fiscal_periods_json", raw.get("fiscal_period"))
        )

        fiscal_year = reported_year if reported_year is not None else fiscal_year_label(period_end, fyr)
        fiscal_quarter = _reported_fiscal_quarter(reported_period, period_end)
        fiscal_period = calendar_period_label(fiscal_year, fiscal_quarter)

        containing_year = int(period_end.year)
        containing_quarter = calendar_quarter(period_end.month)
        containing_period = calendar_period_label(containing_year, containing_quarter)

        quarter_anchor = period_end
        if length_days is not None and 70 <= length_days <= 120:
            quarter_anchor = _midpoint_date(period_start, period_end)
            overlap_year = int(quarter_anchor.year)
        else:
            overlap_year = fiscal_year_label(period_end, fyr)
        overlap_quarter = calendar_quarter(quarter_anchor.month)
        overlap_period = calendar_period_label(overlap_year, overlap_quarter)

        fundamental_period_id = raw.get("fundamental_period_id")
        upstream_source = raw.get("source")
        available_at = raw.get("available_at")
        source_loaded_at = raw.get("source_loaded_at") or available_at or pd.Timestamp("1970-01-01")
        records.append(
            {
                "calendar_map_id": _stable_id(
                    source,
                    upstream_source,
                    fundamental_period_id,
                    "calendar_map_v1",
                ),
                "source": source,
                "upstream_source": upstream_source,
                "fundamental_period_id": fundamental_period_id,
                "period_group_id": raw.get("period_group_id"),
                "security_id": raw.get("security_id"),
                "symbol": raw.get("symbol"),
                "cik": raw.get("cik"),
                "accession_number": raw.get("accession_number"),
                "period_start": period_start,
                "period_end": period_end,
                "normalized_period_type": normalized_period_type,
                "fyr": fyr,
                "period_length_days": length_days,
                "week_count": weeks,
                "is_53_week": is_53_week_period(length_days, str(normalized_period_type or "")),
                "reported_fiscal_year": reported_year,
                "reported_fiscal_period": None if reported_period is None else str(reported_period),
                "fiscal_scheme_year": fiscal_year,
                "fiscal_scheme_quarter": fiscal_quarter,
                "fiscal_scheme_period": fiscal_period,
                "containing_calendar_year": containing_year,
                "containing_calendar_quarter": containing_quarter,
                "containing_calendar_period": containing_period,
                "greatest_overlap_calendar_year": overlap_year,
                "greatest_overlap_calendar_quarter": overlap_quarter,
                "greatest_overlap_calendar_period": overlap_period,
                "as_of_date": _as_date(raw.get("as_of_date")),
                "available_at": available_at,
                "is_latest_revision": bool(raw.get("is_latest_revision", True)),
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )
    return pd.DataFrame.from_records(records, columns=CALENDAR_MAP_COLUMNS)


def refresh_fundamental_calendar_map(
    store: DuckDBStore,
    options: CalendarizationOptions | None = None,
) -> int:
    """Materialize fiscal-to-calendar labels for fundamental_periods."""

    options = options or CalendarizationOptions()
    periods = store.con.execute(
        """
        WITH issuer_fyr AS (
            SELECT
                source,
                security_id,
                arg_max(CAST(EXTRACT(MONTH FROM period_end) AS INTEGER), period_end) AS fyr
            FROM fundamental_periods
            WHERE normalized_period_type = 'annual'
              AND period_end IS NOT NULL
            GROUP BY 1, 2
        )
        SELECT
            fp.fundamental_period_id,
            fp.period_group_id,
            fp.source,
            fp.security_id,
            fp.symbol,
            fp.cik,
            fp.accession_number,
            fp.period_start,
            fp.period_end,
            fp.normalized_period_type,
            coalesce(issuer_fyr.fyr, CAST(EXTRACT(MONTH FROM fp.period_end) AS INTEGER)) AS fyr,
            fp.reported_fiscal_years_json,
            fp.reported_fiscal_periods_json,
            fp.as_of_date,
            fp.available_at,
            fp.is_latest_revision,
            fp.source_loaded_at
        FROM fundamental_periods fp
        LEFT JOIN issuer_fyr
          ON issuer_fyr.source = fp.source
         AND issuer_fyr.security_id = fp.security_id
        """
    ).df()
    rows = compute_calendar_map_rows(periods, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM fundamental_calendar_map WHERE source = ?", [options.source])
        insert_frame(store, rows, "fundamental_calendar_map", "_calendar_map_rows")
    return int(len(rows))


def refresh_fundamental_calendar_ttm(
    store: DuckDBStore,
    options: CalendarizationOptions | None = None,
) -> int:
    """Refresh calendar-aligned trailing-twelve-month statement values."""

    options = options or CalendarizationOptions()
    with store.transaction():
        store.con.execute("DELETE FROM fundamental_calendar_ttm WHERE source = ?", [options.source])
        store.con.execute(
            """
            INSERT INTO fundamental_calendar_ttm (
                calendar_ttm_id,
                calendar_ttm_revision_group_id,
                source,
                upstream_source,
                anchor_statement_point_id,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                unit,
                unit_type,
                calendar_year,
                calendar_quarter,
                calendar_period,
                calendar_period_end,
                ttm_start_date,
                ttm_end_date,
                as_of_date,
                available_at,
                quarter_count,
                coverage_days,
                is_complete,
                min_input_available_at,
                max_input_available_at,
                input_statement_point_ids_json,
                input_accessions_json,
                input_calendar_periods_json,
                ttm_value,
                previous_ttm_value,
                ttm_value_delta,
                ttm_value_delta_percent,
                revision_sequence,
                revision_count,
                is_latest_revision,
                is_value_changed,
                calculation_method,
                run_id,
                source_loaded_at
            )
            WITH statement_points AS (
                SELECT
                    statement_point_id,
                    revision_group_id,
                    source,
                    security_id,
                    symbol,
                    cik,
                    statement_type,
                    statement_section,
                    canonical_metric,
                    canonical_label,
                    unit,
                    unit_type,
                    period_start,
                    period_end,
                    as_of_date,
                    available_at,
                    fiscal_year,
                    fiscal_period,
                    form,
                    accession_number,
                    value,
                    revision_sequence,
                    source_loaded_at,
                    date_diff('day', period_start, period_end) + 1 AS period_days,
                    coalesce(available_at, CAST(as_of_date AS TIMESTAMP)) AS availability_ts
                FROM fundamental_statement_points
                WHERE period_type = 'duration'
                  AND period_start IS NOT NULL
                  AND period_end IS NOT NULL
                  AND value IS NOT NULL
            ),
            reported_quarter_points AS (
                SELECT
                    statement_point_id,
                    statement_point_id AS anchor_statement_point_id,
                    revision_group_id,
                    source,
                    security_id,
                    symbol,
                    cik,
                    statement_type,
                    statement_section,
                    canonical_metric,
                    canonical_label,
                    unit,
                    unit_type,
                    period_start,
                    period_end,
                    as_of_date,
                    available_at,
                    fiscal_year,
                    fiscal_period,
                    form,
                    accession_number,
                    value,
                    revision_sequence,
                    source_loaded_at,
                    period_days,
                    availability_ts,
                    1 AS quarter_source_priority
                FROM statement_points
                WHERE period_days BETWEEN 70 AND 115
            ),
            ytd_points AS (
                SELECT *
                FROM statement_points
                WHERE unit_type = 'monetary'
                  AND period_days BETWEEN 160 AND 380
            ),
            prior_ytd_points AS (
                SELECT *
                FROM statement_points
                WHERE unit_type = 'monetary'
                  AND period_days BETWEEN 70 AND 290
            ),
            derived_ytd_quarter_points AS (
                SELECT
                    sha256(concat_ws('|', 'derived_ytd_quarter', current_ytd.statement_point_id, prior_ytd.statement_point_id)) AS statement_point_id,
                    current_ytd.statement_point_id AS anchor_statement_point_id,
                    sha256(concat_ws('|', 'derived_ytd_quarter', current_ytd.revision_group_id, prior_ytd.revision_group_id)) AS revision_group_id,
                    current_ytd.source,
                    current_ytd.security_id,
                    current_ytd.symbol,
                    current_ytd.cik,
                    current_ytd.statement_type,
                    current_ytd.statement_section,
                    current_ytd.canonical_metric,
                    current_ytd.canonical_label,
                    current_ytd.unit,
                    current_ytd.unit_type,
                    CAST(prior_ytd.period_end + INTERVAL 1 DAY AS DATE) AS period_start,
                    current_ytd.period_end,
                    greatest(current_ytd.as_of_date, prior_ytd.as_of_date) AS as_of_date,
                    greatest(current_ytd.availability_ts, prior_ytd.availability_ts) AS available_at,
                    current_ytd.fiscal_year,
                    CASE
                        WHEN current_ytd.period_days BETWEEN 160 AND 205 THEN 'Q2_DERIVED'
                        WHEN current_ytd.period_days BETWEEN 250 AND 290 THEN 'Q3_DERIVED'
                        WHEN current_ytd.period_days BETWEEN 330 AND 380 THEN 'Q4_DERIVED'
                        ELSE 'Q_DERIVED'
                    END AS fiscal_period,
                    current_ytd.form,
                    current_ytd.accession_number,
                    current_ytd.value - prior_ytd.value AS value,
                    current_ytd.revision_sequence,
                    greatest(
                        coalesce(current_ytd.source_loaded_at, TIMESTAMP '1970-01-01'),
                        coalesce(prior_ytd.source_loaded_at, TIMESTAMP '1970-01-01')
                    ) AS source_loaded_at,
                    date_diff('day', CAST(prior_ytd.period_end + INTERVAL 1 DAY AS DATE), current_ytd.period_end) + 1 AS period_days,
                    greatest(current_ytd.availability_ts, prior_ytd.availability_ts) AS availability_ts,
                    2 AS quarter_source_priority
                FROM ytd_points current_ytd
                JOIN prior_ytd_points prior_ytd
                  ON prior_ytd.source = current_ytd.source
                 AND prior_ytd.security_id = current_ytd.security_id
                 AND prior_ytd.canonical_metric = current_ytd.canonical_metric
                 AND prior_ytd.unit = current_ytd.unit
                 AND prior_ytd.period_start = current_ytd.period_start
                 AND prior_ytd.period_end < current_ytd.period_end
                 AND prior_ytd.as_of_date <= current_ytd.as_of_date
                 AND prior_ytd.availability_ts <= current_ytd.availability_ts
                QUALIFY row_number() OVER (
                    PARTITION BY current_ytd.statement_point_id
                    ORDER BY
                        prior_ytd.period_end DESC,
                        prior_ytd.availability_ts DESC,
                        prior_ytd.as_of_date DESC,
                        coalesce(prior_ytd.source_loaded_at, TIMESTAMP '1970-01-01') DESC,
                        prior_ytd.revision_sequence DESC,
                        prior_ytd.statement_point_id DESC
                ) = 1
            ),
            quarter_points AS (
                SELECT *
                FROM reported_quarter_points
                UNION ALL
                SELECT *
                FROM derived_ytd_quarter_points
                WHERE period_days BETWEEN 70 AND 115
            ),
            mapped AS (
                SELECT
                    q.*,
                    coalesce(
                        cm.greatest_overlap_calendar_year,
                        CAST(EXTRACT(YEAR FROM q.period_end) AS INTEGER)
                    ) AS calendar_year,
                    coalesce(
                        cm.greatest_overlap_calendar_quarter,
                        CAST(EXTRACT(QUARTER FROM q.period_end) AS INTEGER)
                    ) AS calendar_quarter,
                    coalesce(
                        cm.greatest_overlap_calendar_period,
                        CAST(EXTRACT(YEAR FROM q.period_end) AS VARCHAR)
                            || 'Q'
                            || CAST(EXTRACT(QUARTER FROM q.period_end) AS VARCHAR)
                    ) AS calendar_period
                FROM quarter_points q
                LEFT JOIN fundamental_calendar_map cm
                  ON cm.source = ?
                 AND cm.upstream_source = q.source
                 AND cm.security_id = q.security_id
                 AND cm.accession_number = q.accession_number
                 AND cm.period_end = q.period_end
                 AND cm.period_start IS NOT DISTINCT FROM q.period_start
                 AND cm.is_latest_revision
            ),
            mapped_with_end AS (
                SELECT
                    *,
                    CASE calendar_quarter
                        WHEN 1 THEN make_date(calendar_year, 3, 31)
                        WHEN 2 THEN make_date(calendar_year, 6, 30)
                        WHEN 3 THEN make_date(calendar_year, 9, 30)
                        ELSE make_date(calendar_year, 12, 31)
                    END AS calendar_period_end
                FROM mapped
            ),
            visible AS (
                SELECT
                    a.statement_point_id AS anchor_statement_point_id_for_window,
                    a.as_of_date AS anchor_as_of_date,
                    a.available_at AS anchor_available_at,
                    a.calendar_year AS anchor_calendar_year,
                    a.calendar_quarter AS anchor_calendar_quarter,
                    a.calendar_period AS anchor_calendar_period,
                    a.calendar_period_end AS anchor_calendar_period_end,
                    a.source_loaded_at AS anchor_source_loaded_at,
                    q.*,
                    row_number() OVER (
                        PARTITION BY a.statement_point_id, q.calendar_period
                        ORDER BY
                            q.quarter_source_priority,
                            q.availability_ts DESC,
                            q.as_of_date DESC,
                            coalesce(q.source_loaded_at, TIMESTAMP '1970-01-01') DESC,
                            q.revision_sequence DESC,
                            q.statement_point_id DESC
                    ) AS visible_rank
                FROM mapped_with_end a
                JOIN mapped_with_end q
                  ON q.source = a.source
                 AND q.security_id = a.security_id
                 AND q.canonical_metric = a.canonical_metric
                 AND q.unit = a.unit
                 AND q.calendar_period_end <= a.calendar_period_end
                 AND q.calendar_period_end > a.calendar_period_end - INTERVAL 400 DAY
                 AND q.as_of_date <= a.as_of_date
                 AND q.availability_ts <= a.availability_ts
            ),
            latest_visible AS (
                SELECT *
                FROM visible
                WHERE visible_rank = 1
            ),
            trailing_windows AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY anchor_statement_point_id_for_window
                        ORDER BY calendar_period_end DESC, period_end DESC, statement_point_id DESC
                    ) AS trailing_rank
                FROM latest_visible
            ),
            aggregated AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            ?,
                            any_value(source),
                            any_value(security_id),
                            any_value(canonical_metric),
                            any_value(unit),
                            any_value(anchor_calendar_period)
                        )
                    ) AS calendar_ttm_revision_group_id,
                    any_value(anchor_statement_point_id_for_window) AS anchor_statement_point_id,
                    any_value(source) AS upstream_source,
                    any_value(security_id) AS security_id,
                    any_value(symbol) AS symbol,
                    any_value(cik) AS cik,
                    any_value(statement_type) AS statement_type,
                    any_value(statement_section) AS statement_section,
                    any_value(canonical_metric) AS canonical_metric,
                    any_value(canonical_label) AS canonical_label,
                    any_value(unit) AS unit,
                    any_value(unit_type) AS unit_type,
                    any_value(anchor_calendar_year) AS calendar_year,
                    any_value(anchor_calendar_quarter) AS calendar_quarter,
                    any_value(anchor_calendar_period) AS calendar_period,
                    any_value(anchor_calendar_period_end) AS calendar_period_end,
                    min(period_start) AS ttm_start_date,
                    max(period_end) AS ttm_end_date,
                    max(as_of_date) AS as_of_date,
                    max(availability_ts) AS available_at,
                    count(*) AS quarter_count,
                    date_diff('day', min(period_start), max(period_end)) + 1 AS coverage_days,
                    min(availability_ts) AS min_input_available_at,
                    max(availability_ts) AS max_input_available_at,
                    CAST(to_json(list(statement_point_id ORDER BY calendar_period_end, statement_point_id)) AS VARCHAR) AS input_statement_point_ids_json,
                    CAST(to_json(list(accession_number ORDER BY calendar_period_end, statement_point_id)) AS VARCHAR) AS input_accessions_json,
                    CAST(to_json(list(calendar_period ORDER BY calendar_period_end, statement_point_id)) AS VARCHAR) AS input_calendar_periods_json,
                    sum(value) AS ttm_value,
                    max(coalesce(source_loaded_at, anchor_source_loaded_at)) AS source_loaded_at
                FROM trailing_windows
                WHERE trailing_rank <= 4
                GROUP BY anchor_statement_point_id_for_window
            ),
            keyed AS (
                SELECT
                    sha256(concat_ws('|', calendar_ttm_revision_group_id, anchor_statement_point_id)) AS calendar_ttm_id,
                    *
                FROM aggregated
            ),
            sequenced AS (
                SELECT
                    keyed.*,
                    row_number() OVER ttm_window AS revision_sequence,
                    count(*) OVER ttm_window AS revision_count,
                    lag(ttm_value) OVER ttm_window AS previous_ttm_value
                FROM keyed
                WINDOW ttm_window AS (
                    PARTITION BY calendar_ttm_revision_group_id
                    ORDER BY
                        available_at,
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        anchor_statement_point_id
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                calendar_ttm_id,
                calendar_ttm_revision_group_id,
                ? AS source,
                upstream_source,
                anchor_statement_point_id,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                unit,
                unit_type,
                CAST(calendar_year AS INTEGER) AS calendar_year,
                CAST(calendar_quarter AS INTEGER) AS calendar_quarter,
                calendar_period,
                calendar_period_end,
                ttm_start_date,
                ttm_end_date,
                as_of_date,
                available_at,
                CAST(quarter_count AS INTEGER) AS quarter_count,
                CAST(coverage_days AS INTEGER) AS coverage_days,
                quarter_count = 4 AND coverage_days BETWEEN 330 AND 380 AS is_complete,
                min_input_available_at,
                max_input_available_at,
                input_statement_point_ids_json,
                input_accessions_json,
                input_calendar_periods_json,
                ttm_value,
                previous_ttm_value,
                CASE
                    WHEN previous_ttm_value IS NULL OR ttm_value IS NULL THEN NULL
                    ELSE ttm_value - previous_ttm_value
                END AS ttm_value_delta,
                CASE
                    WHEN previous_ttm_value IS NULL OR previous_ttm_value = 0 OR ttm_value IS NULL THEN NULL
                    ELSE (ttm_value - previous_ttm_value) / abs(previous_ttm_value)
                END AS ttm_value_delta_percent,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE ttm_value IS DISTINCT FROM previous_ttm_value
                END AS is_value_changed,
                'calendar_aligned_ttm' AS calculation_method,
                ? AS run_id,
                source_loaded_at
            FROM sequenced
            """,
            [options.source, options.source, options.source, options.run_id],
        )
    return int(
        store.con.execute(
            "SELECT count(*) FROM fundamental_calendar_ttm WHERE source = ?",
            [options.source],
        ).fetchone()[0]
    )


def refresh_calendarization_coverage(
    store: DuckDBStore,
    options: CalendarizationOptions | None = None,
) -> int:
    """Refresh the calendarization coverage/gating report."""

    options = options or CalendarizationOptions()
    with store.transaction():
        store.con.execute("DELETE FROM calendarization_coverage WHERE source = ?", [options.source])
        store.con.execute(
            """
            INSERT INTO calendarization_coverage (
                coverage_id,
                source,
                period_count,
                map_row_count,
                fiscal_scheme_unmapped_count,
                containing_scheme_unmapped_count,
                overlap_scheme_unmapped_count,
                duplicate_map_count,
                overlength_period_count,
                unflagged_53_week_count,
                calendar_ttm_row_count,
                incomplete_calendar_ttm_count,
                duplicate_calendar_ttm_window_count,
                stitched_ttm_row_count,
                duplicate_stitched_ttm_window_count,
                as_of_date,
                available_at,
                is_latest_revision,
                run_id,
                source_loaded_at
            )
            WITH mapped AS (
                SELECT *
                FROM fundamental_calendar_map
                WHERE source = ?
            ),
            calendar_ttm AS (
                SELECT *
                FROM fundamental_calendar_ttm
                WHERE source = ?
            ),
            duplicate_maps AS (
                SELECT count(*) AS duplicate_count
                FROM (
                    SELECT fundamental_period_id, count(*) AS row_count
                    FROM mapped
                    GROUP BY 1
                    HAVING count(*) <> 1
                )
            ),
            duplicate_calendar_ttm AS (
                SELECT count(*) AS duplicate_count
                FROM (
                    SELECT
                        upstream_source,
                        security_id,
                        calendar_period,
                        canonical_metric,
                        unit,
                        count(*) AS row_count
                    FROM calendar_ttm
                    WHERE is_latest_revision
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            ),
            duplicate_stitched AS (
                SELECT count(*) AS duplicate_count
                FROM (
                    SELECT source, security_id, ttm_end_date, canonical_metric, unit, count(*) AS row_count
                    FROM fundamental_ttm_points
                    WHERE calculation_method = ?
                      AND is_latest_revision
                    GROUP BY 1, 2, 3, 4, 5
                    HAVING count(*) > 1
                )
            )
            SELECT
                sha256(concat_ws('|', ?, CAST(current_date AS VARCHAR))) AS coverage_id,
                ? AS source,
                (SELECT count(*) FROM fundamental_periods) AS period_count,
                (SELECT count(*) FROM mapped) AS map_row_count,
                (SELECT count(*) FROM mapped WHERE fiscal_scheme_period IS NULL OR fiscal_scheme_period = '') AS fiscal_scheme_unmapped_count,
                (SELECT count(*) FROM mapped WHERE containing_calendar_period IS NULL OR containing_calendar_period = '') AS containing_scheme_unmapped_count,
                (SELECT count(*) FROM mapped WHERE greatest_overlap_calendar_period IS NULL OR greatest_overlap_calendar_period = '') AS overlap_scheme_unmapped_count,
                (SELECT duplicate_count FROM duplicate_maps) AS duplicate_map_count,
                (SELECT count(*) FROM mapped WHERE period_length_days >= 371 OR (normalized_period_type = 'annual' AND period_length_days > 364) OR (normalized_period_type = 'quarter' AND period_length_days > 91)) AS overlength_period_count,
                (SELECT count(*) FROM mapped WHERE (period_length_days >= 371 OR (normalized_period_type = 'annual' AND period_length_days > 364) OR (normalized_period_type = 'quarter' AND period_length_days > 91)) AND NOT is_53_week) AS unflagged_53_week_count,
                (SELECT count(*) FROM calendar_ttm) AS calendar_ttm_row_count,
                (SELECT count(*) FROM calendar_ttm WHERE NOT is_complete) AS incomplete_calendar_ttm_count,
                (SELECT duplicate_count FROM duplicate_calendar_ttm) AS duplicate_calendar_ttm_window_count,
                (SELECT count(*) FROM fundamental_ttm_points WHERE calculation_method = ?) AS stitched_ttm_row_count,
                (SELECT duplicate_count FROM duplicate_stitched) AS duplicate_stitched_ttm_window_count,
                current_date AS as_of_date,
                now() AS available_at,
                true AS is_latest_revision,
                ? AS run_id,
                now() AS source_loaded_at
            """,
            [
                options.source,
                options.source,
                STITCHED_TTM_METHOD,
                options.source,
                options.source,
                STITCHED_TTM_METHOD,
                options.run_id,
            ],
        )
    return int(
        store.con.execute(
            "SELECT count(*) FROM calendarization_coverage WHERE source = ?",
            [options.source],
        ).fetchone()[0]
    )


def run_calendarization_refresh(
    store: DuckDBStore,
    options: CalendarizationOptions | None = None,
) -> dict[str, Any]:
    """Refresh calendar map, calendar TTM, coverage, and record a summary check."""

    options = options or CalendarizationOptions()
    store.initialize()
    map_rows = refresh_fundamental_calendar_map(store, options)
    calendar_ttm_rows = refresh_fundamental_calendar_ttm(store, options)
    coverage_rows = refresh_calendarization_coverage(store, options)
    coverage = store.con.execute(
        """
        SELECT
            period_count,
            map_row_count,
            fiscal_scheme_unmapped_count
                + containing_scheme_unmapped_count
                + overlap_scheme_unmapped_count
                + duplicate_map_count
                + unflagged_53_week_count
                + duplicate_calendar_ttm_window_count
                + duplicate_stitched_ttm_window_count AS failure_count,
            stitched_ttm_row_count
        FROM calendarization_coverage
        WHERE source = ?
        ORDER BY available_at DESC
        LIMIT 1
        """,
        [options.source],
    ).fetchone()
    failure_count = float(coverage[2]) if coverage is not None else 1.0
    quality_check(
        store,
        dataset_id="calendarization",
        table_name="calendarization_coverage",
        check_name="calendarization_coverage_green",
        status="passed" if failure_count == 0.0 else "failed",
        observed_value=failure_count,
        threshold_value=0.0,
        details={
            "map_rows": map_rows,
            "calendar_ttm_rows": calendar_ttm_rows,
            "coverage_rows": coverage_rows,
            "stitched_ttm_rows": int(coverage[3]) if coverage is not None else 0,
        },
    )
    return {
        "map_rows": map_rows,
        "calendar_ttm_rows": calendar_ttm_rows,
        "coverage_rows": coverage_rows,
        "coverage_failure_count": failure_count,
    }


class CalendarizationDataset(Dataset):
    dataset_id = "calendarization"
    source_name = SOURCE_NAME
    depends_on = (
        "fundamental_periods",
        "fundamental_statement_points",
        "fundamental_ttm_points",
    )

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: CalendarizationOptions) -> DatasetLoadResult:
        details = run_calendarization_refresh(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(details["map_rows"])
            + int(details["calendar_ttm_rows"])
            + int(details["coverage_rows"]),
            source=options.source,
            details=details,
            run_id=options.run_id,
        )
