from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import uuid
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check, symbol_key


SOURCE_NAME = "atx-db feature engine"

FEATURE_DEFINITIONS = {
    "ret_1d": {
        "description": "One-trading-day close-to-close simple return.",
        "expression_sql": "close / lag(close, 1) over (partition by security_id order by trade_date) - 1",
        "lookback_days": 1,
    },
    "mom_5d": {
        "description": "Five-trading-day close-to-close momentum.",
        "expression_sql": "close / lag(close, 5) over (partition by security_id order by trade_date) - 1",
        "lookback_days": 5,
    },
    "mom_21d": {
        "description": "Twenty-one-trading-day close-to-close momentum.",
        "expression_sql": "close / lag(close, 21) over (partition by security_id order by trade_date) - 1",
        "lookback_days": 21,
    },
    "mom_63d": {
        "description": "Sixty-three-trading-day close-to-close momentum.",
        "expression_sql": "close / lag(close, 63) over (partition by security_id order by trade_date) - 1",
        "lookback_days": 63,
    },
    "vol_21d": {
        "description": "Trailing 21-row sample standard deviation of daily simple returns.",
        "expression_sql": "stddev_samp(ret_1d) over (partition by security_id order by as_of_date rows between 20 preceding and current row)",
        "lookback_days": 21,
    },
    "adv_21d": {
        "description": "Trailing 21-row average dollar volume.",
        "expression_sql": "avg(close * volume) over (partition by security_id order by as_of_date rows between 20 preceding and current row)",
        "lookback_days": 21,
    },
    "dollar_volume": {
        "description": "Daily close multiplied by daily share volume.",
        "expression_sql": "close * volume",
        "lookback_days": 0,
    },
}


FUNDAMENTAL_FEATURE_SOURCE_NAME = "atx-db fundamental feature engine"


FUNDAMENTAL_FEATURE_DEFINITIONS = {
    "fund_assets": {
        "description": "Latest reported total assets from SEC companyfacts as of the filing availability date.",
        "expression_sql": "latest PIT Assets value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_liabilities": {
        "description": "Latest reported total liabilities from SEC companyfacts.",
        "expression_sql": "latest PIT Liabilities value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_stockholders_equity": {
        "description": "Latest reported stockholders' equity from SEC companyfacts.",
        "expression_sql": "latest PIT StockholdersEquity value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_shares_outstanding": {
        "description": "Latest reported common shares outstanding from SEC companyfacts.",
        "expression_sql": "latest PIT EntityCommonStockSharesOutstanding value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_revenue_reported": {
        "description": "Latest reported revenue concept, preferring ASC 606 revenue over legacy Revenues.",
        "expression_sql": "latest PIT RevenueFromContractWithCustomerExcludingAssessedTax or Revenues value",
        "lookback_days": 0,
    },
    "fund_net_income_reported": {
        "description": "Latest reported net income/loss from SEC companyfacts.",
        "expression_sql": "latest PIT NetIncomeLoss value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_operating_income_reported": {
        "description": "Latest reported operating income/loss from SEC companyfacts.",
        "expression_sql": "latest PIT OperatingIncomeLoss value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_eps_diluted_reported": {
        "description": "Latest reported diluted EPS from SEC companyfacts.",
        "expression_sql": "latest PIT EarningsPerShareDiluted value from fundamental_points",
        "lookback_days": 0,
    },
    "fund_revenue_ttm": {
        "description": "Latest PIT trailing-twelve-month revenue from normalized SEC statement points.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'revenue'",
        "lookback_days": 0,
    },
    "fund_net_income_ttm": {
        "description": "Latest PIT trailing-twelve-month net income from normalized SEC statement points.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'net_income'",
        "lookback_days": 0,
    },
    "fund_operating_income_ttm": {
        "description": "Latest PIT trailing-twelve-month operating income from normalized SEC statement points.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'operating_income'",
        "lookback_days": 0,
    },
    "fund_operating_cash_flow_ttm": {
        "description": "Latest PIT trailing-twelve-month operating cash flow from normalized SEC cash-flow statement points.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'operating_cash_flow'",
        "lookback_days": 0,
    },
    "fund_capex_ttm": {
        "description": "Latest PIT trailing-twelve-month capital expenditures, signed as a cash outflow.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'capital_expenditures'",
        "lookback_days": 0,
    },
    "fund_dividends_paid_ttm": {
        "description": "Latest PIT trailing-twelve-month dividends paid, signed as a cash outflow.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'dividends_paid'",
        "lookback_days": 0,
    },
    "fund_share_repurchases_ttm": {
        "description": "Latest PIT trailing-twelve-month share repurchases, signed as a cash outflow.",
        "expression_sql": "latest PIT fundamental_ttm_points.ttm_value where canonical_metric = 'share_repurchases'",
        "lookback_days": 0,
    },
    "fund_free_cash_flow_ttm": {
        "description": "Trailing-twelve-month free cash flow as operating cash flow plus signed capital expenditures.",
        "expression_sql": "fund_operating_cash_flow_ttm + fund_capex_ttm",
        "lookback_days": 0,
    },
    "fund_revenue_ttm_yoy_growth": {
        "description": "Year-over-year growth in PIT trailing-twelve-month revenue.",
        "expression_sql": "fund_revenue_ttm / nullif(prior_year_fund_revenue_ttm, 0) - 1",
        "lookback_days": 365,
    },
    "fund_net_income_ttm_yoy_growth": {
        "description": "Year-over-year growth in PIT trailing-twelve-month net income.",
        "expression_sql": "fund_net_income_ttm / nullif(prior_year_fund_net_income_ttm, 0) - 1",
        "lookback_days": 365,
    },
    "fund_operating_income_ttm_yoy_growth": {
        "description": "Year-over-year growth in PIT trailing-twelve-month operating income.",
        "expression_sql": "fund_operating_income_ttm / nullif(prior_year_fund_operating_income_ttm, 0) - 1",
        "lookback_days": 365,
    },
    "fund_operating_cash_flow_ttm_yoy_growth": {
        "description": "Year-over-year growth in PIT trailing-twelve-month operating cash flow.",
        "expression_sql": "fund_operating_cash_flow_ttm / nullif(prior_year_fund_operating_cash_flow_ttm, 0) - 1",
        "lookback_days": 365,
    },
    "fund_free_cash_flow_ttm_yoy_growth": {
        "description": "Year-over-year growth in PIT trailing-twelve-month free cash flow.",
        "expression_sql": "fund_free_cash_flow_ttm / nullif(prior_year_fund_free_cash_flow_ttm, 0) - 1",
        "lookback_days": 365,
    },
    "fund_liabilities_to_assets": {
        "description": "Latest reported liabilities divided by latest reported assets.",
        "expression_sql": "fund_liabilities / nullif(fund_assets, 0)",
        "lookback_days": 0,
    },
    "fund_equity_to_assets": {
        "description": "Latest reported stockholders' equity divided by latest reported assets.",
        "expression_sql": "fund_stockholders_equity / nullif(fund_assets, 0)",
        "lookback_days": 0,
    },
    "fund_roa_reported": {
        "description": "Latest reported net income divided by latest reported assets.",
        "expression_sql": "fund_net_income_reported / nullif(fund_assets, 0)",
        "lookback_days": 0,
    },
    "fund_net_margin_reported": {
        "description": "Latest reported net income divided by latest reported revenue.",
        "expression_sql": "fund_net_income_reported / nullif(fund_revenue_reported, 0)",
        "lookback_days": 0,
    },
    "fund_operating_margin_reported": {
        "description": "Latest reported operating income divided by latest reported revenue.",
        "expression_sql": "fund_operating_income_reported / nullif(fund_revenue_reported, 0)",
        "lookback_days": 0,
    },
    "fund_market_cap": {
        "description": "Latest available daily close multiplied by latest reported common shares outstanding.",
        "expression_sql": "latest PIT equity_daily_bars.close * fund_shares_outstanding",
        "lookback_days": 0,
    },
    "fund_book_to_market": {
        "description": "Latest reported stockholders' equity divided by PIT market capitalization.",
        "expression_sql": "fund_stockholders_equity / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_price_to_book": {
        "description": "PIT market capitalization divided by latest reported stockholders' equity.",
        "expression_sql": "fund_market_cap / nullif(fund_stockholders_equity, 0)",
        "lookback_days": 0,
    },
    "fund_earnings_yield_reported": {
        "description": "Latest reported net income divided by PIT market capitalization.",
        "expression_sql": "fund_net_income_reported / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_price_to_sales_reported": {
        "description": "PIT market capitalization divided by latest reported revenue.",
        "expression_sql": "fund_market_cap / nullif(fund_revenue_reported, 0)",
        "lookback_days": 0,
    },
    "fund_price_to_sales_ttm": {
        "description": "PIT market capitalization divided by trailing-twelve-month revenue.",
        "expression_sql": "fund_market_cap / nullif(fund_revenue_ttm, 0)",
        "lookback_days": 0,
    },
    "fund_fcf_yield_ttm": {
        "description": "Trailing-twelve-month free cash flow divided by PIT market capitalization.",
        "expression_sql": "fund_free_cash_flow_ttm / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_operating_cash_flow_yield_ttm": {
        "description": "Trailing-twelve-month operating cash flow divided by PIT market capitalization.",
        "expression_sql": "fund_operating_cash_flow_ttm / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_buyback_yield_ttm": {
        "description": "Trailing-twelve-month share repurchase cash outflow divided by PIT market capitalization.",
        "expression_sql": "-fund_share_repurchases_ttm / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_dividend_yield_ttm": {
        "description": "Trailing-twelve-month dividend cash outflow divided by PIT market capitalization.",
        "expression_sql": "-fund_dividends_paid_ttm / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_shareholder_yield_ttm": {
        "description": "Trailing-twelve-month buyback plus dividend cash outflow divided by PIT market capitalization.",
        "expression_sql": "-(coalesce(fund_share_repurchases_ttm, 0) + coalesce(fund_dividends_paid_ttm, 0)) / nullif(fund_market_cap, 0)",
        "lookback_days": 0,
    },
    "fund_sales_to_assets_reported": {
        "description": "Latest reported revenue divided by latest reported total assets.",
        "expression_sql": "fund_revenue_reported / nullif(fund_assets, 0)",
        "lookback_days": 0,
    },
    "fund_operating_roa_reported": {
        "description": "Latest reported operating income divided by latest reported total assets.",
        "expression_sql": "fund_operating_income_reported / nullif(fund_assets, 0)",
        "lookback_days": 0,
    },
    "fund_roe_reported": {
        "description": "Latest reported net income divided by latest reported stockholders' equity.",
        "expression_sql": "fund_net_income_reported / nullif(fund_stockholders_equity, 0)",
        "lookback_days": 0,
    },
    "fund_liabilities_to_equity": {
        "description": "Latest reported liabilities divided by latest reported stockholders' equity.",
        "expression_sql": "fund_liabilities / nullif(fund_stockholders_equity, 0)",
        "lookback_days": 0,
    },
    "fund_fcf_margin_ttm": {
        "description": "Trailing-twelve-month free cash flow divided by trailing-twelve-month revenue.",
        "expression_sql": "fund_free_cash_flow_ttm / nullif(fund_revenue_ttm, 0)",
        "lookback_days": 0,
    },
    "fund_operating_cash_flow_margin_ttm": {
        "description": "Trailing-twelve-month operating cash flow divided by trailing-twelve-month revenue.",
        "expression_sql": "fund_operating_cash_flow_ttm / nullif(fund_revenue_ttm, 0)",
        "lookback_days": 0,
    },
    "fund_capex_to_sales_ttm": {
        "description": "Trailing-twelve-month capital expenditures cash outflow divided by trailing-twelve-month revenue.",
        "expression_sql": "-fund_capex_ttm / nullif(fund_revenue_ttm, 0)",
        "lookback_days": 0,
    },
    "fund_cash_conversion_ttm": {
        "description": "Trailing-twelve-month operating cash flow divided by trailing-twelve-month net income.",
        "expression_sql": "fund_operating_cash_flow_ttm / nullif(fund_net_income_ttm, 0)",
        "lookback_days": 0,
    },
    "fund_revision_events_1y": {
        "description": "Count of visible SEC companyfacts accession revisions during the trailing year.",
        "expression_sql": "count fundamental_fact_revisions rows with revision_sequence > 1 and available_at in trailing 365 days",
        "lookback_days": 365,
    },
    "fund_value_change_revisions_1y": {
        "description": "Count of visible SEC companyfacts value-changing revisions during the trailing year.",
        "expression_sql": "count fundamental_fact_revisions rows with is_value_changed and available_at in trailing 365 days",
        "lookback_days": 365,
    },
    "fund_value_change_revision_ratio_1y": {
        "description": "Share of trailing-year SEC accession revisions that changed the fact value.",
        "expression_sql": "fund_value_change_revisions_1y / nullif(fund_revision_events_1y, 0)",
        "lookback_days": 365,
    },
    "fund_days_since_last_value_change_revision": {
        "description": "Days since the latest visible value-changing SEC companyfacts revision.",
        "expression_sql": "date_diff('day', latest visible is_value_changed available_at, feature available_at)",
        "lookback_days": 0,
    },
}


def _feature_hashes(features: pd.DataFrame) -> list[str]:
    hashes: list[str] = []
    for feature_set, feature_name, security_id, as_of_date, value in zip(
        features["feature_set"],
        features["feature_name"],
        features["security_id"],
        features["as_of_date"],
        features["value"],
        strict=True,
    ):
        value_text = "" if pd.isna(value) else f"{float(value):.17g}"
        payload = f"{feature_set}|{feature_name}|{security_id}|{as_of_date}|{value_text}"
        hashes.append(hashlib.sha256(payload.encode("utf-8")).hexdigest())
    return hashes


def manifest_id_for(feature_set: str, run_id: str | None, symbols: list[str]) -> str:
    payload = "|".join([feature_set, run_id or "", ",".join(symbols)])
    return str(uuid.uuid5(uuid.NAMESPACE_URL, payload))


def _json_list(value: object) -> list[str]:
    if isinstance(value, list):
        return sorted({str(item) for item in value if str(item)})
    if value is None or pd.isna(value):
        return []
    try:
        loaded = json.loads(str(value))
    except json.JSONDecodeError:
        return []
    if not isinstance(loaded, list):
        return []
    return sorted({str(item) for item in loaded if str(item)})


def _feature_version_label(feature_set: str) -> str | None:
    match = re.search(r"(?:^|_)(v\d+)$", feature_set)
    return match.group(1) if match else None


def _feature_family(feature_set: str) -> str:
    version = _feature_version_label(feature_set)
    if not version:
        return feature_set
    return feature_set[: -len(version)].rstrip("_") or feature_set


def _dependency_id(
    feature_set: str,
    feature_name: str,
    dependency_type: str,
    dependency_name: str,
) -> str:
    payload = "|".join([feature_set, feature_name, dependency_type, dependency_name])
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def refresh_feature_lineage(store: DuckDBStore) -> dict[str, int]:
    """Refresh feature-set catalog and dependency edges from feature definitions."""

    definitions = store.con.execute(
        """
        SELECT
            feature_set,
            feature_name,
            description,
            expression_sql,
            input_tables_json,
            lookback_days,
            is_point_in_time_safe,
            owner,
            source
        FROM feature_definitions
        ORDER BY feature_set, feature_name
        """
    ).df()
    if definitions.empty:
        with store.transaction():
            store.con.execute("DELETE FROM feature_dependency_edges")
            store.con.execute("DELETE FROM feature_set_catalog")
        return {"feature_sets": 0, "dependency_edges": 0}

    edges: list[dict[str, object]] = []
    catalog_rows: list[dict[str, object]] = []
    feature_names_by_set = {
        feature_set: sorted(group["feature_name"].astype(str).tolist())
        for feature_set, group in definitions.groupby("feature_set", sort=True)
    }
    for row in definitions.itertuples(index=False):
        feature_set = str(row.feature_set)
        feature_name = str(row.feature_name)
        expression_sql = "" if pd.isna(row.expression_sql) else str(row.expression_sql)
        lookback_days = None if pd.isna(row.lookback_days) else int(row.lookback_days)
        source = str(row.source)
        for table_name in _json_list(row.input_tables_json):
            edges.append(
                {
                    "dependency_id": _dependency_id(feature_set, feature_name, "source_table", table_name),
                    "feature_set": feature_set,
                    "feature_name": feature_name,
                    "dependency_type": "source_table",
                    "dependency_name": table_name,
                    "dependency_feature_set": None,
                    "dependency_feature_name": None,
                    "dependency_depth": 1,
                    "expression_sql": expression_sql,
                    "lookback_days": lookback_days,
                    "is_direct": True,
                    "source": source,
                }
            )
        for candidate in feature_names_by_set[feature_set]:
            if candidate == feature_name:
                continue
            pattern = rf"(?<![A-Za-z0-9_]){re.escape(candidate)}(?![A-Za-z0-9_])"
            if re.search(pattern, expression_sql):
                edges.append(
                    {
                        "dependency_id": _dependency_id(feature_set, feature_name, "derived_feature", candidate),
                        "feature_set": feature_set,
                        "feature_name": feature_name,
                        "dependency_type": "derived_feature",
                        "dependency_name": candidate,
                        "dependency_feature_set": feature_set,
                        "dependency_feature_name": candidate,
                        "dependency_depth": 1,
                        "expression_sql": expression_sql,
                        "lookback_days": lookback_days,
                        "is_direct": True,
                        "source": source,
                    }
                )

    edges_frame = pd.DataFrame(edges).drop_duplicates(subset=["dependency_id"]) if edges else pd.DataFrame()
    for feature_set, group in definitions.groupby("feature_set", sort=True):
        feature_set = str(feature_set)
        feature_names = sorted(group["feature_name"].astype(str).tolist())
        set_edges = edges_frame[edges_frame["feature_set"] == feature_set] if not edges_frame.empty else pd.DataFrame()
        input_tables = sorted(
            {
                item
                for value in group["input_tables_json"].tolist()
                for item in _json_list(value)
            }
        )
        owners = sorted({str(value) for value in group["owner"].dropna().tolist() if str(value)})
        sources = sorted({str(value) for value in group["source"].dropna().tolist() if str(value)})
        descriptions = group["description"].dropna()
        catalog_rows.append(
            {
                "feature_set": feature_set,
                "version_label": _feature_version_label(feature_set),
                "feature_family": _feature_family(feature_set),
                "description": f"{feature_set} feature set with {len(feature_names)} definitions. "
                + (str(descriptions.iloc[0]) if not descriptions.empty else ""),
                "feature_count": int(len(feature_names)),
                "dependency_count": int(len(set_edges)),
                "source_table_count": int(
                    0
                    if set_edges.empty
                    else set_edges[set_edges["dependency_type"] == "source_table"]["dependency_name"].nunique()
                ),
                "derived_feature_dependency_count": int(
                    0 if set_edges.empty else (set_edges["dependency_type"] == "derived_feature").sum()
                ),
                "max_lookback_days": None if group["lookback_days"].dropna().empty else int(group["lookback_days"].max()),
                "input_tables_json": json_dumps(input_tables),
                "feature_names_json": json_dumps(feature_names),
                "point_in_time_safe": bool(group["is_point_in_time_safe"].all()),
                "owner": owners[0] if owners else None,
                "source": sources[0] if sources else SOURCE_NAME,
            }
        )
    catalog_frame = pd.DataFrame(catalog_rows)

    with store.transaction():
        store.con.execute("DELETE FROM feature_dependency_edges")
        store.con.execute("DELETE FROM feature_set_catalog")
        if not catalog_frame.empty:
            insert_frame(store, catalog_frame, "feature_set_catalog", "feature_set_catalog_insert")
        if not edges_frame.empty:
            insert_frame(store, edges_frame, "feature_dependency_edges", "feature_dependency_edges_insert")
    return {"feature_sets": int(len(catalog_frame)), "dependency_edges": int(len(edges_frame))}


@dataclass(frozen=True)
class FeatureBuildOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    feature_set: str = "equity_daily_v1"
    min_rows: int = 40
    run_id: str | None = None


@dataclass(frozen=True)
class FundamentalFeatureBuildOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    feature_set: str = "sec_fundamentals_v1"
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None


class EquityDailyFeatureDataset(Dataset):
    dataset_id = "equity_daily_features"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FeatureBuildOptions) -> DatasetLoadResult:
        symbols = sorted({symbol_key(symbol) for symbol in options.symbols})
        params = pd.DataFrame({"symbol": symbols})
        store.con.register("feature_symbol_filter", params)
        try:
            panel = store.con.execute(
                """
                WITH base AS (
                    SELECT
                        b.security_id,
                        b.symbol,
                        b.trade_date AS as_of_date,
                        b.close,
                        b.volume,
                        b.close * b.volume AS dollar_volume,
                        b.close / lag(b.close, 1) OVER w - 1.0 AS ret_1d,
                        b.close / lag(b.close, 5) OVER w - 1.0 AS mom_5d,
                        b.close / lag(b.close, 21) OVER w - 1.0 AS mom_21d,
                        b.close / lag(b.close, 63) OVER w - 1.0 AS mom_63d,
                        count(*) OVER (
                            PARTITION BY b.security_id
                            ORDER BY b.trade_date
                            ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                        ) AS row_number_in_history
                    FROM equity_daily_bars b
                    JOIN feature_symbol_filter f ON f.symbol = b.symbol
                    WINDOW w AS (PARTITION BY b.security_id ORDER BY b.trade_date)
                )
                SELECT
                    *,
                    stddev_samp(ret_1d) OVER (
                        PARTITION BY security_id
                        ORDER BY as_of_date
                        ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                    ) AS vol_21d,
                    avg(dollar_volume) OVER (
                        PARTITION BY security_id
                        ORDER BY as_of_date
                        ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                    ) AS adv_21d
                FROM base
                WHERE row_number_in_history >= ?
                ORDER BY security_id, as_of_date
                """,
                [options.min_rows],
            ).df()
        finally:
            store.con.unregister("feature_symbol_filter")

        if panel.empty:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=SOURCE_NAME,
                details={"symbols": symbols, "feature_set": options.feature_set},
            )

        rows = []
        feature_columns = list(FEATURE_DEFINITIONS)
        for column in feature_columns:
            values = panel[["security_id", "symbol", "as_of_date", column]].rename(columns={column: "value"})
            values = values.dropna(subset=["value"])
            values["feature_set"] = options.feature_set
            values["feature_name"] = column
            values["input_hash"] = None
            values["available_at"] = pd.to_datetime(values["as_of_date"]) + pd.Timedelta(hours=22)
            values["source"] = SOURCE_NAME
            values["run_id"] = options.run_id
            rows.append(values)
        features = pd.concat(rows, ignore_index=True)
        features["input_hash"] = _feature_hashes(features)
        features = features[
            [
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
        ]
        manifest = self._build_manifest(
            panel=panel,
            features=features,
            symbols=symbols,
            feature_columns=feature_columns,
            options=options,
        )
        store.con.register("feature_values_load", features)
        try:
            with store.transaction():
                self._upsert_feature_definitions(store, options, feature_columns)
                store.con.execute(
                    """
                    DELETE FROM feature_values AS dst
                    USING feature_values_load AS src
                    WHERE dst.feature_set = src.feature_set
                      AND dst.feature_name = src.feature_name
                      AND dst.security_id = src.security_id
                      AND dst.as_of_date = src.as_of_date
                    """
                )
                insert_frame(store, features, "feature_values", "feature_values_insert")
                self._upsert_feature_manifest(store, manifest)
        finally:
            store.con.unregister("feature_values_load")

        lineage = refresh_feature_lineage(store)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="feature_values",
            check_name="rows_loaded",
            status="passed" if len(features) > 0 else "warning",
            observed_value=float(len(features)),
            threshold_value=1.0,
            details={"symbols": symbols, "feature_set": options.feature_set},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(len(features)),
            source=SOURCE_NAME,
            details={
                "symbols": symbols,
                "feature_set": options.feature_set,
                "features": feature_columns,
                "feature_lineage": lineage,
            },
        )

    def _build_manifest(
        self,
        *,
        panel: pd.DataFrame,
        features: pd.DataFrame,
        symbols: list[str],
        feature_columns: list[str],
        options: FeatureBuildOptions,
    ) -> pd.DataFrame:
        return pd.DataFrame(
            [
                {
                    "manifest_id": manifest_id_for(options.feature_set, options.run_id, symbols),
                    "feature_set": options.feature_set,
                    "run_id": options.run_id,
                    "symbols_json": json_dumps(symbols),
                    "feature_names_json": json_dumps(feature_columns),
                    "input_tables_json": json_dumps(["equity_daily_bars"]),
                    "input_min_as_of_date": panel["as_of_date"].min(),
                    "input_max_as_of_date": panel["as_of_date"].max(),
                    "input_row_count": int(len(panel)),
                    "output_min_as_of_date": features["as_of_date"].min(),
                    "output_max_as_of_date": features["as_of_date"].max(),
                    "output_row_count": int(len(features)),
                    "feature_count": int(features["feature_name"].nunique()),
                    "min_available_at": features["available_at"].min(),
                    "max_available_at": features["available_at"].max(),
                    "params_json": json_dumps(
                        {
                            "symbols": symbols,
                            "feature_set": options.feature_set,
                            "min_rows": options.min_rows,
                        }
                    ),
                    "source": SOURCE_NAME,
                }
            ]
        )

    def _upsert_feature_manifest(self, store: DuckDBStore, manifest: pd.DataFrame) -> None:
        if manifest.empty:
            return
        store.con.register("feature_build_manifest_load", manifest)
        try:
            store.con.execute(
                """
                DELETE FROM feature_build_manifests AS dst
                USING feature_build_manifest_load AS src
                WHERE dst.manifest_id = src.manifest_id
                """
            )
            insert_frame(
                store,
                manifest,
                "feature_build_manifests",
                "feature_build_manifest_insert",
            )
        finally:
            store.con.unregister("feature_build_manifest_load")

    def _upsert_feature_definitions(
        self,
        store: DuckDBStore,
        options: FeatureBuildOptions,
        feature_columns: list[str],
    ) -> None:
        definitions = []
        for feature_name in feature_columns:
            definition = FEATURE_DEFINITIONS[feature_name]
            definitions.append(
                {
                    "feature_set": options.feature_set,
                    "feature_name": feature_name,
                    "description": definition["description"],
                    "expression_sql": definition["expression_sql"],
                    "input_tables_json": json_dumps(["equity_daily_bars"]),
                    "lookback_days": definition["lookback_days"],
                    "is_point_in_time_safe": True,
                    "available_at_policy": "Feature available after all same-date input bars are available, modeled as as_of_date 22:00.",
                    "owner": "atx-db",
                    "source": SOURCE_NAME,
                }
            )
        frame = pd.DataFrame(definitions)
        if frame.empty:
            return
        store.con.register("feature_definitions_load", frame)
        try:
            store.con.execute(
                """
                DELETE FROM feature_definitions AS dst
                USING feature_definitions_load AS src
                WHERE dst.feature_set = src.feature_set
                  AND dst.feature_name = src.feature_name
                """
            )
            insert_frame(store, frame, "feature_definitions", "feature_definitions_insert")
        finally:
            store.con.unregister("feature_definitions_load")


class FundamentalFeatureDataset(Dataset):
    dataset_id = "sec_fundamental_features"
    source_name = FUNDAMENTAL_FEATURE_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalFeatureBuildOptions) -> DatasetLoadResult:
        symbols = sorted({symbol_key(symbol) for symbol in options.symbols})
        params = pd.DataFrame({"symbol": symbols})
        store.con.register("fund_feature_symbol_filter", params)
        try:
            date_filters: list[str] = []
            date_params: list[dt.date] = []
            if options.start_date is not None:
                date_filters.append("f.as_of_date >= ?")
                date_params.append(options.start_date)
            if options.end_date is not None:
                date_filters.append("f.as_of_date <= ?")
                date_params.append(options.end_date)
            date_where_sql = ""
            if date_filters:
                date_where_sql = " AND " + " AND ".join(date_filters)
            revision_date_filters: list[str] = []
            revision_date_params: list[dt.date] = []
            if options.start_date is not None:
                revision_date_filters.append("CAST(r.available_at AS DATE) >= ?")
                revision_date_params.append(options.start_date - dt.timedelta(days=365))
            if options.end_date is not None:
                revision_date_filters.append("CAST(r.available_at AS DATE) <= ?")
                revision_date_params.append(options.end_date)
            revision_date_where_sql = ""
            if revision_date_filters:
                revision_date_where_sql = " AND " + " AND ".join(revision_date_filters)

            input_row_count = int(
                store.con.execute(
                    f"""
                    SELECT sum(row_count)
                    FROM (
                        SELECT count(*) AS row_count
                        FROM fundamental_points f
                        JOIN fund_feature_symbol_filter sf ON sf.symbol = f.symbol
                        WHERE f.available_at IS NOT NULL
                          AND f.value IS NOT NULL
                          {date_where_sql}
                        UNION ALL
                        SELECT count(*) AS row_count
                        FROM fundamental_ttm_points f
                        JOIN fund_feature_symbol_filter sf ON sf.symbol = f.symbol
                        WHERE f.available_at IS NOT NULL
                          AND f.ttm_value IS NOT NULL
                          {date_where_sql}
                        UNION ALL
                        SELECT count(*) AS row_count
                        FROM fundamental_fact_revisions r
                        JOIN (
                            SELECT DISTINCT fp.security_id
                            FROM fundamental_points fp
                            JOIN fund_feature_symbol_filter sf ON sf.symbol = fp.symbol
                        ) rs
                          ON rs.security_id = r.security_id
                        WHERE r.available_at IS NOT NULL
                          {revision_date_where_sql}
                    )
                    """,
                    date_params + date_params + revision_date_params,
                ).fetchone()[0]
            )
            panel = store.con.execute(
                f"""
                WITH snapshots AS (
                    SELECT
                        f.security_id,
                        any_value(f.symbol) AS symbol,
                        f.as_of_date,
                        max(f.available_at) AS available_at,
                        count(*) AS input_fact_count
                    FROM fundamental_points f
                    JOIN fund_feature_symbol_filter sf ON sf.symbol = f.symbol
                    WHERE f.available_at IS NOT NULL
                      AND f.value IS NOT NULL
                      {date_where_sql}
                    GROUP BY f.security_id, f.as_of_date
                ),
                eligible AS (
                    SELECT
                        s.security_id,
                        s.symbol,
                        s.as_of_date,
                        s.available_at,
                        s.input_fact_count,
                        CASE
                            WHEN f.metric = 'Assets' THEN 'assets'
                            WHEN f.metric = 'Liabilities' THEN 'liabilities'
                            WHEN f.metric = 'StockholdersEquity' THEN 'stockholders_equity'
                            WHEN f.metric = 'EntityCommonStockSharesOutstanding' THEN 'shares_outstanding'
                            WHEN f.metric IN (
                                'RevenueFromContractWithCustomerExcludingAssessedTax',
                                'Revenues'
                            ) THEN 'revenue_reported'
                            WHEN f.metric = 'NetIncomeLoss' THEN 'net_income_reported'
                            WHEN f.metric = 'OperatingIncomeLoss' THEN 'operating_income_reported'
                            WHEN f.metric = 'EarningsPerShareDiluted' THEN 'eps_diluted_reported'
                        END AS feature_metric,
                        CASE
                            WHEN f.metric = 'RevenueFromContractWithCustomerExcludingAssessedTax' THEN 0
                            WHEN f.metric = 'Revenues' THEN 1
                            ELSE 0
                        END AS concept_rank,
                        f.value,
                        f.period_end,
                        f.as_of_date AS fact_as_of_date,
                        f.available_at AS fact_available_at,
                        f.source_loaded_at,
                        f.form,
                        f.accession_number
                    FROM snapshots s
                    JOIN fundamental_points f
                      ON f.security_id = s.security_id
                     AND f.as_of_date <= s.as_of_date
                     AND (f.available_at IS NULL OR f.available_at <= s.available_at)
                    WHERE f.value IS NOT NULL
                      AND f.metric IN (
                          'Assets',
                          'Liabilities',
                          'StockholdersEquity',
                          'EntityCommonStockSharesOutstanding',
                          'RevenueFromContractWithCustomerExcludingAssessedTax',
                          'Revenues',
                          'NetIncomeLoss',
                          'OperatingIncomeLoss',
                          'EarningsPerShareDiluted'
                          )
                ),
                ttm_eligible AS (
                    SELECT
                        s.security_id,
                        s.symbol,
                        s.as_of_date,
                        s.available_at,
                        s.input_fact_count,
                        CASE
                            WHEN t.canonical_metric = 'revenue' THEN 'revenue_ttm'
                            WHEN t.canonical_metric = 'net_income' THEN 'net_income_ttm'
                            WHEN t.canonical_metric = 'operating_income' THEN 'operating_income_ttm'
                            WHEN t.canonical_metric = 'operating_cash_flow' THEN 'operating_cash_flow_ttm'
                            WHEN t.canonical_metric = 'capital_expenditures' THEN 'capex_ttm'
                            WHEN t.canonical_metric = 'dividends_paid' THEN 'dividends_paid_ttm'
                            WHEN t.canonical_metric = 'share_repurchases' THEN 'share_repurchases_ttm'
                        END AS feature_metric,
                        0 AS concept_rank,
                        t.ttm_value AS value,
                        t.ttm_end_date AS period_end,
                        t.as_of_date AS fact_as_of_date,
                        t.available_at AS fact_available_at,
                        t.source_loaded_at,
                        t.form,
                        t.accession_number
                    FROM snapshots s
                    JOIN fundamental_ttm_points t
                      ON t.security_id = s.security_id
                     AND t.as_of_date <= s.as_of_date
                     AND (t.available_at IS NULL OR t.available_at <= s.available_at)
                    WHERE t.ttm_value IS NOT NULL
                      AND t.canonical_metric IN (
                          'revenue',
                          'net_income',
                          'operating_income',
                          'operating_cash_flow',
                          'capital_expenditures',
                          'dividends_paid',
                          'share_repurchases'
                      )
                ),
                ranked AS (
                    SELECT
                        *,
                        row_number() OVER (
                            PARTITION BY security_id, as_of_date, feature_metric
                            ORDER BY period_end DESC NULLS LAST,
                                     fact_as_of_date DESC,
                                     fact_available_at DESC NULLS LAST,
                                     source_loaded_at DESC,
                                     concept_rank,
                                     CASE
                                         WHEN form IN ('10-K', '10-Q') THEN 0
                                         WHEN form LIKE '10-%' THEN 1
                                         ELSE 2
                                     END,
                                     accession_number DESC NULLS LAST
                        ) AS rn
                    FROM (
                        SELECT *
                        FROM eligible
                        UNION ALL
                        SELECT *
                        FROM ttm_eligible
                    )
                    WHERE feature_metric IS NOT NULL
                ),
                latest AS (
                    SELECT *
                    FROM ranked
                    WHERE rn = 1
                ),
                wide AS (
                    SELECT
                        security_id,
                        symbol,
                        as_of_date,
                        available_at,
                        max(input_fact_count) AS input_fact_count,
                        max(CASE WHEN feature_metric = 'assets' THEN value END) AS fund_assets,
                        max(CASE WHEN feature_metric = 'liabilities' THEN value END) AS fund_liabilities,
                        max(CASE WHEN feature_metric = 'stockholders_equity' THEN value END) AS fund_stockholders_equity,
                        max(CASE WHEN feature_metric = 'shares_outstanding' THEN value END) AS fund_shares_outstanding,
                        max(CASE WHEN feature_metric = 'revenue_reported' THEN value END) AS fund_revenue_reported,
                        max(CASE WHEN feature_metric = 'net_income_reported' THEN value END) AS fund_net_income_reported,
                        max(CASE WHEN feature_metric = 'operating_income_reported' THEN value END) AS fund_operating_income_reported,
                        max(CASE WHEN feature_metric = 'eps_diluted_reported' THEN value END) AS fund_eps_diluted_reported,
                        max(CASE WHEN feature_metric = 'revenue_ttm' THEN value END) AS fund_revenue_ttm,
                        max(CASE WHEN feature_metric = 'net_income_ttm' THEN value END) AS fund_net_income_ttm,
                        max(CASE WHEN feature_metric = 'operating_income_ttm' THEN value END) AS fund_operating_income_ttm,
                        max(CASE WHEN feature_metric = 'operating_cash_flow_ttm' THEN value END) AS fund_operating_cash_flow_ttm,
                        max(CASE WHEN feature_metric = 'capex_ttm' THEN value END) AS fund_capex_ttm,
                        max(CASE WHEN feature_metric = 'dividends_paid_ttm' THEN value END) AS fund_dividends_paid_ttm,
                        max(CASE WHEN feature_metric = 'share_repurchases_ttm' THEN value END) AS fund_share_repurchases_ttm
                    FROM latest
                    GROUP BY security_id, symbol, as_of_date, available_at
                ),
                prior_year_candidates AS (
                    SELECT
                        w.security_id,
                        w.as_of_date,
                        py.fund_revenue_ttm AS prior_year_fund_revenue_ttm,
                        py.fund_net_income_ttm AS prior_year_fund_net_income_ttm,
                        py.fund_operating_income_ttm AS prior_year_fund_operating_income_ttm,
                        py.fund_operating_cash_flow_ttm AS prior_year_fund_operating_cash_flow_ttm,
                        py.fund_operating_cash_flow_ttm + py.fund_capex_ttm AS prior_year_fund_free_cash_flow_ttm,
                        row_number() OVER (
                            PARTITION BY w.security_id, w.as_of_date
                            ORDER BY py.as_of_date DESC NULLS LAST,
                                     py.available_at DESC NULLS LAST
                        ) AS rn
                    FROM wide w
                    LEFT JOIN wide py
                      ON py.security_id = w.security_id
                     AND py.as_of_date <= w.as_of_date - INTERVAL 300 DAY
                     AND py.as_of_date >= w.as_of_date - INTERVAL 455 DAY
                     AND py.available_at <= w.available_at
                ),
                prior_year AS (
                    SELECT
                        security_id,
                        as_of_date,
                        prior_year_fund_revenue_ttm,
                        prior_year_fund_net_income_ttm,
                        prior_year_fund_operating_income_ttm,
                        prior_year_fund_operating_cash_flow_ttm,
                        prior_year_fund_free_cash_flow_ttm
                    FROM prior_year_candidates
                    WHERE rn = 1
                ),
                revision_activity AS (
                    SELECT
                        s.security_id,
                        s.as_of_date,
                        sum(CASE WHEN r.revision_sequence > 1 THEN 1 ELSE 0 END) AS fund_revision_events_1y,
                        sum(CASE WHEN coalesce(r.is_value_changed, FALSE) THEN 1 ELSE 0 END) AS fund_value_change_revisions_1y,
                        max(CASE WHEN coalesce(r.is_value_changed, FALSE) THEN r.available_at END) AS latest_value_change_available_at
                    FROM snapshots s
                    LEFT JOIN fundamental_fact_revisions r
                      ON r.security_id = s.security_id
                     AND r.available_at <= s.available_at
                     AND r.available_at > s.available_at - INTERVAL 365 DAY
                    GROUP BY s.security_id, s.as_of_date
                ),
                prices AS (
                    SELECT
                        w.security_id,
                        w.as_of_date,
                        b.close,
                        row_number() OVER (
                            PARTITION BY w.security_id, w.as_of_date
                            ORDER BY b.trade_date DESC,
                                     b.available_at DESC NULLS LAST,
                                     b.source_loaded_at DESC
                        ) AS rn
                    FROM wide w
                    LEFT JOIN equity_daily_bars b
                      ON b.security_id = w.security_id
                     AND b.trade_date <= w.as_of_date
                     AND (b.available_at IS NULL OR b.available_at <= w.available_at)
                ),
                latest_prices AS (
                    SELECT security_id, as_of_date, close
                    FROM prices
                    WHERE rn = 1
                )
                SELECT
                    w.security_id,
                    w.symbol,
                    w.as_of_date,
                    w.available_at,
                    w.input_fact_count,
                    w.fund_assets,
                    w.fund_liabilities,
                    w.fund_stockholders_equity,
                    w.fund_shares_outstanding,
                    w.fund_revenue_reported,
                    w.fund_net_income_reported,
                    w.fund_operating_income_reported,
                    w.fund_eps_diluted_reported,
                    w.fund_revenue_ttm,
                    w.fund_net_income_ttm,
                    w.fund_operating_income_ttm,
                    w.fund_operating_cash_flow_ttm,
                    w.fund_capex_ttm,
                    w.fund_dividends_paid_ttm,
                    w.fund_share_repurchases_ttm,
                    w.fund_operating_cash_flow_ttm + w.fund_capex_ttm AS fund_free_cash_flow_ttm,
                    w.fund_liabilities / nullif(w.fund_assets, 0) AS fund_liabilities_to_assets,
                    w.fund_stockholders_equity / nullif(w.fund_assets, 0) AS fund_equity_to_assets,
                    w.fund_net_income_reported / nullif(w.fund_assets, 0) AS fund_roa_reported,
                    w.fund_net_income_reported / nullif(w.fund_revenue_reported, 0) AS fund_net_margin_reported,
                    w.fund_operating_income_reported / nullif(w.fund_revenue_reported, 0) AS fund_operating_margin_reported,
                    p.close * w.fund_shares_outstanding AS fund_market_cap,
                    w.fund_stockholders_equity / nullif(p.close * w.fund_shares_outstanding, 0) AS fund_book_to_market,
                    (p.close * w.fund_shares_outstanding) / nullif(w.fund_stockholders_equity, 0) AS fund_price_to_book,
                    w.fund_net_income_reported / nullif(p.close * w.fund_shares_outstanding, 0) AS fund_earnings_yield_reported,
                    (p.close * w.fund_shares_outstanding) / nullif(w.fund_revenue_reported, 0) AS fund_price_to_sales_reported,
                    (p.close * w.fund_shares_outstanding) / nullif(w.fund_revenue_ttm, 0) AS fund_price_to_sales_ttm,
                    (w.fund_operating_cash_flow_ttm + w.fund_capex_ttm) / nullif(p.close * w.fund_shares_outstanding, 0) AS fund_fcf_yield_ttm,
                    w.fund_operating_cash_flow_ttm / nullif(p.close * w.fund_shares_outstanding, 0) AS fund_operating_cash_flow_yield_ttm,
                    -w.fund_share_repurchases_ttm / nullif(p.close * w.fund_shares_outstanding, 0) AS fund_buyback_yield_ttm,
                    -w.fund_dividends_paid_ttm / nullif(p.close * w.fund_shares_outstanding, 0) AS fund_dividend_yield_ttm,
                    CASE
                        WHEN w.fund_share_repurchases_ttm IS NULL AND w.fund_dividends_paid_ttm IS NULL THEN NULL
                        ELSE -(coalesce(w.fund_share_repurchases_ttm, 0) + coalesce(w.fund_dividends_paid_ttm, 0))
                            / nullif(p.close * w.fund_shares_outstanding, 0)
                    END AS fund_shareholder_yield_ttm,
                    w.fund_revenue_reported / nullif(w.fund_assets, 0) AS fund_sales_to_assets_reported,
                    w.fund_operating_income_reported / nullif(w.fund_assets, 0) AS fund_operating_roa_reported,
                    w.fund_net_income_reported / nullif(w.fund_stockholders_equity, 0) AS fund_roe_reported,
                    w.fund_liabilities / nullif(w.fund_stockholders_equity, 0) AS fund_liabilities_to_equity,
                    (w.fund_operating_cash_flow_ttm + w.fund_capex_ttm) / nullif(w.fund_revenue_ttm, 0) AS fund_fcf_margin_ttm,
                    w.fund_operating_cash_flow_ttm / nullif(w.fund_revenue_ttm, 0) AS fund_operating_cash_flow_margin_ttm,
                    -w.fund_capex_ttm / nullif(w.fund_revenue_ttm, 0) AS fund_capex_to_sales_ttm,
                    w.fund_operating_cash_flow_ttm / nullif(w.fund_net_income_ttm, 0) AS fund_cash_conversion_ttm,
                    w.fund_revenue_ttm / nullif(py.prior_year_fund_revenue_ttm, 0) - 1 AS fund_revenue_ttm_yoy_growth,
                    w.fund_net_income_ttm / nullif(py.prior_year_fund_net_income_ttm, 0) - 1 AS fund_net_income_ttm_yoy_growth,
                    w.fund_operating_income_ttm / nullif(py.prior_year_fund_operating_income_ttm, 0) - 1 AS fund_operating_income_ttm_yoy_growth,
                    w.fund_operating_cash_flow_ttm / nullif(py.prior_year_fund_operating_cash_flow_ttm, 0) - 1 AS fund_operating_cash_flow_ttm_yoy_growth,
                    (w.fund_operating_cash_flow_ttm + w.fund_capex_ttm) / nullif(py.prior_year_fund_free_cash_flow_ttm, 0) - 1 AS fund_free_cash_flow_ttm_yoy_growth,
                    CAST(coalesce(ra.fund_revision_events_1y, 0) AS DOUBLE) AS fund_revision_events_1y,
                    CAST(coalesce(ra.fund_value_change_revisions_1y, 0) AS DOUBLE) AS fund_value_change_revisions_1y,
                    CAST(coalesce(ra.fund_value_change_revisions_1y, 0) AS DOUBLE) / nullif(CAST(coalesce(ra.fund_revision_events_1y, 0) AS DOUBLE), 0) AS fund_value_change_revision_ratio_1y,
                    CASE
                        WHEN ra.latest_value_change_available_at IS NULL THEN NULL
                        ELSE date_diff('day', ra.latest_value_change_available_at, w.available_at)
                    END AS fund_days_since_last_value_change_revision
                FROM wide w
                LEFT JOIN prior_year py
                  ON py.security_id = w.security_id
                 AND py.as_of_date = w.as_of_date
                LEFT JOIN revision_activity ra
                  ON ra.security_id = w.security_id
                 AND ra.as_of_date = w.as_of_date
                LEFT JOIN latest_prices p
                  ON p.security_id = w.security_id
                 AND p.as_of_date = w.as_of_date
                ORDER BY w.security_id, w.as_of_date
                """,
                date_params,
            ).df()

            if panel.empty:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="feature_values",
                    check_name="rows_loaded",
                    status="warning",
                    observed_value=0.0,
                    threshold_value=1.0,
                    details={"symbols": symbols, "feature_set": options.feature_set},
                )
                return DatasetLoadResult(
                    dataset_id=self.dataset_id,
                    rows_loaded=0,
                    source=FUNDAMENTAL_FEATURE_SOURCE_NAME,
                    details={"symbols": symbols, "feature_set": options.feature_set},
                )

            feature_columns = list(FUNDAMENTAL_FEATURE_DEFINITIONS)
            feature_frames = []
            for column in feature_columns:
                values = panel[["security_id", "symbol", "as_of_date", "available_at", column]].rename(
                    columns={column: "value"}
                )
                values = values.dropna(subset=["value"])
                values["feature_set"] = options.feature_set
                values["feature_name"] = column
                values["input_hash"] = None
                values["source"] = FUNDAMENTAL_FEATURE_SOURCE_NAME
                values["run_id"] = options.run_id
                feature_frames.append(values)
            features = pd.concat(feature_frames, ignore_index=True)
            features["input_hash"] = _feature_hashes(features)
            features = features[
                [
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
            ]
            manifest = self._build_manifest(
                panel=panel,
                features=features,
                symbols=symbols,
                feature_columns=feature_columns,
                input_row_count=input_row_count,
                options=options,
            )
            store.con.register("fundamental_feature_values_load", features)
            try:
                with store.transaction():
                    self._upsert_feature_definitions(store, options, feature_columns)
                    store.con.execute(
                        """
                        DELETE FROM feature_values AS dst
                        USING fund_feature_symbol_filter AS sf
                        WHERE dst.feature_set = ?
                          AND dst.symbol = sf.symbol
                          AND (? IS NULL OR dst.as_of_date >= ?)
                          AND (? IS NULL OR dst.as_of_date <= ?)
                        """,
                        [
                            options.feature_set,
                            options.start_date,
                            options.start_date,
                            options.end_date,
                            options.end_date,
                        ],
                    )
                    insert_frame(
                        store,
                        features,
                        "feature_values",
                        "fundamental_feature_values_insert",
                    )
                    self._upsert_feature_manifest(store, manifest)
            finally:
                store.con.unregister("fundamental_feature_values_load")
        finally:
            store.con.unregister("fund_feature_symbol_filter")

        lineage = refresh_feature_lineage(store)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="feature_values",
            check_name="rows_loaded",
            status="passed" if len(features) > 0 else "warning",
            observed_value=float(len(features)),
            threshold_value=1.0,
            details={"symbols": symbols, "feature_set": options.feature_set},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(len(features)),
            source=FUNDAMENTAL_FEATURE_SOURCE_NAME,
            details={
                "symbols": symbols,
                "feature_set": options.feature_set,
                "features": feature_columns,
                "input_row_count": input_row_count,
                "feature_lineage": lineage,
            },
        )

    def _build_manifest(
        self,
        *,
        panel: pd.DataFrame,
        features: pd.DataFrame,
        symbols: list[str],
        feature_columns: list[str],
        input_row_count: int,
        options: FundamentalFeatureBuildOptions,
    ) -> pd.DataFrame:
        return pd.DataFrame(
            [
                {
                    "manifest_id": manifest_id_for(options.feature_set, options.run_id, symbols),
                    "feature_set": options.feature_set,
                    "run_id": options.run_id,
                    "symbols_json": json_dumps(symbols),
                    "feature_names_json": json_dumps(feature_columns),
                    "input_tables_json": json_dumps(
                        [
                            "fundamental_points",
                            "fundamental_ttm_points",
                            "fundamental_fact_revisions",
                            "equity_daily_bars",
                        ]
                    ),
                    "input_min_as_of_date": panel["as_of_date"].min(),
                    "input_max_as_of_date": panel["as_of_date"].max(),
                    "input_row_count": input_row_count,
                    "output_min_as_of_date": features["as_of_date"].min(),
                    "output_max_as_of_date": features["as_of_date"].max(),
                    "output_row_count": int(len(features)),
                    "feature_count": int(features["feature_name"].nunique()),
                    "min_available_at": features["available_at"].min(),
                    "max_available_at": features["available_at"].max(),
                    "params_json": json_dumps(
                        {
                            "symbols": symbols,
                            "feature_set": options.feature_set,
                            "start_date": options.start_date,
                            "end_date": options.end_date,
                        }
                    ),
                    "source": FUNDAMENTAL_FEATURE_SOURCE_NAME,
                }
            ]
        )

    def _upsert_feature_manifest(self, store: DuckDBStore, manifest: pd.DataFrame) -> None:
        if manifest.empty:
            return
        store.con.register("feature_build_manifest_load", manifest)
        try:
            store.con.execute(
                """
                DELETE FROM feature_build_manifests AS dst
                USING feature_build_manifest_load AS src
                WHERE dst.manifest_id = src.manifest_id
                """
            )
            insert_frame(
                store,
                manifest,
                "feature_build_manifests",
                "feature_build_manifest_insert",
            )
        finally:
            store.con.unregister("feature_build_manifest_load")

    def _upsert_feature_definitions(
        self,
        store: DuckDBStore,
        options: FundamentalFeatureBuildOptions,
        feature_columns: list[str],
    ) -> None:
        definitions = []
        for feature_name in feature_columns:
            definition = FUNDAMENTAL_FEATURE_DEFINITIONS[feature_name]
            definitions.append(
                {
                    "feature_set": options.feature_set,
                    "feature_name": feature_name,
                    "description": definition["description"],
                    "expression_sql": definition["expression_sql"],
                    "input_tables_json": json_dumps(
                        [
                            "fundamental_points",
                            "fundamental_ttm_points",
                            "fundamental_fact_revisions",
                            "equity_daily_bars",
                        ]
                    ),
                    "lookback_days": definition["lookback_days"],
                    "is_point_in_time_safe": True,
                    "available_at_policy": "Feature available only after the SEC filing-date fact, TTM, or revision availability timestamp; same-date market cap uses bars with available_at no later than the feature timestamp.",
                    "owner": "atx-db",
                    "source": FUNDAMENTAL_FEATURE_SOURCE_NAME,
                }
            )
        frame = pd.DataFrame(definitions)
        if frame.empty:
            return
        store.con.register("feature_definitions_load", frame)
        try:
            store.con.execute(
                """
                DELETE FROM feature_definitions AS dst
                USING feature_definitions_load AS src
                WHERE dst.feature_set = src.feature_set
                  AND dst.feature_name = src.feature_name
                """
            )
            insert_frame(store, frame, "feature_definitions", "feature_definitions_insert")
        finally:
            store.con.unregister("feature_definitions_load")
