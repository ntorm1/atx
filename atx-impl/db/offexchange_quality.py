"""Derived quality reports for public off-exchange and short-flow surfaces."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .offexchange import DEFAULT_SOURCE as DEFAULT_OFFEXCHANGE_SOURCE
from .short_volume import DEFAULT_SOURCE as DEFAULT_SHORT_VOLUME_SOURCE
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived off-exchange quality report"
DEFAULT_SOURCE = "derived_offexchange_quality_report_v1"

REPORT_COLUMNS = [
    "report_id", "source", "surface", "input_source", "period_type",
    "period_start_date", "period_end_date", "row_count", "security_count",
    "venue_or_market_count", "total_volume", "ats_volume", "non_ats_volume",
    "short_volume", "short_exempt_volume", "short_volume_ratio",
    "ats_share_pct", "high_short_flow_count", "restated_key_count",
    "multiple_latest_key_count", "bad_row_count", "missing_available_at_count",
    "max_publication_lag_days", "restatement_seq", "is_latest_revision",
    "as_of_date", "available_at", "source_inputs_json", "run_id",
]


@dataclass(frozen=True)
class OffExchangeQualityReportOptions:
    source: str = DEFAULT_SOURCE
    offexchange_source: str = DEFAULT_OFFEXCHANGE_SOURCE
    short_volume_source: str = DEFAULT_SHORT_VOLUME_SOURCE
    include_offexchange: bool = True
    include_short_volume: bool = True
    run_id: str | None = None


def _empty_report_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=REPORT_COLUMNS)


def _report_id(row: pd.Series) -> str:
    parts = [
        row.get("source"), row.get("surface"), row.get("input_source"),
        row.get("period_type"), row.get("period_start_date"), row.get("available_at"),
    ]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _finalize_reports(frame: pd.DataFrame, *, source: str, run_id: str | None) -> pd.DataFrame:
    if frame.empty:
        return _empty_report_frame()
    out = frame.copy()
    out["source"] = source
    out["restatement_seq"] = 0
    out["is_latest_revision"] = True
    out["run_id"] = run_id
    out["report_id"] = out.apply(_report_id, axis=1)
    return out[REPORT_COLUMNS]


def _offexchange_reports(store: DuckDBStore, options: OffExchangeQualityReportOptions) -> pd.DataFrame:
    return store.con.execute(
        """
        WITH latest AS (
            SELECT
                *,
                CASE
                    WHEN summary_end_date IS NOT NULL THEN summary_end_date
                    WHEN period_type = 'weekly' THEN CAST(summary_start_date + INTERVAL 6 DAY AS DATE)
                    WHEN period_type = 'monthly' THEN last_day(summary_start_date)
                    ELSE summary_start_date
                END AS effective_period_end_date
            FROM offexchange_volume
            WHERE source = ?
              AND is_latest
        ),
        period_rows AS (
            SELECT
                'offexchange_volume' AS surface,
                source AS input_source,
                period_type,
                summary_start_date AS period_start_date,
                max(effective_period_end_date) AS period_end_date,
                count(*) AS row_count,
                count(DISTINCT symbol) AS security_count,
                count(DISTINCT mpid) AS venue_or_market_count,
                sum(coalesce(total_share_quantity, 0)) AS total_volume,
                sum(CASE WHEN venue_class = 'ATS' THEN coalesce(total_share_quantity, 0) ELSE 0 END) AS ats_volume,
                sum(CASE WHEN venue_class = 'non_ATS' THEN coalesce(total_share_quantity, 0) ELSE 0 END) AS non_ats_volume,
                NULL::DOUBLE AS short_volume,
                NULL::DOUBLE AS short_exempt_volume,
                NULL::DOUBLE AS short_volume_ratio,
                CASE WHEN sum(coalesce(total_share_quantity, 0)) > 0
                     THEN sum(CASE WHEN venue_class = 'ATS' THEN coalesce(total_share_quantity, 0) ELSE 0 END)
                          / sum(coalesce(total_share_quantity, 0)) * 100
                     ELSE NULL END AS ats_share_pct,
                NULL::BIGINT AS high_short_flow_count,
                sum(CASE WHEN available_at IS NULL THEN 1 ELSE 0 END) AS missing_available_at_count,
                sum(CASE
                    WHEN symbol IS NULL OR symbol = ''
                      OR summary_start_date IS NULL
                      OR venue_class NOT IN ('ATS', 'non_ATS')
                      OR coalesce(total_share_quantity, -1) < 0
                      OR available_at IS NULL
                    THEN 1 ELSE 0 END) AS bad_row_count,
                max(date_diff('day', effective_period_end_date, CAST(available_at AS DATE))) AS max_publication_lag_days,
                max(as_of_date) AS as_of_date,
                max(available_at) AS available_at
            FROM latest
            GROUP BY source, period_type, summary_start_date
        ),
        key_stats AS (
            SELECT
                source AS input_source,
                period_type,
                summary_start_date AS period_start_date,
                sum(CASE WHEN row_count > 1 THEN 1 ELSE 0 END) AS restated_key_count,
                sum(CASE WHEN latest_count > 1 THEN 1 ELSE 0 END) AS multiple_latest_key_count
            FROM (
                SELECT
                    source, symbol, mpid, venue_class, period_type, summary_start_date,
                    count(*) AS row_count,
                    sum(CASE WHEN is_latest THEN 1 ELSE 0 END) AS latest_count
                FROM offexchange_volume
                WHERE source = ?
                GROUP BY source, symbol, mpid, venue_class, period_type, summary_start_date
            )
            GROUP BY source, period_type, summary_start_date
        )
        SELECT
            p.*,
            coalesce(k.restated_key_count, 0) AS restated_key_count,
            coalesce(k.multiple_latest_key_count, 0) AS multiple_latest_key_count,
            ? AS source_inputs_json
        FROM period_rows p
        LEFT JOIN key_stats k
          ON k.input_source = p.input_source
         AND k.period_type = p.period_type
         AND k.period_start_date = p.period_start_date
        ORDER BY p.period_start_date, p.period_type
        """,
        [
            options.offexchange_source,
            options.offexchange_source,
            json_dumps({"tables": ["offexchange_volume"], "input_source": options.offexchange_source}),
        ],
    ).df()


def _short_volume_reports(store: DuckDBStore, options: OffExchangeQualityReportOptions) -> pd.DataFrame:
    return store.con.execute(
        """
        WITH latest AS (
            SELECT *
            FROM finra_short_volume
            WHERE source = ?
              AND is_latest
        ),
        metric_high AS (
            SELECT
                source AS input_source,
                trade_date AS period_start_date,
                sum(CASE WHEN is_high_short_flow THEN 1 ELSE 0 END) AS high_short_flow_count,
                max(available_at) AS metric_available_at
            FROM short_volume_metrics
            WHERE source = ?
              AND is_latest_revision
            GROUP BY source, trade_date
        ),
        period_rows AS (
            SELECT
                'finra_short_volume' AS surface,
                source AS input_source,
                'daily' AS period_type,
                trade_date AS period_start_date,
                trade_date AS period_end_date,
                count(*) AS row_count,
                count(DISTINCT symbol) AS security_count,
                count(DISTINCT market_code) AS venue_or_market_count,
                sum(coalesce(total_volume, 0)) AS total_volume,
                NULL::DOUBLE AS ats_volume,
                NULL::DOUBLE AS non_ats_volume,
                sum(coalesce(short_volume, 0)) AS short_volume,
                sum(coalesce(short_exempt_volume, 0)) AS short_exempt_volume,
                CASE WHEN sum(coalesce(total_volume, 0)) > 0
                     THEN sum(coalesce(short_volume, 0)) / sum(coalesce(total_volume, 0))
                     ELSE NULL END AS short_volume_ratio,
                NULL::DOUBLE AS ats_share_pct,
                sum(CASE WHEN available_at IS NULL THEN 1 ELSE 0 END) AS missing_available_at_count,
                sum(CASE
                    WHEN symbol IS NULL OR symbol = ''
                      OR trade_date IS NULL
                      OR market_code IS NULL OR market_code = ''
                      OR coalesce(short_volume, -1) < 0
                      OR coalesce(short_exempt_volume, -1) < 0
                      OR coalesce(total_volume, -1) < 0
                      OR short_volume > total_volume
                      OR short_exempt_volume > total_volume
                      OR available_at IS NULL
                    THEN 1 ELSE 0 END) AS bad_row_count,
                max(date_diff('day', trade_date, CAST(available_at AS DATE))) AS max_publication_lag_days,
                max(as_of_date) AS as_of_date,
                max(available_at) AS raw_available_at
            FROM latest
            GROUP BY source, trade_date
        ),
        key_stats AS (
            SELECT
                source AS input_source,
                trade_date AS period_start_date,
                sum(CASE WHEN row_count > 1 THEN 1 ELSE 0 END) AS restated_key_count,
                sum(CASE WHEN latest_count > 1 THEN 1 ELSE 0 END) AS multiple_latest_key_count
            FROM (
                SELECT
                    source, symbol, trade_date, market_code,
                    count(*) AS row_count,
                    sum(CASE WHEN is_latest THEN 1 ELSE 0 END) AS latest_count
                FROM finra_short_volume
                WHERE source = ?
                GROUP BY source, symbol, trade_date, market_code
            )
            GROUP BY source, trade_date
        )
        SELECT
            p.surface,
            p.input_source,
            p.period_type,
            p.period_start_date,
            p.period_end_date,
            p.row_count,
            p.security_count,
            p.venue_or_market_count,
            p.total_volume,
            p.ats_volume,
            p.non_ats_volume,
            p.short_volume,
            p.short_exempt_volume,
            p.short_volume_ratio,
            p.ats_share_pct,
            coalesce(h.high_short_flow_count, 0) AS high_short_flow_count,
            p.missing_available_at_count,
            p.bad_row_count,
            p.max_publication_lag_days,
            p.as_of_date,
            greatest(p.raw_available_at, coalesce(h.metric_available_at, p.raw_available_at)) AS available_at,
            coalesce(k.restated_key_count, 0) AS restated_key_count,
            coalesce(k.multiple_latest_key_count, 0) AS multiple_latest_key_count,
            ? AS source_inputs_json
        FROM period_rows p
        LEFT JOIN metric_high h
          ON h.input_source = p.input_source
         AND h.period_start_date = p.period_start_date
        LEFT JOIN key_stats k
          ON k.input_source = p.input_source
         AND k.period_start_date = p.period_start_date
        ORDER BY p.period_start_date
        """,
        [
            options.short_volume_source,
            options.short_volume_source,
            options.short_volume_source,
            json_dumps({"tables": ["finra_short_volume", "short_volume_metrics"], "input_source": options.short_volume_source}),
        ],
    ).df()


def compute_offexchange_quality_reports(store: DuckDBStore, options: OffExchangeQualityReportOptions) -> pd.DataFrame:
    frames: list[pd.DataFrame] = []
    if options.include_offexchange:
        frames.append(_offexchange_reports(store, options))
    if options.include_short_volume:
        frames.append(_short_volume_reports(store, options))
    non_empty = [frame for frame in frames if not frame.empty]
    if not non_empty:
        return _empty_report_frame()
    records = [row for frame in non_empty for row in frame.to_dict("records")]
    return _finalize_reports(pd.DataFrame.from_records(records), source=options.source, run_id=options.run_id)


def _delete_existing_ids(store: DuckDBStore, frame: pd.DataFrame) -> None:
    ids = frame[["report_id"]].drop_duplicates()
    relation = "offexchange_quality_report_delete"
    store.con.register(relation, ids)
    try:
        store.con.execute(
            "DELETE FROM offexchange_quality_report AS dst USING offexchange_quality_report_delete AS src WHERE dst.report_id = src.report_id"
        )
    finally:
        store.con.unregister(relation)


def _recompute_latest(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH ranked AS (
            SELECT
                report_id,
                row_number() OVER (
                    PARTITION BY source, surface, input_source, period_type, period_start_date
                    ORDER BY available_at DESC, report_id
                ) AS rn,
                dense_rank() OVER (
                    PARTITION BY source, surface, input_source, period_type, period_start_date
                    ORDER BY available_at ASC
                ) - 1 AS seq
            FROM offexchange_quality_report
            WHERE source = ?
        )
        UPDATE offexchange_quality_report r
        SET is_latest_revision = (ranked.rn = 1),
            restatement_seq = ranked.seq,
            updated_at = now()
        FROM ranked
        WHERE ranked.report_id = r.report_id
        """,
        [source],
    )


def refresh_offexchange_quality_report(
    store: DuckDBStore,
    options: OffExchangeQualityReportOptions | None = None,
) -> int:
    store.initialize()
    options = options or OffExchangeQualityReportOptions()
    reports = compute_offexchange_quality_reports(store, options)
    if reports.empty:
        return 0
    with store.transaction():
        _delete_existing_ids(store, reports)
        insert_frame(store, reports, "offexchange_quality_report", "offexchange_quality_report_insert")
        _recompute_latest(store, options.source)
    return int(len(reports))


class OffExchangeQualityReportDataset(Dataset):
    dataset_id = "offexchange_quality_report"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: OffExchangeQualityReportOptions) -> DatasetLoadResult:
        rows = refresh_offexchange_quality_report(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="offexchange_quality_report",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "offexchange_source": options.offexchange_source,
                "short_volume_source": options.short_volume_source,
                "include_offexchange": options.include_offexchange,
                "include_short_volume": options.include_short_volume,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "surface,input_source,period_type,period_start_date"},
        )
