"""Cross-domain factor namespace mappers."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import numbers
from dataclasses import dataclass
from typing import Iterable

import pandas as pd

from ..warehouse import json_dumps
from .catalog import FactorDefinition
from .cross_section import rank as cs_rank


SOURCE_NAME = "atx-impl cross-domain factor mapper"
PRICE_LIQUIDITY_DECLARED_IN = "db.factors.cross_domain.price_liquidity_specs"
ESTIMATE_13F_DECLARED_IN = "db.factors.cross_domain.estimate_13f_specs"
SHORT_INSIDER_DECLARED_IN = "db.factors.cross_domain.short_insider_specs"
CROSS_DOMAIN_FACTOR_COLUMNS = [
    "factor_value_id",
    "factor_id",
    "factor_name",
    "domain",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "source_row_id",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
]


@dataclass(frozen=True)
class CrossDomainFactorSpec:
    factor_id: str
    factor_name: str
    domain: str
    family: str
    source_table: str
    source_column: str
    description: str
    direction: int
    unit: str
    sign: str
    native_rank_column: str | None = None
    lineage_columns: tuple[str, ...] = ()
    declared_in: str = PRICE_LIQUIDITY_DECLARED_IN

    @property
    def input_ids_json(self) -> str:
        return json_dumps([f"source:{self.source_table}"])

    def to_factor_definition(self) -> FactorDefinition:
        return FactorDefinition(
            factor_id=self.factor_id,
            factor_name=self.factor_name,
            family=self.family,
            description=self.description,
            expression=f"source:{self.source_table}|column:{self.source_column}|operator:rank_cs",
            input_ids_json=self.input_ids_json,
            direction=self.direction,
            lookback_days=0,
            neutralization_spec_json=json_dumps({"method": "none", "by": []}),
            unit=self.unit,
            sign=self.sign,
            scale="percent_rank",
            is_point_in_time_safe=True,
            available_at_policy=f"Factor is available when the source {self.source_table} row is visible as-of.",
            declared_in=self.declared_in,
            owner="atx-impl",
            source=SOURCE_NAME,
        )


PRICE_LIQUIDITY_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    CrossDomainFactorSpec(
        "price_momentum_21d",
        "Price momentum 21d",
        "price_liquidity",
        "price_momentum",
        "equity_price_metrics",
        "momentum_21d",
        "Trailing 21-trading-day price momentum ranked cross-sectionally.",
        1,
        "ratio",
        "signed",
        "momentum_21d_cs_pct_rank",
    ),
    CrossDomainFactorSpec(
        "price_momentum_126d",
        "Price momentum 126d",
        "price_liquidity",
        "price_momentum",
        "equity_price_metrics",
        "momentum_126d",
        "Trailing 126-trading-day price momentum ranked cross-sectionally.",
        1,
        "ratio",
        "signed",
    ),
    CrossDomainFactorSpec(
        "price_realized_vol_20d",
        "Realized volatility 20d",
        "price_liquidity",
        "price_risk",
        "equity_price_metrics",
        "realized_vol_20d",
        "Trailing 20-trading-day annualized realized volatility ranked cross-sectionally.",
        -1,
        "ratio",
        "nonnegative",
        "realized_vol_20d_cs_pct_rank",
    ),
    CrossDomainFactorSpec(
        "price_realized_vol_60d",
        "Realized volatility 60d",
        "price_liquidity",
        "price_risk",
        "equity_price_metrics",
        "realized_vol_60d",
        "Trailing 60-trading-day annualized realized volatility ranked cross-sectionally.",
        -1,
        "ratio",
        "nonnegative",
    ),
    CrossDomainFactorSpec(
        "price_pct_from_high_252d",
        "Percent from high 252d",
        "price_liquidity",
        "price_drawdown",
        "equity_price_metrics",
        "pct_from_high_252d",
        "Distance from the trailing 252-day adjusted high ranked cross-sectionally.",
        1,
        "ratio",
        "signed",
    ),
    CrossDomainFactorSpec(
        "price_avg_dollar_volume_21d",
        "Average dollar volume 21d",
        "price_liquidity",
        "price_liquidity",
        "equity_price_metrics",
        "avg_dollar_volume_21d",
        "Trailing 21-day average dollar volume ranked cross-sectionally.",
        1,
        "currency",
        "nonnegative",
    ),
    CrossDomainFactorSpec(
        "price_amihud_illiquidity_21d",
        "Amihud illiquidity 21d",
        "price_liquidity",
        "price_liquidity",
        "equity_price_metrics",
        "amihud_illiquidity_21d",
        "Trailing 21-day Amihud illiquidity ranked cross-sectionally.",
        -1,
        "score",
        "nonnegative",
        "amihud_illiquidity_21d_cs_pct_rank",
    ),
    CrossDomainFactorSpec(
        "price_beta_60d",
        "Market beta 60d",
        "price_liquidity",
        "price_risk",
        "equity_price_metrics",
        "beta_60d",
        "Trailing 60-day market beta ranked cross-sectionally.",
        -1,
        "ratio",
        "signed",
    ),
    CrossDomainFactorSpec(
        "price_idiosyncratic_vol_60d",
        "Idiosyncratic volatility 60d",
        "price_liquidity",
        "price_risk",
        "equity_price_metrics",
        "idiosyncratic_vol_60d",
        "Trailing 60-day idiosyncratic volatility ranked cross-sectionally.",
        -1,
        "ratio",
        "nonnegative",
    ),
    CrossDomainFactorSpec(
        "price_max_drawdown_126d",
        "Maximum drawdown 126d",
        "price_liquidity",
        "price_drawdown",
        "equity_price_metrics",
        "max_drawdown_126d",
        "Trailing 126-day maximum drawdown ranked cross-sectionally.",
        1,
        "ratio",
        "signed",
    ),
)


ESTIMATE_SURPRISE_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    CrossDomainFactorSpec(
        "estimate_sue",
        "Standardized earnings surprise",
        "estimate_revision",
        "estimate_surprise",
        "est_surprise",
        "sue",
        "Standardized unexpected earnings from est_surprise ranked cross-sectionally.",
        1,
        "score",
        "signed",
        lineage_columns=("measure_code", "fiscal_year", "fiscal_period", "period_end", "actual", "expected", "surprise"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "estimate_surprise_pct",
        "Earnings surprise percent",
        "estimate_revision",
        "estimate_surprise",
        "est_surprise",
        "surprise_pct",
        "Actual minus visible consensus mean divided by absolute visible consensus mean.",
        1,
        "ratio",
        "signed",
        lineage_columns=("measure_code", "fiscal_year", "fiscal_period", "period_end", "actual", "consensus_mean"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
)

ESTIMATE_CONSENSUS_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    CrossDomainFactorSpec(
        "estimate_consensus_revision_mean_pct",
        "Consensus mean revision percent",
        "estimate_revision",
        "estimate_revision",
        "est_consensus",
        "revision_mean_pct",
        "Latest visible consensus mean change versus the prior visible snapshot for the same fiscal period.",
        1,
        "ratio",
        "signed",
        lineage_columns=("measure_code", "fiscal_year", "fiscal_period", "period_end", "consensus_date", "mean", "prior_mean"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "estimate_consensus_revision_breadth",
        "Consensus revision breadth",
        "estimate_revision",
        "estimate_revision",
        "est_consensus",
        "revision_breadth",
        "Up-minus-down estimate revision breadth divided by visible estimate count.",
        1,
        "ratio",
        "signed",
        lineage_columns=("measure_code", "fiscal_year", "fiscal_period", "period_end", "consensus_date", "num_up", "num_down", "num_estimates"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
)

THIRTEENF_FLOW_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    CrossDomainFactorSpec(
        "thirteenf_value_hhi",
        "13F value HHI",
        "13f_flow",
        "thirteenf_crowding",
        "thirteenf_concentration_metrics",
        "value_hhi",
        "Value-weighted 13F holder Herfindahl-Hirschman concentration index.",
        -1,
        "ratio",
        "nonnegative",
        lineage_columns=("cusip", "report_period", "filing_date", "holder_count", "filing_count"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "thirteenf_top10_holder_value_pct",
        "13F top-ten holder value share",
        "13f_flow",
        "thirteenf_crowding",
        "thirteenf_concentration_metrics",
        "top_10_holder_value_pct",
        "Top-ten visible 13F common-share holder value divided by aggregate visible common value.",
        -1,
        "ratio",
        "nonnegative",
        lineage_columns=("cusip", "report_period", "filing_date", "holder_count", "filing_count"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "thirteenf_effective_holder_count_value",
        "13F effective value holder count",
        "13f_flow",
        "thirteenf_conviction",
        "thirteenf_concentration_metrics",
        "effective_holder_count_value",
        "Inverse value HHI, the equally weighted holder count implied by visible 13F concentration.",
        1,
        "count",
        "nonnegative",
        lineage_columns=("cusip", "report_period", "filing_date", "value_hhi", "holder_count"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "thirteenf_value_hhi_change",
        "13F value HHI change",
        "13f_flow",
        "thirteenf_flow",
        "thirteenf_concentration_metrics",
        "value_hhi_change",
        "Quarter-over-quarter change in visible 13F value HHI.",
        -1,
        "ratio",
        "signed",
        lineage_columns=("cusip", "report_period", "prior_report_period", "prior_value_hhi", "value_hhi"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "thirteenf_holder_count_change",
        "13F holder count change",
        "13f_flow",
        "thirteenf_flow",
        "thirteenf_concentration_metrics",
        "holder_count_change",
        "Quarter-over-quarter change in visible common-share 13F holder count.",
        1,
        "count",
        "signed",
        lineage_columns=("cusip", "report_period", "prior_report_period", "prior_holder_count", "holder_count"),
        declared_in=ESTIMATE_13F_DECLARED_IN,
    ),
)


ESTIMATE_13F_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    *ESTIMATE_SURPRISE_SPECS,
    *ESTIMATE_CONSENSUS_SPECS,
    *THIRTEENF_FLOW_SPECS,
)


SHORT_INTEREST_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    CrossDomainFactorSpec(
        "short_interest_days_to_cover",
        "Short interest days to cover",
        "short_interest",
        "short_interest_crowding",
        "short_interest_metrics",
        "days_to_cover",
        "Current short position divided by average daily volume.",
        -1,
        "days",
        "nonnegative",
        lineage_columns=("settlement_date", "current_short_position", "average_daily_volume", "days_to_cover_percentile"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "short_interest_pct_shares_outstanding",
        "Short interest percent shares outstanding",
        "short_interest",
        "short_interest_crowding",
        "short_interest_metrics",
        "short_pct_shares_outstanding",
        "Current short position divided by point-in-time shares outstanding.",
        -1,
        "ratio",
        "nonnegative",
        lineage_columns=("settlement_date", "current_short_position", "short_interest_change_pct"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "short_interest_change_pct",
        "Short interest change percent",
        "short_interest",
        "short_interest_flow",
        "short_interest_metrics",
        "short_interest_change_pct",
        "Current short position change divided by previous short position.",
        -1,
        "ratio",
        "signed",
        lineage_columns=("settlement_date", "current_short_position", "previous_short_position"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "short_interest_momentum_3",
        "Short interest momentum 3 settlements",
        "short_interest",
        "short_interest_flow",
        "short_interest_metrics",
        "short_interest_momentum_3",
        "Short-position growth over the trailing three visible settlements.",
        -1,
        "ratio",
        "signed",
        lineage_columns=("settlement_date", "current_short_position", "days_to_cover_change_3"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "short_interest_short_pressure_score",
        "Short pressure score",
        "short_interest",
        "short_interest_pressure",
        "short_interest_metrics",
        "short_pressure_score",
        "Composite short-pressure score from crowding percentile and short-interest momentum diagnostics.",
        -1,
        "score",
        "bounded",
        lineage_columns=("settlement_date", "days_to_cover_percentile", "short_interest_change_pct_percentile", "is_squeeze_candidate"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
)

INSIDER_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    CrossDomainFactorSpec(
        "insider_net_purchase_value",
        "Insider net purchase value",
        "insider",
        "insider_net_buy",
        "insider_transaction_metrics",
        "net_purchase_value",
        "Open-market purchase value minus sale value for the issuer signal date.",
        1,
        "currency",
        "signed",
        lineage_columns=("signal_date", "window_days", "gross_purchase_value", "gross_sale_value", "transaction_count"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "insider_net_purchase_shares",
        "Insider net purchase shares",
        "insider",
        "insider_net_buy",
        "insider_transaction_metrics",
        "net_purchase_shares",
        "Open-market purchase shares minus sale shares for the issuer signal date.",
        1,
        "shares",
        "signed",
        lineage_columns=("signal_date", "window_days", "gross_purchase_shares", "gross_sale_shares"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "insider_cluster_purchase_value",
        "Insider cluster purchase value",
        "insider",
        "insider_cluster_buy",
        "insider_transaction_metrics",
        "cluster_purchase_value",
        "Trailing-window insider purchase value used by the cluster-buy diagnostic.",
        1,
        "currency",
        "nonnegative",
        lineage_columns=("signal_date", "window_days", "cluster_buyer_count", "cluster_purchase_count", "is_cluster_buy"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "insider_cluster_buy_flag",
        "Insider cluster-buy flag",
        "insider",
        "insider_cluster_buy",
        "insider_transaction_metrics",
        "is_cluster_buy",
        "Boolean indicator that the issuer meets the configured cluster-buy threshold.",
        1,
        "boolean",
        "bounded",
        lineage_columns=("signal_date", "window_days", "cluster_buyer_count", "cluster_min_buyers", "cluster_purchase_value"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
    CrossDomainFactorSpec(
        "insider_plan_sale_value_ratio",
        "Insider plan-sale value ratio",
        "insider",
        "insider_sell_pressure",
        "insider_transaction_metrics",
        "plan_sale_value_ratio",
        "10b5-1 plan sale value divided by gross sale value for the issuer signal date.",
        -1,
        "ratio",
        "nonnegative",
        lineage_columns=("signal_date", "window_days", "plan_sale_value", "gross_sale_value", "plan_sale_count"),
        declared_in=SHORT_INSIDER_DECLARED_IN,
    ),
)

SHORT_INSIDER_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    *SHORT_INTEREST_SPECS,
    *INSIDER_SPECS,
)

CROSS_DOMAIN_SPECS: tuple[CrossDomainFactorSpec, ...] = (
    *PRICE_LIQUIDITY_SPECS,
    *ESTIMATE_13F_SPECS,
    *SHORT_INSIDER_SPECS,
)


def price_liquidity_factor_definitions() -> tuple[FactorDefinition, ...]:
    return tuple(spec.to_factor_definition() for spec in PRICE_LIQUIDITY_SPECS)


def _definition_frame(specs: Iterable[CrossDomainFactorSpec]) -> pd.DataFrame:
    rows = []
    for spec in specs:
        definition = spec.to_factor_definition()
        rows.append(
            {
                "factor_id": definition.factor_id,
                "factor_name": definition.factor_name,
                "family": definition.family,
                "description": definition.description,
                "expression": definition.expression,
                "input_ids_json": definition.input_ids_json,
                "direction": definition.direction,
                "lookback_days": definition.lookback_days,
                "neutralization_spec_json": definition.neutralization_spec_json,
                "unit": definition.unit,
                "sign": definition.sign,
                "scale": definition.scale,
                "is_point_in_time_safe": definition.is_point_in_time_safe,
                "available_at_policy": definition.available_at_policy,
                "declared_in": definition.declared_in,
                "owner": definition.owner,
                "source": definition.source,
                "standardization_spec_json": json_dumps(
                    {
                        "method": "rank_cs",
                        "source_table": spec.source_table,
                        "source_column": spec.source_column,
                        "native_rank_column": spec.native_rank_column,
                    }
                ),
                "valid_from": dt.date(1900, 1, 1),
                "valid_to": None,
            }
        )
    return pd.DataFrame(rows).sort_values("factor_id").reset_index(drop=True)


def price_liquidity_definition_frame() -> pd.DataFrame:
    return _definition_frame(PRICE_LIQUIDITY_SPECS)


def estimate_13f_factor_definitions() -> tuple[FactorDefinition, ...]:
    return tuple(spec.to_factor_definition() for spec in ESTIMATE_13F_SPECS)


def estimate_13f_definition_frame() -> pd.DataFrame:
    return _definition_frame(ESTIMATE_13F_SPECS)


def _dependency_edges_frame(specs: Iterable[CrossDomainFactorSpec]) -> pd.DataFrame:
    rows = []
    for spec in specs:
        definition = spec.to_factor_definition()
        rows.append(
            {
                "dependency_id": _hash_id("factor_dependency", definition.factor_id, "source", spec.source_table),
                "factor_id": definition.factor_id,
                "dependency_type": "source",
                "dependency_name": spec.source_table,
                "dependency_factor_id": None,
                "dependency_metric_id": None,
                "dependency_source_id": spec.source_table,
                "dependency_depth": 1,
                "expression": definition.expression,
                "lookback_days": definition.lookback_days,
                "is_direct": True,
                "source": SOURCE_NAME,
            }
        )
    return pd.DataFrame(rows).sort_values(["factor_id", "dependency_type", "dependency_name"]).reset_index(drop=True)


def price_liquidity_dependency_edges_frame() -> pd.DataFrame:
    return _dependency_edges_frame(PRICE_LIQUIDITY_SPECS)


def estimate_13f_dependency_edges_frame() -> pd.DataFrame:
    return _dependency_edges_frame(ESTIMATE_13F_SPECS)


def short_insider_factor_definitions() -> tuple[FactorDefinition, ...]:
    return tuple(spec.to_factor_definition() for spec in SHORT_INSIDER_SPECS)


def short_insider_definition_frame() -> pd.DataFrame:
    return _definition_frame(SHORT_INSIDER_SPECS)


def short_insider_dependency_edges_frame() -> pd.DataFrame:
    return _dependency_edges_frame(SHORT_INSIDER_SPECS)


def cross_domain_factor_definitions() -> tuple[FactorDefinition, ...]:
    return tuple(spec.to_factor_definition() for spec in CROSS_DOMAIN_SPECS)


def cross_domain_definition_frame() -> pd.DataFrame:
    return _definition_frame(CROSS_DOMAIN_SPECS)


def cross_domain_dependency_edges_frame() -> pd.DataFrame:
    return _dependency_edges_frame(CROSS_DOMAIN_SPECS)


def _hash_id(prefix: str, *parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(f"{prefix}|{payload}".encode("utf-8")).hexdigest()


def _normalize_price_metrics(frame: pd.DataFrame) -> pd.DataFrame:
    if frame is None or frame.empty:
        return pd.DataFrame()
    out = frame.copy()
    if "symbol" not in out.columns:
        out["symbol"] = pd.NA
    if "metric_id" not in out.columns:
        out["metric_id"] = pd.NA
    out["security_id"] = out["security_id"].astype("string")
    out["as_of_date"] = pd.to_datetime(out.get("as_of_date", out.get("trade_date")), errors="coerce").dt.date
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out = out.dropna(subset=["security_id", "as_of_date", "available_at"])
    available_dates = out["available_at"].dt.date
    out = out[available_dates <= out["as_of_date"]]
    return out.reset_index(drop=True)


def _native_percent_rank(frame: pd.DataFrame, spec: CrossDomainFactorSpec) -> pd.Series:
    if not spec.native_rank_column or spec.native_rank_column not in frame.columns:
        return pd.Series(pd.NA, index=frame.index, dtype="Float64")
    native = pd.to_numeric(frame[spec.native_rank_column], errors="coerce")
    counts = native.groupby(frame["as_of_date"], dropna=False).transform("count")
    converted = ((native * counts) - 1.0) / (counts - 1.0)
    if spec.direction < 0:
        converted = 1.0 - converted
    converted = converted.where(counts > 1, 0.0)
    return converted.astype("Float64")


def _json_scalar(value: object) -> object:
    if value is None or pd.isna(value):
        return None
    if isinstance(value, pd.Timestamp):
        return value.isoformat()
    if isinstance(value, dt.datetime):
        return value.isoformat()
    if isinstance(value, dt.date):
        return value.isoformat()
    if isinstance(value, bool):
        return bool(value)
    if isinstance(value, numbers.Integral):
        return int(value)
    if isinstance(value, numbers.Real):
        return float(value)
    return str(value)


def _first_existing(frame: pd.DataFrame, columns: Iterable[str]) -> str | None:
    for column in columns:
        if column in frame.columns:
            return column
    return None


def _normalize_source_metrics(
    frame: pd.DataFrame | None,
    *,
    source_row_id_column: str | None,
    date_columns: tuple[str, ...],
) -> pd.DataFrame:
    if frame is None or frame.empty:
        return pd.DataFrame()
    out = frame.copy()
    if "symbol" not in out.columns:
        out["symbol"] = pd.NA
    if "source_row_id" not in out.columns:
        if source_row_id_column and source_row_id_column in out.columns:
            out["source_row_id"] = out[source_row_id_column]
        else:
            fallback = _first_existing(out, ("metric_id", "est_consensus_id"))
            out["source_row_id"] = out[fallback] if fallback else pd.NA
    date_column = _first_existing(out, date_columns)
    if date_column is None:
        return pd.DataFrame()
    out["security_id"] = out["security_id"].astype("string")
    out["as_of_date"] = pd.to_datetime(out[date_column], errors="coerce").dt.date
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    return out.dropna(subset=["security_id", "as_of_date", "available_at"]).reset_index(drop=True)


def _filter_decision_time(
    frame: pd.DataFrame,
    *,
    as_of_date: dt.date | None,
    as_of_ts: dt.datetime | pd.Timestamp | None,
) -> pd.DataFrame:
    if frame.empty:
        return frame
    out = frame
    if as_of_date is not None:
        decision_date = pd.Timestamp(as_of_date).date()
        out = out[out["as_of_date"] <= decision_date]
        if as_of_ts is None:
            as_of_ts = dt.datetime.combine(decision_date, dt.time(23, 59, 59, 999999))
    if as_of_ts is not None:
        out = out[out["available_at"] <= pd.Timestamp(as_of_ts)]
    return out.reset_index(drop=True)


def _compute_source_factor_rows(
    source_frame: pd.DataFrame,
    *,
    specs: Iterable[CrossDomainFactorSpec],
    run_id: str | None,
    source: str,
    hash_prefix: str,
) -> pd.DataFrame:
    if source_frame.empty:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    rows: list[dict[str, object]] = []
    for spec in specs:
        if spec.source_column not in source_frame.columns:
            continue
        subset = source_frame.dropna(subset=[spec.source_column]).copy()
        if subset.empty:
            continue
        subset["native_percent_rank_value"] = _native_percent_rank(subset, spec)
        line_columns = list(dict.fromkeys(column for column in spec.lineage_columns if column in subset.columns))
        native_columns = [spec.native_rank_column] if spec.native_rank_column and spec.native_rank_column in subset.columns else []
        temp = subset[
            [
                "security_id",
                "symbol",
                "as_of_date",
                "available_at",
                "source_row_id",
                "native_percent_rank_value",
                spec.source_column,
            ]
            + native_columns
            + [column for column in line_columns if column not in {spec.source_column, *native_columns}]
        ].rename(columns={spec.source_column: "raw_value"})
        ranked = cs_rank(
            temp.assign(factor_id=spec.factor_id, value=temp["raw_value"]),
            value_column="value",
            output_column="value",
            partition_columns=("factor_id", "as_of_date"),
            ascending=spec.direction >= 0,
        )
        for row in ranked.itertuples(index=False):
            lineage_record = {
                "source_table": spec.source_table,
                "source_column": spec.source_column,
                "source_row_id": _json_scalar(row.source_row_id),
                "as_of_date": _json_scalar(row.as_of_date),
                "available_at": _json_scalar(pd.Timestamp(row.available_at)),
                "raw_value": float(row.raw_value),
                "native_rank_column": spec.native_rank_column,
                "native_rank_value": None
                if not spec.native_rank_column or pd.isna(getattr(row, spec.native_rank_column, pd.NA))
                else float(getattr(row, spec.native_rank_column)),
                "native_percent_rank_value": None
                if pd.isna(row.native_percent_rank_value)
                else float(row.native_percent_rank_value),
            }
            for column in line_columns:
                if column == spec.source_column:
                    continue
                lineage_record[column] = _json_scalar(getattr(row, column))
            rows.append(
                {
                    "factor_value_id": _hash_id(hash_prefix, spec.factor_id, row.security_id, row.as_of_date, row.available_at, run_id),
                    "factor_id": spec.factor_id,
                    "factor_name": spec.factor_name,
                    "domain": spec.domain,
                    "family": spec.family,
                    "security_id": str(row.security_id),
                    "symbol": row.symbol,
                    "as_of_date": row.as_of_date,
                    "raw_value": float(row.raw_value),
                    "value": float(row.value) if not pd.isna(row.value) else pd.NA,
                    "available_at": pd.Timestamp(row.available_at),
                    "source_row_id": _json_scalar(row.source_row_id),
                    "input_ids_json": spec.input_ids_json,
                    "input_lineage_json": json_dumps([lineage_record]),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )
    if not rows:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    return pd.DataFrame(rows)[CROSS_DOMAIN_FACTOR_COLUMNS].sort_values(
        ["domain", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def compute_price_liquidity_factor_rows(
    price_metrics: pd.DataFrame,
    *,
    specs: Iterable[CrossDomainFactorSpec] = PRICE_LIQUIDITY_SPECS,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Map equity_price_metrics rows into S7-style price/liquidity factor rows."""

    source_frame = _normalize_price_metrics(price_metrics)
    if source_frame.empty:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    rows: list[dict[str, object]] = []
    for spec in specs:
        if spec.source_column not in source_frame.columns:
            continue
        subset = source_frame.dropna(subset=[spec.source_column]).copy()
        if subset.empty:
            continue
        subset["native_percent_rank_value"] = _native_percent_rank(subset, spec)
        temp = subset[
            [
                "security_id",
                "symbol",
                "as_of_date",
                "available_at",
                "metric_id",
                "native_percent_rank_value",
                spec.source_column,
            ]
            + ([spec.native_rank_column] if spec.native_rank_column and spec.native_rank_column in subset.columns else [])
        ].rename(columns={spec.source_column: "raw_value"})
        ranked = cs_rank(
            temp.assign(factor_id=spec.factor_id, value=temp["raw_value"]),
            value_column="value",
            output_column="value",
            partition_columns=("factor_id", "as_of_date"),
            ascending=spec.direction >= 0,
        )
        for row in ranked.itertuples(index=False):
            lineage = [
                {
                    "source_table": "equity_price_metrics",
                    "source_column": spec.source_column,
                    "source_row_id": None if pd.isna(row.metric_id) else str(row.metric_id),
                    "as_of_date": row.as_of_date.isoformat() if isinstance(row.as_of_date, dt.date) else str(row.as_of_date),
                    "available_at": pd.Timestamp(row.available_at).isoformat(),
                    "raw_value": float(row.raw_value),
                    "native_rank_column": spec.native_rank_column,
                    "native_rank_value": None
                    if not spec.native_rank_column or pd.isna(getattr(row, spec.native_rank_column, pd.NA))
                    else float(getattr(row, spec.native_rank_column)),
                    "native_percent_rank_value": None
                    if pd.isna(row.native_percent_rank_value)
                    else float(row.native_percent_rank_value),
                }
            ]
            rows.append(
                {
                    "factor_value_id": _hash_id("cross_domain_factor", spec.factor_id, row.security_id, row.as_of_date, row.available_at, run_id),
                    "factor_id": spec.factor_id,
                    "factor_name": spec.factor_name,
                    "domain": spec.domain,
                    "family": spec.family,
                    "security_id": str(row.security_id),
                    "symbol": row.symbol,
                    "as_of_date": row.as_of_date,
                    "raw_value": float(row.raw_value),
                    "value": float(row.value) if not pd.isna(row.value) else pd.NA,
                    "available_at": pd.Timestamp(row.available_at),
                    "source_row_id": None if pd.isna(row.metric_id) else str(row.metric_id),
                    "input_ids_json": spec.input_ids_json,
                    "input_lineage_json": json_dumps(lineage),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )
    if not rows:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    return pd.DataFrame(rows)[CROSS_DOMAIN_FACTOR_COLUMNS].sort_values(
        ["domain", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def price_liquidity_rank_crosscheck(frame: pd.DataFrame, *, tolerance: float = 1e-12) -> dict[str, object]:
    """Compare S9 canonical ranks to native equity_price_metrics rank diagnostics."""

    if frame is None or frame.empty:
        return {"status": "warning", "checked_count": 0, "mismatch_count": 0, "max_abs_delta": None}
    checked = []
    for row in frame.itertuples(index=False):
        payload = row.input_lineage_json
        try:
            lineage = json.loads(payload)
        except (TypeError, json.JSONDecodeError):
            continue
        if not lineage:
            continue
        native_value = lineage[0].get("native_percent_rank_value")
        if native_value is None or pd.isna(row.value):
            continue
        checked.append(abs(float(row.value) - float(native_value)))
    if not checked:
        return {"status": "warning", "checked_count": 0, "mismatch_count": 0, "max_abs_delta": None}
    mismatch_count = sum(1 for delta in checked if delta > tolerance)
    return {
        "status": "passed" if mismatch_count == 0 else "failed",
        "checked_count": len(checked),
        "mismatch_count": int(mismatch_count),
        "max_abs_delta": float(max(checked)),
    }


def _prepare_consensus_revision_frame(consensus: pd.DataFrame | None) -> pd.DataFrame:
    frame = _normalize_source_metrics(
        consensus,
        source_row_id_column="est_consensus_id",
        date_columns=("as_of_date", "consensus_date", "period_end"),
    )
    if frame.empty:
        return frame
    out = frame.copy()
    for column in ("mean", "num_up", "num_down", "num_estimates"):
        if column not in out.columns:
            out[column] = pd.NA
        out[column] = pd.to_numeric(out[column], errors="coerce")
    for column in ("consensus_date", "period_end"):
        if column in out.columns:
            out[column] = pd.to_datetime(out[column], errors="coerce").dt.date
    group_columns = [
        column
        for column in ("security_id", "measure_code", "fiscal_year", "fiscal_period", "period_end")
        if column in out.columns
    ]
    out = out.sort_values([*group_columns, "available_at", "as_of_date"], kind="mergesort").reset_index(drop=True)
    out["prior_mean"] = out.groupby(group_columns, dropna=False)["mean"].shift(1) if group_columns else pd.NA
    prior = pd.to_numeric(out["prior_mean"], errors="coerce")
    out["revision_mean_pct"] = (out["mean"] - prior) / prior.abs()
    out.loc[prior == 0, "revision_mean_pct"] = pd.NA
    out["revision_breadth"] = (out["num_up"] - out["num_down"]) / out["num_estimates"]
    out.loc[out["num_estimates"] <= 0, "revision_breadth"] = pd.NA
    return out[out["prior_mean"].notna()].reset_index(drop=True)


def compute_estimate_revision_factor_rows(
    *,
    surprises: pd.DataFrame | None = None,
    consensus: pd.DataFrame | None = None,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Map estimate surprise and consensus snapshots into cross-domain factor rows."""

    surprise_frame = _normalize_source_metrics(
        surprises,
        source_row_id_column=None,
        date_columns=("as_of_date", "period_end"),
    )
    surprise_frame = _filter_decision_time(surprise_frame, as_of_date=as_of_date, as_of_ts=as_of_ts)
    consensus_frame = _prepare_consensus_revision_frame(consensus)
    consensus_frame = _filter_decision_time(consensus_frame, as_of_date=as_of_date, as_of_ts=as_of_ts)
    pieces = [
        _compute_source_factor_rows(
            surprise_frame,
            specs=ESTIMATE_SURPRISE_SPECS,
            run_id=run_id,
            source=source,
            hash_prefix="estimate_revision_factor",
        ),
        _compute_source_factor_rows(
            consensus_frame,
            specs=ESTIMATE_CONSENSUS_SPECS,
            run_id=run_id,
            source=source,
            hash_prefix="estimate_revision_factor",
        ),
    ]
    materialized = [piece for piece in pieces if not piece.empty]
    if not materialized:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    return pd.concat(materialized, ignore_index=True).sort_values(
        ["domain", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def compute_thirteenf_flow_factor_rows(
    concentration_metrics: pd.DataFrame,
    *,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Map 13F concentration metrics into PIT-filterable cross-domain factor rows."""

    frame = _normalize_source_metrics(
        concentration_metrics,
        source_row_id_column="metric_id",
        date_columns=("as_of_date", "report_period"),
    )
    frame = _filter_decision_time(frame, as_of_date=as_of_date, as_of_ts=as_of_ts)
    return _compute_source_factor_rows(
        frame,
        specs=THIRTEENF_FLOW_SPECS,
        run_id=run_id,
        source=source,
        hash_prefix="thirteenf_flow_factor",
    )


def compute_estimate_13f_factor_rows(
    *,
    surprises: pd.DataFrame | None = None,
    consensus: pd.DataFrame | None = None,
    concentration_metrics: pd.DataFrame | None = None,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Return the S9-1 estimate-revision plus 13F-flow factor rows."""

    pieces = [
        compute_estimate_revision_factor_rows(
            surprises=surprises,
            consensus=consensus,
            as_of_date=as_of_date,
            as_of_ts=as_of_ts,
            run_id=run_id,
            source=source,
        ),
        compute_thirteenf_flow_factor_rows(
            concentration_metrics if concentration_metrics is not None else pd.DataFrame(),
            as_of_date=as_of_date,
            as_of_ts=as_of_ts,
            run_id=run_id,
            source=source,
        ),
    ]
    materialized = [piece for piece in pieces if not piece.empty]
    if not materialized:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    return pd.concat(materialized, ignore_index=True).sort_values(
        ["domain", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def compute_short_interest_factor_rows(
    short_interest_metrics: pd.DataFrame,
    *,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Map FINRA short-interest metrics into PIT-filterable factor rows."""

    frame = _normalize_source_metrics(
        short_interest_metrics,
        source_row_id_column="metric_id",
        date_columns=("as_of_date", "settlement_date"),
    )
    frame = _filter_decision_time(frame, as_of_date=as_of_date, as_of_ts=as_of_ts)
    return _compute_source_factor_rows(
        frame,
        specs=SHORT_INTEREST_SPECS,
        run_id=run_id,
        source=source,
        hash_prefix="short_interest_factor",
    )


def compute_insider_factor_rows(
    insider_metrics: pd.DataFrame,
    *,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Map insider transaction metrics into PIT-filterable factor rows."""

    frame = insider_metrics.copy() if insider_metrics is not None else pd.DataFrame()
    if "symbol" not in frame.columns and "issuer_trading_symbol" in frame.columns:
        frame["symbol"] = frame["issuer_trading_symbol"]
    frame = _normalize_source_metrics(
        frame,
        source_row_id_column="metric_id",
        date_columns=("as_of_date", "signal_date"),
    )
    frame = _filter_decision_time(frame, as_of_date=as_of_date, as_of_ts=as_of_ts)
    return _compute_source_factor_rows(
        frame,
        specs=INSIDER_SPECS,
        run_id=run_id,
        source=source,
        hash_prefix="insider_factor",
    )


def compute_short_insider_factor_rows(
    *,
    short_interest_metrics: pd.DataFrame | None = None,
    insider_metrics: pd.DataFrame | None = None,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Return the S9-2 short-interest plus insider factor rows."""

    pieces = [
        compute_short_interest_factor_rows(
            short_interest_metrics if short_interest_metrics is not None else pd.DataFrame(),
            as_of_date=as_of_date,
            as_of_ts=as_of_ts,
            run_id=run_id,
            source=source,
        ),
        compute_insider_factor_rows(
            insider_metrics if insider_metrics is not None else pd.DataFrame(),
            as_of_date=as_of_date,
            as_of_ts=as_of_ts,
            run_id=run_id,
            source=source,
        ),
    ]
    materialized = [piece for piece in pieces if not piece.empty]
    if not materialized:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    return pd.concat(materialized, ignore_index=True).sort_values(
        ["domain", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def compute_cross_domain_factor_rows(
    *,
    price_metrics: pd.DataFrame | None = None,
    surprises: pd.DataFrame | None = None,
    consensus: pd.DataFrame | None = None,
    concentration_metrics: pd.DataFrame | None = None,
    short_interest_metrics: pd.DataFrame | None = None,
    insider_metrics: pd.DataFrame | None = None,
    as_of_date: dt.date | None = None,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Assemble all S9 cross-domain factor families into one canonical shape."""

    pieces = [
        compute_price_liquidity_factor_rows(
            price_metrics if price_metrics is not None else pd.DataFrame(),
            run_id=run_id,
            source=source,
        ),
        compute_estimate_13f_factor_rows(
            surprises=surprises,
            consensus=consensus,
            concentration_metrics=concentration_metrics,
            as_of_date=as_of_date,
            as_of_ts=as_of_ts,
            run_id=run_id,
            source=source,
        ),
        compute_short_insider_factor_rows(
            short_interest_metrics=short_interest_metrics,
            insider_metrics=insider_metrics,
            as_of_date=as_of_date,
            as_of_ts=as_of_ts,
            run_id=run_id,
            source=source,
        ),
    ]
    materialized = [piece for piece in pieces if not piece.empty]
    if not materialized:
        return pd.DataFrame(columns=CROSS_DOMAIN_FACTOR_COLUMNS)
    return pd.concat(materialized, ignore_index=True).sort_values(
        ["domain", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def cross_domain_namespace_consistency(
    frame: pd.DataFrame,
    *,
    definitions: Iterable[FactorDefinition] | None = None,
) -> dict[str, object]:
    """Return a gate-ready consistency report for the S9 factor namespace."""

    definition_rows = tuple(cross_domain_factor_definitions() if definitions is None else definitions)
    factor_ids = [definition.factor_id for definition in definition_rows]
    duplicate_factor_ids = sorted({factor_id for factor_id in factor_ids if factor_ids.count(factor_id) > 1})
    definition_by_id = {definition.factor_id: definition for definition in definition_rows}
    metadata_missing = sorted(
        definition.factor_id
        for definition in definition_rows
        if not definition.unit or not definition.sign or not definition.scale
    )
    emitted_factor_ids = set() if frame is None or frame.empty else {str(value) for value in frame["factor_id"].dropna().unique()}
    missing_catalog = sorted(emitted_factor_ids - set(definition_by_id))
    domain_collisions: list[str] = []
    if frame is not None and not frame.empty and {"factor_id", "domain"} <= set(frame.columns):
        domains_by_factor = frame.groupby("factor_id")["domain"].nunique(dropna=True)
        domain_collisions = sorted(str(factor_id) for factor_id, count in domains_by_factor.items() if int(count) > 1)
    status = "passed" if not (duplicate_factor_ids or metadata_missing or missing_catalog or domain_collisions) else "failed"
    return {
        "status": status,
        "definition_count": len(definition_rows),
        "emitted_factor_count": len(emitted_factor_ids),
        "duplicate_factor_ids": duplicate_factor_ids,
        "missing_catalog_factor_ids": missing_catalog,
        "metadata_missing_factor_ids": metadata_missing,
        "domain_collision_factor_ids": domain_collisions,
    }
