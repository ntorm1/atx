from __future__ import annotations

import hashlib
import uuid
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .features import refresh_feature_lineage
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "atx-db FINRA short-interest feature engine"
DEFAULT_SHORT_INTEREST_FEATURE_SET = "finra_short_interest_v1"


SHORT_INTEREST_FEATURE_DEFINITIONS = {
    "si_current_short_position": {
        "description": "FINRA current short position quantity as of the settlement date.",
        "expression_sql": "sum(finra_short_interest.current_short_position_quantity) by security_id and settlement_date",
        "lookback_days": 0,
    },
    "si_previous_short_position": {
        "description": "FINRA previous short position quantity reported with the settlement row.",
        "expression_sql": "sum(finra_short_interest.previous_short_position_quantity) by security_id and settlement_date",
        "lookback_days": 0,
    },
    "si_average_daily_volume": {
        "description": "FINRA average daily volume quantity reported with the short-interest row.",
        "expression_sql": "sum(finra_short_interest.average_daily_volume_quantity) by security_id and settlement_date",
        "lookback_days": 0,
    },
    "si_days_to_cover": {
        "description": "Short-interest days-to-cover ratio, preferring current short position divided by average daily volume.",
        "expression_sql": "current_short_position_quantity / nullif(average_daily_volume_quantity, 0)",
        "lookback_days": 0,
    },
    "si_change_previous": {
        "description": "Source-reported change from previous short position quantity.",
        "expression_sql": "sum(finra_short_interest.change_previous_number) by security_id and settlement_date",
        "lookback_days": 0,
    },
    "si_change_ratio": {
        "description": "Current short position divided by source-reported previous short position minus one.",
        "expression_sql": "current_short_position_quantity / nullif(previous_short_position_quantity, 0) - 1",
        "lookback_days": 0,
    },
    "si_source_change_ratio": {
        "description": "FINRA source-reported change percent converted from percentage points to a decimal ratio.",
        "expression_sql": "change_percent / 100",
        "lookback_days": 0,
    },
    "si_short_to_adv": {
        "description": "Current short position divided by average daily volume.",
        "expression_sql": "current_short_position_quantity / nullif(average_daily_volume_quantity, 0)",
        "lookback_days": 0,
    },
    "si_short_position_1p_change": {
        "description": "Change in current short position versus the prior visible FINRA settlement row.",
        "expression_sql": "current_short_position_quantity - lag(current_short_position_quantity) by security_id",
        "lookback_days": 31,
    },
    "si_short_position_1p_change_ratio": {
        "description": "Percent change in current short position versus the prior visible FINRA settlement row.",
        "expression_sql": "current_short_position_quantity / nullif(lag(current_short_position_quantity), 0) - 1",
        "lookback_days": 31,
    },
    "si_days_to_cover_1p_change": {
        "description": "Change in days to cover versus the prior visible FINRA settlement row.",
        "expression_sql": "days_to_cover - lag(days_to_cover) by security_id",
        "lookback_days": 31,
    },
    "si_short_position_zscore_12p": {
        "description": "Trailing 12-settlement z-score of current short position.",
        "expression_sql": "(current_short_position - avg(current_short_position, 12 rows)) / stddev_samp(current_short_position, 12 rows)",
        "lookback_days": 210,
    },
    "si_short_to_adv_xsec_rank": {
        "description": "Cross-sectional rank of short interest to average daily volume within the settlement date; 1 is highest short pressure.",
        "expression_sql": "rank(short_to_adv desc) by settlement_date, emitted only when settlement-date cross-section is large enough",
        "lookback_days": 0,
    },
    "si_short_to_adv_xsec_percentile": {
        "description": "Cross-sectional percentile of short interest to average daily volume within the settlement date; higher means more short pressure.",
        "expression_sql": "percentile(short_to_adv) by settlement_date, emitted only when settlement-date cross-section is large enough",
        "lookback_days": 0,
    },
    "si_days_to_cover_xsec_percentile": {
        "description": "Cross-sectional percentile of days to cover within the settlement date; higher means more days to cover.",
        "expression_sql": "percentile(days_to_cover) by settlement_date, emitted only when settlement-date cross-section is large enough",
        "lookback_days": 0,
    },
    "si_current_short_position_xsec_percentile": {
        "description": "Cross-sectional percentile of current short position quantity within the settlement date.",
        "expression_sql": "percentile(current_short_position_quantity) by settlement_date, emitted only when settlement-date cross-section is large enough",
        "lookback_days": 0,
    },
    "si_change_ratio_xsec_percentile": {
        "description": "Cross-sectional percentile of short-position change ratio within the settlement date.",
        "expression_sql": "percentile(change_ratio) by settlement_date, emitted only when settlement-date cross-section is large enough",
        "lookback_days": 0,
    },
    "si_short_to_adv_xsec_zscore": {
        "description": "Cross-sectional z-score of short interest to average daily volume within the settlement date.",
        "expression_sql": "(short_to_adv - avg(short_to_adv by settlement_date)) / stddev_samp(short_to_adv by settlement_date)",
        "lookback_days": 0,
    },
}


FEATURE_COLUMNS = {
    "si_current_short_position": "current_short_position_quantity",
    "si_previous_short_position": "previous_short_position_quantity",
    "si_average_daily_volume": "average_daily_volume_quantity",
    "si_days_to_cover": "days_to_cover",
    "si_change_previous": "change_previous_number",
    "si_change_ratio": "change_ratio",
    "si_source_change_ratio": "source_change_ratio",
    "si_short_to_adv": "short_to_adv",
    "si_short_position_1p_change": "short_position_1p_change",
    "si_short_position_1p_change_ratio": "short_position_1p_change_ratio",
    "si_days_to_cover_1p_change": "days_to_cover_1p_change",
    "si_short_position_zscore_12p": "short_position_zscore_12p",
    "si_short_to_adv_xsec_rank": "short_to_adv_xsec_rank",
    "si_short_to_adv_xsec_percentile": "short_to_adv_xsec_percentile",
    "si_days_to_cover_xsec_percentile": "days_to_cover_xsec_percentile",
    "si_current_short_position_xsec_percentile": "current_short_position_xsec_percentile",
    "si_change_ratio_xsec_percentile": "change_ratio_xsec_percentile",
    "si_short_to_adv_xsec_zscore": "short_to_adv_xsec_zscore",
}


@dataclass(frozen=True)
class ShortInterestFeatureOptions:
    feature_set: str = DEFAULT_SHORT_INTEREST_FEATURE_SET
    source: str = SOURCE_NAME
    min_cross_section: int = 20
    run_id: str | None = None


def _feature_hashes(features: pd.DataFrame) -> list[str]:
    hashes: list[str] = []
    for row in features.itertuples(index=False):
        value_text = "" if pd.isna(row.value) else f"{float(row.value):.17g}"
        payload = "|".join(
            [
                str(row.feature_set),
                str(row.feature_name),
                str(row.security_id),
                str(row.as_of_date),
                value_text,
            ]
        )
        hashes.append(hashlib.sha256(payload.encode("utf-8")).hexdigest())
    return hashes


def _manifest_id(feature_set: str, run_id: str | None) -> str:
    payload = "|".join(["finra-short-interest-feature-manifest", feature_set, run_id or ""])
    return str(uuid.uuid5(uuid.NAMESPACE_URL, payload))


class ShortInterestFeatureDataset(Dataset):
    dataset_id = "finra_short_interest_features"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ShortInterestFeatureOptions) -> DatasetLoadResult:
        if not self._source_table_exists(store):
            self._delete_feature_set(store, options.feature_set)
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=options.source,
                details={"feature_set": options.feature_set, "missing_source_tables": ["finra_short_interest"]},
            )

        if options.min_cross_section < 1:
            raise ValueError("min_cross_section must be positive")

        panel = self._build_panel(store, options)
        definitions = self._build_feature_definitions(options)
        features = self._build_feature_values(panel, options)
        manifest = self._build_manifest(panel, features, options)

        with store.transaction():
            self._delete_feature_set(store, options.feature_set)
            insert_frame(store, definitions, "feature_definitions", "short_interest_feature_definitions_insert")
            insert_frame(store, features, "feature_values", "short_interest_feature_values_insert")
            insert_frame(store, manifest, "feature_build_manifests", "short_interest_feature_manifest_insert")

        lineage = refresh_feature_lineage(store)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="feature_values",
            check_name="rows_loaded",
            status="passed" if len(features) > 0 else "warning",
            observed_value=float(len(features)),
            threshold_value=1.0,
            details={
                "feature_set": options.feature_set,
                "input_rows": int(len(panel)),
                "feature_value_rows": int(len(features)),
                "feature_lineage": lineage,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(len(features)),
            source=options.source,
            details={
                "feature_set": options.feature_set,
                "input_rows": int(len(panel)),
                "symbols": sorted(panel["symbol"].dropna().astype(str).unique().tolist()) if not panel.empty else [],
                "min_cross_section": options.min_cross_section,
                "features": sorted(SHORT_INTEREST_FEATURE_DEFINITIONS),
                "feature_lineage": lineage,
            },
        )

    def _source_table_exists(self, store: DuckDBStore) -> bool:
        return bool(
            store.con.execute(
                """
                SELECT count(*)
                FROM duckdb_tables()
                WHERE schema_name = 'main'
                  AND table_name = 'finra_short_interest'
                """
            ).fetchone()[0]
        )

    def _delete_feature_set(self, store: DuckDBStore, feature_set: str) -> None:
        store.con.execute("DELETE FROM feature_values WHERE feature_set = ?", [feature_set])
        store.con.execute("DELETE FROM feature_definitions WHERE feature_set = ?", [feature_set])
        store.con.execute("DELETE FROM feature_build_manifests WHERE feature_set = ?", [feature_set])

    def _build_panel(self, store: DuckDBStore, options: ShortInterestFeatureOptions) -> pd.DataFrame:
        return store.con.execute(
            """
            WITH params AS (
                SELECT ?::INTEGER AS min_cross_section
            ),
            aggregated AS (
                SELECT
                    security_id,
                    any_value(symbol) AS symbol,
                    settlement_date AS as_of_date,
                    max(available_at) AS available_at,
                    count(*) AS source_row_count,
                    count(DISTINCT market_class_code) AS market_class_count,
                    sum(coalesce(current_short_position_quantity, 0))::DOUBLE AS current_short_position_quantity,
                    sum(coalesce(previous_short_position_quantity, 0))::DOUBLE AS previous_short_position_quantity,
                    sum(coalesce(average_daily_volume_quantity, 0))::DOUBLE AS average_daily_volume_quantity,
                    sum(coalesce(change_previous_number, 0))::DOUBLE AS change_previous_number,
                    avg(change_percent)::DOUBLE / 100.0 AS source_change_ratio,
                    max(source_loaded_at) AS source_loaded_at
                FROM finra_short_interest
                WHERE security_id IS NOT NULL
                  AND security_id <> ''
                  AND settlement_date IS NOT NULL
                GROUP BY security_id, settlement_date
            ),
            enriched AS (
                SELECT
                    *,
                    CASE
                        WHEN average_daily_volume_quantity > 0
                        THEN current_short_position_quantity / average_daily_volume_quantity
                        ELSE NULL
                    END AS short_to_adv,
                    CASE
                        WHEN average_daily_volume_quantity > 0
                        THEN current_short_position_quantity / average_daily_volume_quantity
                        ELSE NULL
                    END AS days_to_cover,
                    CASE
                        WHEN previous_short_position_quantity <> 0
                        THEN current_short_position_quantity / previous_short_position_quantity - 1
                        ELSE NULL
                    END AS change_ratio
                FROM aggregated
            ),
            lagged AS (
                SELECT
                    *,
                    lag(current_short_position_quantity) OVER w AS prior_current_short_position_quantity,
                    lag(days_to_cover) OVER w AS prior_days_to_cover,
                    avg(current_short_position_quantity) OVER (
                        PARTITION BY security_id
                        ORDER BY as_of_date
                        ROWS BETWEEN 11 PRECEDING AND CURRENT ROW
                    ) AS rolling_12p_short_position_mean,
                    stddev_samp(current_short_position_quantity) OVER (
                        PARTITION BY security_id
                        ORDER BY as_of_date
                        ROWS BETWEEN 11 PRECEDING AND CURRENT ROW
                    ) AS rolling_12p_short_position_stddev
                FROM enriched
                WINDOW w AS (PARTITION BY security_id ORDER BY as_of_date)
            ),
            xsection AS (
                SELECT
                    *,
                    sum(CASE WHEN short_to_adv IS NOT NULL THEN 1 ELSE 0 END)
                        OVER (PARTITION BY as_of_date) AS short_to_adv_xsec_count,
                    avg(short_to_adv) OVER (PARTITION BY as_of_date) AS short_to_adv_xsec_mean,
                    stddev_samp(short_to_adv) OVER (PARTITION BY as_of_date) AS short_to_adv_xsec_stddev,
                    rank() OVER (PARTITION BY as_of_date ORDER BY short_to_adv DESC NULLS LAST) AS short_to_adv_xsec_rank_raw,
                    rank() OVER (PARTITION BY as_of_date ORDER BY short_to_adv ASC NULLS LAST) AS short_to_adv_xsec_rank_asc,
                    sum(CASE WHEN days_to_cover IS NOT NULL THEN 1 ELSE 0 END)
                        OVER (PARTITION BY as_of_date) AS days_to_cover_xsec_count,
                    rank() OVER (PARTITION BY as_of_date ORDER BY days_to_cover ASC NULLS LAST) AS days_to_cover_xsec_rank_asc,
                    sum(CASE WHEN current_short_position_quantity IS NOT NULL THEN 1 ELSE 0 END)
                        OVER (PARTITION BY as_of_date) AS current_short_position_xsec_count,
                    rank() OVER (
                        PARTITION BY as_of_date
                        ORDER BY current_short_position_quantity ASC NULLS LAST
                    ) AS current_short_position_xsec_rank_asc,
                    sum(CASE WHEN change_ratio IS NOT NULL THEN 1 ELSE 0 END)
                        OVER (PARTITION BY as_of_date) AS change_ratio_xsec_count,
                    rank() OVER (PARTITION BY as_of_date ORDER BY change_ratio ASC NULLS LAST) AS change_ratio_xsec_rank_asc
                FROM lagged
            )
            SELECT
                *,
                current_short_position_quantity - prior_current_short_position_quantity AS short_position_1p_change,
                CASE
                    WHEN prior_current_short_position_quantity <> 0
                    THEN current_short_position_quantity / prior_current_short_position_quantity - 1
                    ELSE NULL
                END AS short_position_1p_change_ratio,
                days_to_cover - prior_days_to_cover AS days_to_cover_1p_change,
                CASE
                    WHEN rolling_12p_short_position_stddev > 0
                    THEN (current_short_position_quantity - rolling_12p_short_position_mean)
                         / rolling_12p_short_position_stddev
                    ELSE NULL
                END AS short_position_zscore_12p,
                CASE
                    WHEN short_to_adv IS NOT NULL
                     AND short_to_adv_xsec_count >= (SELECT min_cross_section FROM params)
                    THEN short_to_adv_xsec_rank_raw::DOUBLE
                    ELSE NULL
                END AS short_to_adv_xsec_rank,
                CASE
                    WHEN short_to_adv IS NOT NULL
                     AND short_to_adv_xsec_count >= (SELECT min_cross_section FROM params)
                    THEN (short_to_adv_xsec_count - short_to_adv_xsec_rank_asc + 1)::DOUBLE
                         / short_to_adv_xsec_count
                    ELSE NULL
                END AS short_to_adv_xsec_percentile,
                CASE
                    WHEN days_to_cover IS NOT NULL
                     AND days_to_cover_xsec_count >= (SELECT min_cross_section FROM params)
                    THEN (days_to_cover_xsec_count - days_to_cover_xsec_rank_asc + 1)::DOUBLE
                         / days_to_cover_xsec_count
                    ELSE NULL
                END AS days_to_cover_xsec_percentile,
                CASE
                    WHEN current_short_position_quantity IS NOT NULL
                     AND current_short_position_xsec_count >= (SELECT min_cross_section FROM params)
                    THEN (current_short_position_xsec_count - current_short_position_xsec_rank_asc + 1)::DOUBLE
                         / current_short_position_xsec_count
                    ELSE NULL
                END AS current_short_position_xsec_percentile,
                CASE
                    WHEN change_ratio IS NOT NULL
                     AND change_ratio_xsec_count >= (SELECT min_cross_section FROM params)
                    THEN (change_ratio_xsec_count - change_ratio_xsec_rank_asc + 1)::DOUBLE
                         / change_ratio_xsec_count
                    ELSE NULL
                END AS change_ratio_xsec_percentile,
                CASE
                    WHEN short_to_adv IS NOT NULL
                     AND short_to_adv_xsec_count >= (SELECT min_cross_section FROM params)
                     AND short_to_adv_xsec_stddev > 0
                    THEN (short_to_adv - short_to_adv_xsec_mean) / short_to_adv_xsec_stddev
                    ELSE NULL
                END AS short_to_adv_xsec_zscore
            FROM xsection
            ORDER BY security_id, as_of_date
            """,
            [options.min_cross_section],
        ).df()

    def _build_feature_definitions(self, options: ShortInterestFeatureOptions) -> pd.DataFrame:
        rows = []
        for feature_name, definition in SHORT_INTEREST_FEATURE_DEFINITIONS.items():
            rows.append(
                {
                    "feature_set": options.feature_set,
                    "feature_name": feature_name,
                    "description": definition["description"],
                    "expression_sql": definition["expression_sql"],
                    "input_tables_json": json_dumps(["finra_short_interest"]),
                    "lookback_days": definition["lookback_days"],
                    "is_point_in_time_safe": True,
                    "available_at_policy": "Feature available after FINRA source row available_at; loader models this conservatively as settlement_date plus 10 days at 22:00.",
                    "owner": "atx-db",
                    "source": options.source,
                }
            )
        return pd.DataFrame(rows)

    def _build_feature_values(
        self,
        panel: pd.DataFrame,
        options: ShortInterestFeatureOptions,
    ) -> pd.DataFrame:
        columns = [
            "feature_set",
            "feature_name",
            "security_id",
            "symbol",
            "as_of_date",
            "value",
            "input_hash",
            "available_at",
            "source",
            "run_id",
        ]
        if panel.empty:
            return pd.DataFrame(columns=columns)
        frames = []
        for feature_name, column in FEATURE_COLUMNS.items():
            values = panel[["security_id", "symbol", "as_of_date", "available_at", column]].rename(
                columns={column: "value"}
            )
            values = values.dropna(subset=["value"])
            values["feature_set"] = options.feature_set
            values["feature_name"] = feature_name
            values["source"] = options.source
            values["run_id"] = options.run_id
            frames.append(values)
        if not frames:
            return pd.DataFrame(columns=columns)
        features = pd.concat(frames, ignore_index=True)
        features["input_hash"] = _feature_hashes(features)
        return features[columns]

    def _build_manifest(
        self,
        panel: pd.DataFrame,
        features: pd.DataFrame,
        options: ShortInterestFeatureOptions,
    ) -> pd.DataFrame:
        symbols = sorted(panel["symbol"].dropna().astype(str).unique().tolist()) if not panel.empty else []
        feature_names = sorted(SHORT_INTEREST_FEATURE_DEFINITIONS)
        return pd.DataFrame(
            [
                {
                    "manifest_id": _manifest_id(options.feature_set, options.run_id),
                    "feature_set": options.feature_set,
                    "run_id": options.run_id,
                    "symbols_json": json_dumps(symbols),
                    "feature_names_json": json_dumps(feature_names),
                    "input_tables_json": json_dumps(["finra_short_interest"]),
                    "input_min_as_of_date": None if panel.empty else panel["as_of_date"].min(),
                    "input_max_as_of_date": None if panel.empty else panel["as_of_date"].max(),
                    "input_row_count": int(len(panel)),
                    "output_min_as_of_date": None if features.empty else features["as_of_date"].min(),
                    "output_max_as_of_date": None if features.empty else features["as_of_date"].max(),
                    "output_row_count": int(len(features)),
                    "feature_count": int(len(feature_names)),
                    "min_available_at": None if features.empty else features["available_at"].min(),
                    "max_available_at": None if features.empty else features["available_at"].max(),
                    "params_json": json_dumps(
                        {
                            "feature_set": options.feature_set,
                            "min_cross_section": options.min_cross_section,
                        }
                    ),
                    "source": options.source,
                }
            ]
        )
