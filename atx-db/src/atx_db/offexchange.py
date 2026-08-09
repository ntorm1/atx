"""S7a: FINRA OTC Transparency off-exchange ATS / non-ATS volume warehouse.

FINRA's OTC Transparency program is the only public, free, structured dataset of
US off-exchange (dark-pool ATS + non-ATS member-firm) trading volume per security
and venue. The live feed is the FINRA Query API (`otcMarket` group, OAuth2
client-credentials) on a tiered publication delay (Tier-1 ~14d, Tier-2/OTCE ~28d),
so — like the estimate surfaces — this module ingests an *injectable* normalized
or FINRA `weeklySummary`-shaped CSV and never calls the network in tests.

Surfaces:
* ``offexchange_venue``           -- MPID venue dimension (ATS vs non-ATS)
* ``offexchange_volume``          -- per (security, venue, period) volume fact, with
                                     bitemporal availability and FINRA restatement
                                     handling (``is_latest`` / ``restatement_seq``)
* ``offexchange_security_period`` -- derived per (security, period) ATS-share rollup
"""

from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import file_sha256, insert_frame, json_dumps, now_utc_naive, quality_check, record_source_file, snake_case, symbol_key


SOURCE_NAME = "FINRA OTC Transparency off-exchange volume"
DEFAULT_SOURCE = "injected_finra_offexchange_v1"

VENUE_CLASS_FROM_SUMMARY = {
    "ATS_W_SMBL": "ATS",
    "OTC_W_SMBL": "non_ATS",
}

VOLUME_COLUMNS = [
    "volume_id", "security_id", "symbol", "mpid", "venue_class",
    "summary_type_code", "period_type", "tier", "summary_start_date",
    "summary_end_date", "total_share_quantity", "total_trade_count",
    "finra_last_update_date", "restatement_seq", "is_latest",
    "as_of_date", "available_at", "source", "source_file",
    "source_file_sha256", "raw_payload_json", "run_id",
]

COLUMN_ALIASES = {
    "issuesymbolidentifier": "symbol",
    "issue_symbol_identifier": "symbol",
    "issuesymbol": "symbol",
    "ticker": "symbol",
    "tic": "symbol",
    "marketparticipantid": "mpid",
    "mpid": "mpid",
    "summarytypecode": "summary_type_code",
    "totalweeklysharequantity": "total_share_quantity",
    "totalmonthlysharequantity": "total_share_quantity",
    "totalsharequantity": "total_share_quantity",
    "sharequantity": "total_share_quantity",
    "totalweeklytradecount": "total_trade_count",
    "totalmonthlytradecount": "total_trade_count",
    "totaltradecount": "total_trade_count",
    "tradecount": "total_trade_count",
    "weekstartdate": "summary_start_date",
    "monthstartdate": "summary_start_date",
    "reportstartdate": "summary_start_date",
    "weekenddate": "summary_end_date",
    "monthenddate": "summary_end_date",
    "lastupdatedate": "finra_last_update_date",
    "tierdescription": "tier",
    "tieridentifier": "tier",
    "firmname": "venue_name",
    "crd": "firm_crd",
    "crdnumber": "firm_crd",
    "asofdate": "as_of_date",
    "knowledgefrom": "available_at",
    "sourceloadedat": "available_at",
}


@dataclass(frozen=True)
class FinraOffExchangeOptions:
    source_file: Path | None = None
    source: str = DEFAULT_SOURCE
    period_type: str = "weekly"
    tier: str | None = None
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_volume_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=VOLUME_COLUMNS)


def _normalize_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = COLUMN_ALIASES.get(normalized, COLUMN_ALIASES.get(compact, normalized))
    return frame.rename(columns=renamed)


def _string(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="string")
    return frame[column].replace("", pd.NA).astype("string")


def _numeric(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="Float64")
    return pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")


def _date(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NaT] * len(frame), index=frame.index)
    return pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce").dt.date


def _canon_venue_class(summary_type_code: object, venue_class: object) -> str:
    stc = ("" if pd.isna(summary_type_code) else str(summary_type_code)).strip().upper()
    if stc in VENUE_CLASS_FROM_SUMMARY:
        return VENUE_CLASS_FROM_SUMMARY[stc]
    vc = ("" if pd.isna(venue_class) else str(venue_class)).strip()
    if vc:
        return "ATS" if vc.upper() == "ATS" else "non_ATS"
    return "non_ATS"


def _volume_id(row: pd.Series) -> str:
    parts = [
        row.get("source"), row.get("symbol"), row.get("mpid"),
        row.get("venue_class"), row.get("period_type"),
        row.get("summary_start_date"), row.get("available_at"),
    ]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def normalize_offexchange_rows(
    frame: pd.DataFrame,
    *,
    options: FinraOffExchangeOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_volume_frame()

    raw = _normalize_columns(frame.copy())
    if "symbol" not in raw.columns:
        raise ValueError("Off-exchange rows require an issue symbol identifier")
    if "summary_start_date" not in raw.columns:
        raise ValueError("Off-exchange rows require a summary start date")

    now = now_utc_naive()
    summary_type = _string(raw, "summary_type_code")
    venue_class_raw = _string(raw, "venue_class")
    venue_class = pd.Series(
        [_canon_venue_class(s, v) for s, v in zip(summary_type, venue_class_raw)],
        index=raw.index,
        dtype="string",
    )

    start_date = _date(raw, "summary_start_date")
    end_date = _date(raw, "summary_end_date")
    as_of_date = _date(raw, "as_of_date")
    as_of_date = as_of_date.where(pd.notna(as_of_date), end_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), start_date)
    available_at = pd.to_datetime(
        raw["available_at"].replace("", pd.NA) if "available_at" in raw.columns else pd.Series([pd.NA] * len(raw)),
        errors="coerce",
    ).fillna(pd.Timestamp(now))

    period_type = _string(raw, "period_type").fillna(options.period_type).str.lower()
    tier = _string(raw, "tier")
    if options.tier is not None:
        tier = tier.fillna(options.tier)

    out = pd.DataFrame(index=raw.index)
    out["security_id"] = _string(raw, "security_id")
    out["symbol"] = _string(raw, "symbol").map(lambda v: symbol_key(None if pd.isna(v) else str(v))).replace("", pd.NA)
    out["mpid"] = _string(raw, "mpid").str.strip().str.upper()
    out["venue_class"] = venue_class
    out["summary_type_code"] = summary_type
    out["period_type"] = period_type
    out["tier"] = tier
    out["summary_start_date"] = start_date
    out["summary_end_date"] = end_date
    out["total_share_quantity"] = _numeric(raw, "total_share_quantity")
    out["total_trade_count"] = pd.to_numeric(
        raw["total_trade_count"].replace("", pd.NA) if "total_trade_count" in raw.columns else pd.Series([pd.NA] * len(raw)),
        errors="coerce",
    ).astype("Int64")
    out["finra_last_update_date"] = _date(raw, "finra_last_update_date")
    out["restatement_seq"] = 0
    out["is_latest"] = True
    out["as_of_date"] = as_of_date
    out["available_at"] = available_at
    out["source"] = options.source
    out["source_file"] = str(source_file) if source_file else pd.NA
    out["source_file_sha256"] = source_file_sha256
    out["raw_payload_json"] = raw.apply(lambda r: json_dumps(r.dropna().to_dict()), axis=1)
    out["run_id"] = options.run_id

    out = out[
        out["symbol"].notna()
        & out["summary_start_date"].notna()
        & out["as_of_date"].notna()
        & out["available_at"].notna()
    ].copy()
    if out.empty:
        return _empty_volume_frame()
    out["volume_id"] = out.apply(_volume_id, axis=1)
    return out[VOLUME_COLUMNS]


def _recompute_is_latest(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH ranked AS (
            SELECT
                volume_id,
                row_number() OVER (
                    PARTITION BY symbol, mpid, venue_class, period_type, summary_start_date
                    ORDER BY available_at DESC, volume_id
                ) AS rn,
                (dense_rank() OVER (
                    PARTITION BY symbol, mpid, venue_class, period_type, summary_start_date
                    ORDER BY available_at ASC
                ) - 1) AS seq
            FROM offexchange_volume
            WHERE source = ?
        )
        UPDATE offexchange_volume v
        SET is_latest = (r.rn = 1),
            restatement_seq = r.seq,
            updated_at = now()
        FROM ranked r
        WHERE r.volume_id = v.volume_id
        """,
        [source],
    )


def _upsert_venues(store: DuckDBStore, source: str) -> int:
    store.con.execute(
        """
        INSERT OR REPLACE INTO offexchange_venue (
            mpid, venue_name, venue_class, firm_crd, first_seen_date, last_seen_date, source
        )
        SELECT
            v.mpid,
            NULL AS venue_name,           -- venue_name is not carried on the volume fact
            any_value(v.venue_class) AS venue_class,
            NULL AS firm_crd,
            min(v.summary_start_date) AS first_seen_date,
            max(v.summary_start_date) AS last_seen_date,
            ? AS source
        FROM offexchange_volume v
        WHERE v.source = ? AND v.mpid IS NOT NULL
        GROUP BY v.mpid
        """,
        [source, source],
    )
    return int(
        store.con.execute(
            "SELECT count(*) FROM offexchange_venue WHERE source = ?", [source]
        ).fetchone()[0]
    )


def _resolve_security_ids(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        UPDATE offexchange_volume v
        SET security_id = s.security_id
        FROM securities s
        WHERE s.primary_symbol = v.symbol
          AND v.source = ?
          AND v.security_id IS NULL
        """,
        [source],
    )


def load_offexchange_volume(store: DuckDBStore, options: FinraOffExchangeOptions) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    normalized = normalize_offexchange_rows(
        frame, options=options, source_file_sha256=source_hash, source_file=source_file
    )
    record_source_file(
        store,
        dataset_id="offexchange_volume",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"period_type": options.period_type, "rows": int(len(frame))},
    )
    if normalized.empty:
        return 0
    with store.transaction():
        if options.replace_source_file:
            store.con.execute(
                "DELETE FROM offexchange_volume WHERE source = ? AND source_file_sha256 = ?",
                [options.source, source_hash],
            )
        insert_frame(store, normalized, "offexchange_volume", "offexchange_volume_insert")
        _resolve_security_ids(store, options.source)
        _recompute_is_latest(store, options.source)
        _upsert_venues(store, options.source)
    return int(len(normalized))


def refresh_offexchange_security_period(
    store: DuckDBStore,
    *,
    period_type: str | None = None,
    source: str = DEFAULT_SOURCE,
) -> int:
    """Materialize the per (security, period) ATS-share rollup from latest volume."""
    store.initialize()
    with store.transaction():
        store.con.execute(
            """
            DELETE FROM offexchange_security_period
            WHERE source = ?
              AND (?::VARCHAR IS NULL OR period_type = ?)
            """,
            [source, period_type, period_type],
        )
        store.con.execute(
            """
            INSERT INTO offexchange_security_period (
                security_period_id, security_id, symbol, period_type, summary_start_date,
                summary_end_date, ats_share_quantity, non_ats_share_quantity,
                total_share_quantity, ats_share_pct, ats_venue_count,
                restatement_detected, as_of_date, available_at, source, run_id
            )
            WITH latest AS (
                SELECT *
                FROM offexchange_volume
                WHERE is_latest
                  AND source = ?
                  AND (?::VARCHAR IS NULL OR period_type = ?)
            ),
            restate AS (
                SELECT symbol, period_type, summary_start_date,
                       bool_or(NOT is_latest) AS restated
                FROM offexchange_volume
                WHERE source = ?
                  AND (?::VARCHAR IS NULL OR period_type = ?)
                GROUP BY symbol, period_type, summary_start_date
            )
            SELECT
                sha256(concat_ws('|', l.symbol, l.period_type, CAST(l.summary_start_date AS VARCHAR), ?)) AS security_period_id,
                any_value(l.security_id) AS security_id,
                l.symbol,
                l.period_type,
                l.summary_start_date,
                max(l.summary_end_date) AS summary_end_date,
                sum(CASE WHEN l.venue_class = 'ATS' THEN coalesce(l.total_share_quantity, 0) ELSE 0 END) AS ats_share_quantity,
                sum(CASE WHEN l.venue_class = 'non_ATS' THEN coalesce(l.total_share_quantity, 0) ELSE 0 END) AS non_ats_share_quantity,
                sum(coalesce(l.total_share_quantity, 0)) AS total_share_quantity,
                CASE WHEN sum(coalesce(l.total_share_quantity, 0)) > 0
                     THEN sum(CASE WHEN l.venue_class = 'ATS' THEN coalesce(l.total_share_quantity, 0) ELSE 0 END)
                          / sum(coalesce(l.total_share_quantity, 0)) * 100
                     ELSE NULL END AS ats_share_pct,
                count(DISTINCT CASE WHEN l.venue_class = 'ATS' THEN l.mpid END) AS ats_venue_count,
                coalesce(any_value(r.restated), FALSE) AS restatement_detected,
                max(l.as_of_date) AS as_of_date,
                max(l.available_at) AS available_at,
                ? AS source,
                ? AS run_id
            FROM latest l
            LEFT JOIN restate r
              ON r.symbol = l.symbol
             AND r.period_type = l.period_type
             AND r.summary_start_date = l.summary_start_date
            GROUP BY l.symbol, l.period_type, l.summary_start_date
            """,
            [source, period_type, period_type, source, period_type, period_type, source, source, None],
        )
    return int(
        store.con.execute(
            """
            SELECT count(*) FROM offexchange_security_period
            WHERE source = ? AND (?::VARCHAR IS NULL OR period_type = ?)
            """,
            [source, period_type, period_type],
        ).fetchone()[0]
    )


class FinraOffExchangeDataset(Dataset):
    dataset_id = "offexchange_volume"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FinraOffExchangeOptions) -> DatasetLoadResult:
        rows = load_offexchange_volume(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="offexchange_volume",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "period_type": options.period_type,
                "source_file": str(options.source_file) if options.source_file else None,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"period_type": options.period_type},
        )


class OffExchangeSecurityPeriodDataset(Dataset):
    dataset_id = "offexchange_security_period"
    source_name = "Derived off-exchange ATS-share rollup"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FinraOffExchangeOptions) -> DatasetLoadResult:
        period_type = options.period_type or None
        rows = refresh_offexchange_security_period(store, period_type=period_type, source=options.source)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="offexchange_security_period",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "period_type": options.period_type},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"period_type": options.period_type},
        )
