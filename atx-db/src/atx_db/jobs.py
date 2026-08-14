from __future__ import annotations

import datetime as dt
import json
import subprocess
import time
import traceback
import uuid
from dataclasses import asdict, is_dataclass
from pathlib import Path
from typing import Any, Callable

from .adjustment_factors import AdjustmentFactorHistoryDataset, AdjustmentFactorHistoryOptions
from .alpha_research import AlphaResearchDataset, AlphaResearchOptions
from .calendar import TradingCalendarDataset, TradingCalendarOptions
from .connection import DuckDBStore
from .corporate_actions import CorporateActionsDataset, CorporateActionsOptions
from .daily_adjustments import DailyAdjustmentFactorDataset, DailyAdjustmentFactorOptions
from .delisting import (
    DelistingEventDataset,
    DelistingEventOptions,
    DelistingReturnObservationDataset,
    DelistingReturnObservationOptions,
)
from .dataset import Dataset, DatasetLoadResult
from .features import (
    EquityDailyFeatureDataset,
    FeatureBuildOptions,
    FundamentalFeatureBuildOptions,
    FundamentalFeatureDataset,
)
from .fact_disagreement import FactDisagreementDataset, FactDisagreementOptions
from .filer_alias import FilerAliasDataset, FilerAliasOptions
from .finra import FinraShortInterestDataset, FinraShortInterestOptions, parse_date
from .fundamental_ratios import FundamentalRatiosDataset, FundamentalRatiosOptions
from .fundamental_reconciliation import (
    FundamentalReconciliationDataset,
    FundamentalReconciliationRefreshOptions,
)
from .metric_engine import FundamentalGrowthDataset, FundamentalGrowthOptions
from .provider_coverage import (
    ProviderCoverageDataset,
    ProviderCoverageOptions,
)
from .standardization import FundamentalStandardizationDataset, FundamentalStandardizationOptions
from .fundamental_xbrl_metrics import FundamentalXbrlMetricDataset, FundamentalXbrlMetricOptions
from .segments import SegmentDataset, SegmentOptions
from .footnotes import FootnoteDataset, FootnoteOptions
from .press_release import PressReleaseDataset, PressReleaseOptions
from .short_interest_metrics import ShortInterestMetricsDataset, ShortInterestMetricsOptions
from .short_volume import FinraShortVolumeDataset, FinraShortVolumeOptions, ShortVolumeMetricsDataset
from .macro_metrics import MacroMetricsDataset, MacroMetricsOptions
from .equity_price_metrics import EquityPriceMetricsDataset, EquityPriceMetricsOptions
from .enterprise_value import EnterpriseValueDataset, EnterpriseValueOptions
from .valuation_multiples import (
    MarketCapDataset,
    MarketCapOptions,
    ValuationMultiplesDataset,
    ValuationMultiplesOptions,
)
from .thirteenf_concentration_metrics import (
    ThirteenFConcentrationMetricsDataset,
    ThirteenFConcentrationMetricsOptions,
)
from .thirteenf_position_metrics import ThirteenFPositionMetricsDataset, ThirteenFPositionMetricsOptions
from .thirteenf_option_metrics import ThirteenFOptionMetricsDataset, ThirteenFOptionMetricsOptions
from .corporate_action_metrics import (
    CorporateActionDividendMetricsDataset,
    CorporateActionDividendMetricsOptions,
    CorporateActionFactorReconciliationDataset,
    CorporateActionFactorReconciliationOptions,
    CorporateActionSplitMetricsDataset,
    CorporateActionSplitMetricsOptions,
)
from .fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions
from .identifier_decisions import IdentifierResolutionDecisionDataset, IdentifierResolutionDecisionOptions
from .identifier_resolution import IdentifierResolutionCandidateDataset, IdentifierResolutionOptions
from .estimate_security_links import EstimateSecurityLinkDataset, EstimateSecurityLinkOptions
from .insider_ownership import (
    DEFAULT_BLOCKHOLDER_FORMS,
    DEFAULT_FORMS,
    BlockholderOwnershipDataset,
    BlockholderOwnershipOptions,
    InsiderOwnershipDataset,
    InsiderOwnershipOptions,
)
from .insider_metrics import InsiderTransactionMetricsDataset, InsiderTransactionMetricsOptions
from .listing_metrics import SecurityListingMetricsDataset, SecurityListingMetricsOptions
from .form144 import Form144IntentDataset, Form144Options, Form144ReconciliationDataset
from .listing_status import ListingStatusIntervalDataset, ListingStatusIntervalOptions
from .macro import FredMacroDataset, FredMacroOptions
from .orchestrator import (
    DATASET_PARAMS_KEY,
    DatasetOrchestrator,
    OrchestratorResult,
)
from .offexchange import (
    FinraOffExchangeDataset,
    FinraOffExchangeOptions,
    OffExchangeSecurityPeriodDataset,
)
from .offexchange_quality import OffExchangeQualityReportDataset, OffExchangeQualityReportOptions
from .ownership import OwnershipFeatureDataset, OwnershipFeatureOptions
from .pricing_bulk import (
    BulkBarsBackfillDataset,
    BulkBarsBackfillOptions,
    BulkBarsDataset,
    BulkBarsOptions,
)
from .sec_submissions import SecSubmissionsDataset, SecSubmissionsOptions
from .security_master import SecurityMasterDataset, SecurityMasterOptions
from .shares_outstanding import SharesOutstandingHistoryDataset, SharesOutstandingHistoryOptions
from .short_interest_features import ShortInterestFeatureDataset, ShortInterestFeatureOptions
from .symbol_directory import (
    NasdaqListingEventsDataset,
    NasdaqListingEventsOptions,
    NasdaqSymbolDirectoryDataset,
    NasdaqSymbolDirectoryOptions,
)
from .thirteenf import ThirteenFDataSet, ThirteenFOptions
from .ticker_history import TickerHistoryDataset, TickerHistoryOptions
from .universes import UniverseBuildOptions, UniverseMembershipDataset
from .watermarks import refresh_warehouse_watermarks
from .warehouse import json_dumps, now_utc_naive
from .xbrl_filing_contexts import XbrlFilingContextDataset, XbrlFilingContextOptions
from .xbrl_validation import XbrlValidationDataset, XbrlValidationOptions
from .reference_classifications import (
    EntityClassificationDataset,
    EntityClassificationOptions,
    FamaFrenchTaxonomyDataset,
    FamaFrenchTaxonomyOptions,
    NaicsTaxonomyDataset,
    NaicsTaxonomyOptions,
    SicTaxonomyDataset,
    SicTaxonomyOptions,
)
from .estimates import (
    EstimateMeasureSeedDataset,
    EstimateMeasureSeedOptions,
    EstimateActualsDataset,
    EstimateActualsOptions,
    EstimateSurpriseDataset,
    EstimateSurpriseOptions,
    EstimateDetailDataset,
    EstimateDetailOptions,
    EstimateConsensusDataset,
    EstimateConsensusOptions,
    EstimateGuidanceDataset,
    EstimateGuidanceOptions,
    EstimateRecommendationDataset,
    EstimateRecommendationOptions,
    EstimateRecommendationSummaryDataset,
    EstimateRecommendationSummaryOptions,
)
from .xbrl_taxonomy import XbrlTaxonomyDataset, XbrlTaxonomyOptions


OptionFactory = Callable[[dict[str, Any]], Any]


def _tuple_or_none(value: Any) -> tuple[str, ...] | None:
    if value is None:
        return None
    if isinstance(value, str):
        return tuple(part.strip().upper() for part in value.split(",") if part.strip())
    return tuple(str(part).strip().upper() for part in value)


def _lower_tuple_or_none(value: Any) -> tuple[str, ...] | None:
    if value is None:
        return None
    if isinstance(value, str):
        return tuple(part.strip().lower() for part in value.split(",") if part.strip())
    return tuple(str(part).strip().lower() for part in value)


def _string_tuple_or_none(value: Any) -> tuple[str, ...] | None:
    if value is None:
        return None
    if isinstance(value, str):
        return tuple(part.strip() for part in value.split(",") if part.strip())
    return tuple(str(part).strip() for part in value if str(part).strip())


def _bool_param(value: Any, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"1", "true", "t", "yes", "y", "on"}:
            return True
        if normalized in {"0", "false", "f", "no", "n", "off", ""}:
            return False
    return bool(value)


def _date_or_none(value: Any) -> dt.date | None:
    if isinstance(value, dt.date):
        return value
    return parse_date(value)


def _datetime_or_none(value: Any) -> dt.datetime | None:
    if value in (None, ""):
        return None
    if isinstance(value, dt.datetime):
        return value
    if isinstance(value, dt.date):
        return dt.datetime.combine(value, dt.time())
    return dt.datetime.fromisoformat(str(value))


def _path(value: Any, default: Path) -> Path:
    if value in (None, ""):
        return default
    return Path(value)


def _path_tuple_or_none(value: Any) -> tuple[Path, ...] | None:
    if value is None:
        return None
    if isinstance(value, str):
        return tuple(Path(part.strip()) for part in value.split(",") if part.strip())
    return tuple(Path(str(part)) for part in value if str(part).strip())


def _nonnegative_int(value: Any, name: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise ValueError(f"{name} must be >= 0")
    return parsed


def _nonnegative_float(value: Any, name: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise ValueError(f"{name} must be >= 0")
    return parsed


def _security_master_options(params: dict[str, Any]) -> SecurityMasterOptions:
    return SecurityMasterOptions(
        source_url=params.get("source_url", SecurityMasterOptions.source_url),
        request_timeout=int(params.get("request_timeout", 60)),
        user_agent=params.get("user_agent", SecurityMasterOptions.user_agent),
    )


def _ticker_history_options(params: dict[str, Any]) -> TickerHistoryOptions:
    default = TickerHistoryOptions()
    return TickerHistoryOptions(
        zip_path=_path(params.get("zip_path"), default.zip_path),
        symbols=_tuple_or_none(params.get("symbols", default.symbols)),
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        chunk_size=int(params.get("chunk_size", default.chunk_size)),
        max_chunks=None if params.get("max_chunks") in (None, "") else int(params["max_chunks"]),
        source=params.get("source", default.source),
        compute_source_hash=_bool_param(params.get("compute_source_hash"), default.compute_source_hash),
    )


def _bulk_bars_options(params: dict[str, Any]) -> BulkBarsOptions:
    default = BulkBarsOptions()
    return BulkBarsOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source_zip=None if params.get("source_zip") in (None, "") else Path(params["source_zip"]),
        symbols=_tuple_or_none(params.get("symbols")),
        start_date=_date_or_none(params.get("start_date", default.start_date)),
        end_date=_date_or_none(params.get("end_date")),
        chunk_size=int(params.get("chunk_size", default.chunk_size)),
        max_chunks=None if params.get("max_chunks") in (None, "") else int(params["max_chunks"]),
        source=params.get("source", default.source),
        compute_source_hash=_bool_param(params.get("compute_source_hash"), default.compute_source_hash),
        run_id=params.get("run_id") or default.run_id,
    )


def _bulk_bars_backfill_options(params: dict[str, Any]) -> BulkBarsBackfillOptions:
    default = BulkBarsBackfillOptions()
    return BulkBarsBackfillOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source_zip=None if params.get("source_zip") in (None, "") else Path(params["source_zip"]),
        symbols=_tuple_or_none(params.get("symbols")),
        start_date=_date_or_none(params.get("start_date", default.start_date)),
        end_date=_date_or_none(params.get("end_date")),
        chunk_size=int(params.get("chunk_size", default.chunk_size)),
        max_chunks=None if params.get("max_chunks") in (None, "") else int(params["max_chunks"]),
        source=params.get("source", default.source),
        compute_source_hash=_bool_param(params.get("compute_source_hash"), default.compute_source_hash),
        partition_key=params.get("partition_key") or params.get("backfill_partition_key"),
        window_lo=_date_or_none(params.get("window_lo")),
        window_hi=_date_or_none(params.get("window_hi") or params.get("window_hi_exclusive")),
        backfill_run_id=params.get("backfill_run_id"),
        run_id=params.get("run_id") or default.run_id,
    )


def _corporate_actions_options(params: dict[str, Any]) -> CorporateActionsOptions:
    default = CorporateActionsOptions()
    return CorporateActionsOptions(
        source=params.get("source", default.source),
        min_cash_amount=float(params.get("min_cash_amount", default.min_cash_amount)),
        max_dividend_factor=float(params.get("max_dividend_factor", default.max_dividend_factor)),
    )


def _adjustment_factor_options(params: dict[str, Any]) -> AdjustmentFactorHistoryOptions:
    default = AdjustmentFactorHistoryOptions()
    return AdjustmentFactorHistoryOptions(
        source=params.get("source") or default.source,
        run_id=params.get("run_id") or default.run_id,
    )


def _daily_adjustment_factor_options(params: dict[str, Any]) -> DailyAdjustmentFactorOptions:
    default = DailyAdjustmentFactorOptions()
    return DailyAdjustmentFactorOptions(
        source=params.get("source") or default.source,
        factor_source=params.get("factor_source") or default.factor_source,
        bar_source=params.get("bar_source") or default.bar_source,
        as_of_date=_date_or_none(params.get("as_of_date")),
        as_of_ts=_datetime_or_none(params.get("as_of_ts")),
        run_id=params.get("run_id") or default.run_id,
    )


def _delisting_event_options(params: dict[str, Any]) -> DelistingEventOptions:
    default = DelistingEventOptions()
    return DelistingEventOptions(
        source=params.get("source") or default.source,
        listing_status_source=params.get("listing_status_source") or default.listing_status_source,
        include_snapshot_absence=_bool_param(
            params.get("include_snapshot_absence"),
            default.include_snapshot_absence,
        ),
        apply_shumway_warther_imputation=_bool_param(
            params.get("apply_shumway_warther_imputation"),
            default.apply_shumway_warther_imputation,
        ),
        run_id=params.get("run_id") or default.run_id,
    )


def _delisting_return_observation_options(params: dict[str, Any]) -> DelistingReturnObservationOptions:
    default = DelistingReturnObservationOptions()
    return DelistingReturnObservationOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        provider=params.get("provider") or default.provider,
        vendor_security_id_type=params.get("vendor_security_id_type") or default.vendor_security_id_type,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        run_id=params.get("run_id") or default.run_id,
    )


def _estimate_detail_options(params: dict[str, Any]) -> EstimateDetailOptions:
    default = EstimateDetailOptions()
    return EstimateDetailOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        provider=params.get("provider") or default.provider,
        vendor_security_id_type=params.get("vendor_security_id_type") or default.vendor_security_id_type,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        run_id=params.get("run_id") or default.run_id,
    )


def _estimate_consensus_options(params: dict[str, Any]) -> EstimateConsensusOptions:
    default = EstimateConsensusOptions()
    return EstimateConsensusOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        provider_name=params.get("provider_name") or default.provider_name,
        vendor_security_id_type=params.get("vendor_security_id_type") or default.vendor_security_id_type,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        stale_after_days=int(params.get("stale_after_days", default.stale_after_days)),
        run_id=params.get("run_id") or default.run_id,
    )


def _estimate_guidance_options(params: dict[str, Any]) -> EstimateGuidanceOptions:
    default = EstimateGuidanceOptions()
    return EstimateGuidanceOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        min_confidence=float(params.get("min_confidence", default.min_confidence)),
        run_id=params.get("run_id") or default.run_id,
    )


def _press_release_options(params: dict[str, Any]) -> PressReleaseOptions:
    default = PressReleaseOptions()
    return PressReleaseOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        min_confidence=float(params.get("min_confidence", default.min_confidence)),
        reconciliation_tolerance=float(params.get("reconciliation_tolerance", default.reconciliation_tolerance)),
        run_id=params.get("run_id") or default.run_id,
    )


def _estimate_recommendation_options(params: dict[str, Any]) -> EstimateRecommendationOptions:
    default = EstimateRecommendationOptions()
    return EstimateRecommendationOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        provider_name=params.get("provider_name") or default.provider_name,
        vendor_security_id_type=params.get("vendor_security_id_type") or default.vendor_security_id_type,
        source_vendor_table=params.get("source_vendor_table") or default.source_vendor_table,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        run_id=params.get("run_id") or default.run_id,
    )


def _estimate_recommendation_summary_options(params: dict[str, Any]) -> EstimateRecommendationSummaryOptions:
    default = EstimateRecommendationSummaryOptions()
    return EstimateRecommendationSummaryOptions(
        source_file=None if params.get("source_file") in (None, "") else Path(params["source_file"]),
        source=params.get("source") or default.source,
        provider_name=params.get("provider_name") or default.provider_name,
        vendor_security_id_type=params.get("vendor_security_id_type") or default.vendor_security_id_type,
        source_vendor_table=params.get("source_vendor_table") or default.source_vendor_table,
        rating_scale=params.get("rating_scale") or default.rating_scale,
        scale_direction=params.get("scale_direction") or default.scale_direction,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        run_id=params.get("run_id") or default.run_id,
    )


def _estimate_security_link_options(params: dict[str, Any]) -> EstimateSecurityLinkOptions:
    default = EstimateSecurityLinkOptions()
    return EstimateSecurityLinkOptions(
        provider_names=_tuple_or_none(params.get("provider_names", default.provider_names)),
        vendor_security_id_types=_tuple_or_none(
            params.get("vendor_security_id_types", default.vendor_security_id_types)
        ),
        min_confidence=float(params.get("min_confidence", default.min_confidence)),
        apply_to_security_identifier_history=_bool_param(
            params.get("apply_to_security_identifier_history"),
            default.apply_to_security_identifier_history,
        ),
        source=params.get("source") or default.source,
        run_id=params.get("run_id") or default.run_id,
    )


def _finra_options(params: dict[str, Any]) -> FinraShortInterestOptions:
    default = FinraShortInterestOptions()
    return FinraShortInterestOptions(
        api_url=params.get("api_url", default.api_url),
        symbol=params.get("symbol"),
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        limit=int(params.get("limit", default.limit)),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        max_retries=int(params.get("max_retries", default.max_retries)),
        retry_sleep=float(params.get("retry_sleep", default.retry_sleep)),
        limit_dates=None if params.get("limit_dates") in (None, "") else int(params["limit_dates"]),
        date_order=params.get("date_order", default.date_order),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _short_interest_feature_options(params: dict[str, Any]) -> ShortInterestFeatureOptions:
    default = ShortInterestFeatureOptions()
    return ShortInterestFeatureOptions(
        feature_set=params.get("feature_set", default.feature_set),
        source=params.get("source", default.source),
        min_cross_section=int(params.get("min_cross_section", default.min_cross_section)),
    )


def _thirteenf_options(params: dict[str, Any]) -> ThirteenFOptions:
    default = ThirteenFOptions()
    full_holdings = _bool_param(params.get("full_holdings"), False)
    return ThirteenFOptions(
        dataset_url=params.get("dataset_url"),
        cache_dir=_path(params.get("cache_dir"), default.cache_dir),
        cusips=None if full_holdings else _tuple_or_none(params.get("cusips", default.cusips)),
        chunk_size=int(params.get("chunk_size", default.chunk_size)),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
        compute_source_hash=_bool_param(params.get("compute_source_hash"), default.compute_source_hash),
    )


def _identifier_resolution_options(params: dict[str, Any]) -> IdentifierResolutionOptions:
    default = IdentifierResolutionOptions()
    return IdentifierResolutionOptions(
        source_dataset_id=params.get("source_dataset_id") or default.source_dataset_id,
        source_period=params.get("source_period") or default.source_period,
        min_confidence=float(params.get("min_confidence", default.min_confidence)),
        include_already_mapped=_bool_param(
            params.get("include_already_mapped"),
            default.include_already_mapped,
        ),
        source=params.get("source", default.source),
    )


def _identifier_decision_options(params: dict[str, Any]) -> IdentifierResolutionDecisionOptions:
    default = IdentifierResolutionDecisionOptions()
    return IdentifierResolutionDecisionOptions(
        source_dataset_id=params.get("source_dataset_id") or default.source_dataset_id,
        source_period=params.get("source_period") or default.source_period,
        min_accept_confidence=float(params.get("min_accept_confidence", default.min_accept_confidence)),
        min_review_confidence=float(params.get("min_review_confidence", default.min_review_confidence)),
        accept_candidate_statuses=(
            _lower_tuple_or_none(params.get("accept_candidate_statuses")) or default.accept_candidate_statuses
        ),
        review_candidate_statuses=(
            _lower_tuple_or_none(params.get("review_candidate_statuses")) or default.review_candidate_statuses
        ),
        apply_accepted=_bool_param(params.get("apply_accepted"), default.apply_accepted),
        decision_method=params.get("decision_method") or default.decision_method,
        decided_by=params.get("decided_by") or default.decided_by,
        source=params.get("source") or default.source,
    )


def _company_facts_options(params: dict[str, Any]) -> SecCompanyFactsOptions:
    default = SecCompanyFactsOptions()
    return SecCompanyFactsOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        concepts=_string_tuple_or_none(params.get("concepts", default.concepts)) or default.concepts,
        symbol_source=(params.get("symbol_source") or default.symbol_source),
        symbol_limit=(None if params.get("symbol_limit") in (None, "") else int(params["symbol_limit"])),
        universe_id=params.get("universe_id") or default.universe_id,
        as_of_date=_date_or_none(params.get("as_of_date")),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _shares_outstanding_options(params: dict[str, Any]) -> SharesOutstandingHistoryOptions:
    default = SharesOutstandingHistoryOptions()
    return SharesOutstandingHistoryOptions(
        source=params.get("source") or default.source,
        run_id=params.get("run_id") or default.run_id,
    )


def _xbrl_taxonomy_options(params: dict[str, Any]) -> XbrlTaxonomyOptions:
    default = XbrlTaxonomyOptions()
    return XbrlTaxonomyOptions(
        package_urls=_string_tuple_or_none(params.get("package_urls", default.package_urls)) or default.package_urls,
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _features_options(params: dict[str, Any]) -> FeatureBuildOptions:
    default = FeatureBuildOptions()
    return FeatureBuildOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        feature_set=params.get("feature_set", default.feature_set),
        min_rows=int(params.get("min_rows", default.min_rows)),
    )


def _fundamental_features_options(params: dict[str, Any]) -> FundamentalFeatureBuildOptions:
    default = FundamentalFeatureBuildOptions()
    return FundamentalFeatureBuildOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        feature_set=params.get("feature_set", default.feature_set),
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
    )


def _alpha_research_options(params: dict[str, Any]) -> AlphaResearchOptions:
    default = AlphaResearchOptions()
    return AlphaResearchOptions(
        symbols=_tuple_or_none(params.get("symbols")),
        feature_set=params.get("feature_set", default.feature_set),
        universe_id=params.get("universe_id", default.universe_id),
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        horizon_days=int(params.get("horizon_days", default.horizon_days)),
        top_quantile=float(params.get("top_quantile", default.top_quantile)),
        bottom_quantile=float(params.get("bottom_quantile", default.bottom_quantile)),
        min_cross_section=int(params.get("min_cross_section", default.min_cross_section)),
    )


def _calendar_options(params: dict[str, Any]) -> TradingCalendarOptions:
    default = TradingCalendarOptions()
    return TradingCalendarOptions(
        calendar_id=params.get("calendar_id", default.calendar_id),
        source=params.get("source", default.source),
    )


def _universe_options(params: dict[str, Any]) -> UniverseBuildOptions:
    default = UniverseBuildOptions()
    symbols = params.get("symbols", default.symbols)
    return UniverseBuildOptions(
        universe_id=params.get("universe_id", default.universe_id),
        name=params.get("name", default.name),
        description=params.get("description", default.description),
        symbols=None if symbols is None else _tuple_or_none(symbols),
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        lookback_days=int(params.get("lookback_days", default.lookback_days)),
        min_history_days=int(params.get("min_history_days", default.min_history_days)),
        min_price=float(params.get("min_price", default.min_price)),
        min_dollar_volume=float(params.get("min_dollar_volume", default.min_dollar_volume)),
        source=params.get("source", default.source),
    )


def _submissions_options(params: dict[str, Any]) -> SecSubmissionsOptions:
    default = SecSubmissionsOptions()
    forms = params.get("forms", default.forms)
    return SecSubmissionsOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        forms=None if forms is None else _tuple_or_none(forms),
        include_history_files=_bool_param(params.get("include_history_files"), default.include_history_files),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _xbrl_filing_context_options(params: dict[str, Any]) -> XbrlFilingContextOptions:
    default = XbrlFilingContextOptions()
    return XbrlFilingContextOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        forms=_tuple_or_none(params.get("forms", default.forms)) or default.forms,
        accession_numbers=_string_tuple_or_none(params.get("accession_numbers")),
        max_filings=int(params.get("max_filings", default.max_filings)),
        max_filings_per_symbol=(
            None if params.get("max_filings_per_symbol") in (None, "") else int(params["max_filings_per_symbol"])
        ),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _xbrl_validation_options(params: dict[str, Any]) -> XbrlValidationOptions:
    default = XbrlValidationOptions()
    return XbrlValidationOptions(
        absolute_tolerance=float(params.get("absolute_tolerance", default.absolute_tolerance)),
        run_id=params.get("run_id") or default.run_id,
    )


def _symbol_directory_options(params: dict[str, Any]) -> NasdaqSymbolDirectoryOptions:
    default = NasdaqSymbolDirectoryOptions()
    return NasdaqSymbolDirectoryOptions(
        nasdaq_url=params.get("nasdaq_url", default.nasdaq_url),
        other_url=params.get("other_url", default.other_url),
        as_of_date=_date_or_none(params.get("as_of_date")),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _listing_events_options(params: dict[str, Any]) -> NasdaqListingEventsOptions:
    default = NasdaqListingEventsOptions()
    return NasdaqListingEventsOptions(
        source_url=params.get("source_url", default.source_url),
        as_of_date=_date_or_none(params.get("as_of_date")),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


def _listing_status_options(params: dict[str, Any]) -> ListingStatusIntervalOptions:
    default = ListingStatusIntervalOptions()
    return ListingStatusIntervalOptions(source=params.get("source", default.source))


def _ownership_feature_options(params: dict[str, Any]) -> OwnershipFeatureOptions:
    default = OwnershipFeatureOptions()
    return OwnershipFeatureOptions(
        feature_set=params.get("feature_set", default.feature_set),
        source=params.get("source", default.source),
    )


def _insider_ownership_options(params: dict[str, Any]) -> InsiderOwnershipOptions:
    default = InsiderOwnershipOptions()
    return InsiderOwnershipOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        forms=_tuple_or_none(params.get("forms", default.forms)) or default.forms,
        accession_numbers=_string_tuple_or_none(params.get("accession_numbers")),
        source_urls=_string_tuple_or_none(params.get("source_urls")),
        source_files=_path_tuple_or_none(params.get("source_files")),
        max_filings=int(params.get("max_filings", default.max_filings)),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
        source=params.get("source", default.source),
    )


def _blockholder_ownership_options(params: dict[str, Any]) -> BlockholderOwnershipOptions:
    default = BlockholderOwnershipOptions()
    return BlockholderOwnershipOptions(
        symbols=_tuple_or_none(params.get("symbols", default.symbols)) or default.symbols,
        forms=_tuple_or_none(params.get("forms", default.forms)) or default.forms,
        accession_numbers=_string_tuple_or_none(params.get("accession_numbers")),
        source_urls=_string_tuple_or_none(params.get("source_urls")),
        source_files=_path_tuple_or_none(params.get("source_files")),
        max_filings=int(params.get("max_filings", default.max_filings)),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
        source=params.get("source", default.source),
    )


def _insider_transaction_metrics_options(params: dict[str, Any]) -> InsiderTransactionMetricsOptions:
    default = InsiderTransactionMetricsOptions()
    return InsiderTransactionMetricsOptions(
        source=params.get("source", default.source),
        input_source=params.get("input_source", default.input_source),
        window_days=int(params.get("window_days", default.window_days)),
        cluster_min_buyers=int(params.get("cluster_min_buyers", default.cluster_min_buyers)),
        cluster_min_purchase_value=float(params.get("cluster_min_purchase_value", default.cluster_min_purchase_value)),
        security_ids=_string_tuple_or_none(params.get("security_ids")),
        symbols=_tuple_or_none(params.get("symbols")),
        run_id=params.get("run_id", default.run_id),
    )


def _security_listing_metrics_options(params: dict[str, Any]) -> SecurityListingMetricsOptions:
    default = SecurityListingMetricsOptions()
    return SecurityListingMetricsOptions(
        source=params.get("source", default.source),
        input_listing_source=params.get("input_listing_source", default.input_listing_source),
        security_ids=_string_tuple_or_none(params.get("security_ids")),
        symbols=_tuple_or_none(params.get("symbols")),
        run_id=params.get("run_id", default.run_id),
    )


def _form144_options(params: dict[str, Any]) -> Form144Options:
    default = Form144Options()
    source_files = _path_tuple_or_none(params.get("source_files"))
    if not source_files and params.get("source_file") not in (None, ""):
        source_files = (Path(params["source_file"]),)
    metadata = params.get("metadata_by_source") or {}
    return Form144Options(
        source_files=source_files,
        source=params.get("source", default.source),
        replace_source_files=_bool_param(params.get("replace_source_files"), default.replace_source_files),
        reconcile=_bool_param(params.get("reconcile"), default.reconcile),
        match_window_days=int(params.get("match_window_days", default.match_window_days)),
        run_id=params.get("run_id", default.run_id),
        metadata_by_source=dict(metadata),
    )


def _entity_classification_options(params: dict[str, Any]) -> EntityClassificationOptions:
    _default = EntityClassificationOptions()
    sic_file = params.get("sic_file")
    submissions_zip = params.get("submissions_zip")
    return EntityClassificationOptions(
        symbols=_tuple_or_none(params.get("symbols")),
        sic_file=Path(sic_file) if sic_file else _default.sic_file,
        submissions_zip=Path(submissions_zip) if submissions_zip else _default.submissions_zip,
        user_agent=params.get("user_agent", _default.user_agent),
        request_timeout=int(params.get("request_timeout", _default.request_timeout)),
    )


def _offexchange_volume_options(params: dict[str, Any]) -> FinraOffExchangeOptions:
    default = FinraOffExchangeOptions()
    source_file = params.get("source_file")
    return FinraOffExchangeOptions(
        source_file=Path(source_file) if source_file else default.source_file,
        source=params.get("source") or default.source,
        period_type=params.get("period_type") or default.period_type,
        tier=params.get("tier") or default.tier,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        run_id=params.get("run_id") or default.run_id,
    )


def _finra_short_volume_options(params: dict[str, Any]) -> FinraShortVolumeOptions:
    default = FinraShortVolumeOptions()
    source_file = params.get("source_file")
    return FinraShortVolumeOptions(
        source_file=Path(source_file) if source_file else default.source_file,
        source=params.get("source") or default.source,
        replace_source_file=_bool_param(params.get("replace_source_file"), default.replace_source_file),
        run_id=params.get("run_id") or default.run_id,
    )


def _offexchange_quality_report_options(params: dict[str, Any]) -> OffExchangeQualityReportOptions:
    default = OffExchangeQualityReportOptions()
    return OffExchangeQualityReportOptions(
        source=params.get("source") or default.source,
        offexchange_source=params.get("offexchange_source") or default.offexchange_source,
        short_volume_source=params.get("short_volume_source") or default.short_volume_source,
        include_offexchange=_bool_param(params.get("include_offexchange"), default.include_offexchange),
        include_short_volume=_bool_param(params.get("include_short_volume"), default.include_short_volume),
        run_id=params.get("run_id") or default.run_id,
    )


def _filer_alias_options(params: dict[str, Any]) -> FilerAliasOptions:
    default = FilerAliasOptions()
    seed_file = params.get("seed_file")
    return FilerAliasOptions(
        source=params.get("source") or default.source,
        seed_file=Path(seed_file) if seed_file else default.seed_file,
        seed_source=params.get("seed_source") or default.seed_source,
        replace=_bool_param(params.get("replace"), default.replace),
        run_id=params.get("run_id") or default.run_id,
    )


def _fundamental_xbrl_metric_options(params: dict[str, Any]) -> FundamentalXbrlMetricOptions:
    default = FundamentalXbrlMetricOptions()
    return FundamentalXbrlMetricOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _segment_options(params: dict[str, Any]) -> SegmentOptions:
    default = SegmentOptions()
    return SegmentOptions(
        source=params.get("source") or default.source,
        coverage_source=params.get("coverage_source") or default.coverage_source,
        reconciliation_tolerance=float(params.get("reconciliation_tolerance", default.reconciliation_tolerance)),
        run_id=params.get("run_id") or default.run_id,
    )


def _footnote_options(params: dict[str, Any]) -> FootnoteOptions:
    default = FootnoteOptions()
    return FootnoteOptions(
        source=params.get("source") or default.source,
        run_id=params.get("run_id") or default.run_id,
    )


def _fundamental_ratios_options(params: dict[str, Any]) -> FundamentalRatiosOptions:
    default = FundamentalRatiosOptions()
    return FundamentalRatiosOptions(
        source=params.get("source") or default.source,
        basis=params.get("basis") or default.basis,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _provider_coverage_options(params: dict[str, Any]) -> ProviderCoverageOptions:
    observed_at = params.get("observed_at")
    if isinstance(observed_at, str):
        observed_at = dt.datetime.fromisoformat(observed_at)
    return ProviderCoverageOptions(
        dataset_ids=_string_tuple_or_none(params.get("dataset_ids") or params.get("datasets")),
        observed_at=observed_at,
        run_id=params.get("run_id"),
    )


def _fundamental_growth_options(params: dict[str, Any]) -> FundamentalGrowthOptions:
    default = FundamentalGrowthOptions()
    return FundamentalGrowthOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        metrics=_string_tuple_or_none(params.get("metrics")) or default.metrics,
        run_id=params.get("run_id") or default.run_id,
    )


def _short_interest_metrics_options(params: dict[str, Any]) -> ShortInterestMetricsOptions:
    default = ShortInterestMetricsOptions()
    return ShortInterestMetricsOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _macro_metrics_options(params: dict[str, Any]) -> MacroMetricsOptions:
    default = MacroMetricsOptions()
    return MacroMetricsOptions(
        source=params.get("source") or default.source,
        series_ids=_tuple_or_none(params.get("series_ids")) or default.series_ids,
        run_id=params.get("run_id") or default.run_id,
    )


def _equity_price_metrics_options(params: dict[str, Any]) -> EquityPriceMetricsOptions:
    default = EquityPriceMetricsOptions()
    return EquityPriceMetricsOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _market_cap_options(params: dict[str, Any]) -> MarketCapOptions:
    default = MarketCapOptions()
    return MarketCapOptions(
        source=params.get("source") or default.source,
        price_sources=_string_tuple_or_none(params.get("price_sources")) or default.price_sources,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        run_id=params.get("run_id") or default.run_id,
    )


def _valuation_multiples_options(params: dict[str, Any]) -> ValuationMultiplesOptions:
    default = ValuationMultiplesOptions()
    return ValuationMultiplesOptions(
        source=params.get("source") or default.source,
        market_cap_sources=_string_tuple_or_none(params.get("market_cap_sources")) or default.market_cap_sources,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        run_id=params.get("run_id") or default.run_id,
    )


def _enterprise_value_options(params: dict[str, Any]) -> EnterpriseValueOptions:
    default = EnterpriseValueOptions()
    return EnterpriseValueOptions(
        source=params.get("source") or default.source,
        market_cap_sources=_string_tuple_or_none(params.get("market_cap_sources")) or default.market_cap_sources,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        run_id=params.get("run_id") or default.run_id,
    )


def _fundamental_standardization_options(params: dict[str, Any]) -> FundamentalStandardizationOptions:
    default = FundamentalStandardizationOptions()
    return FundamentalStandardizationOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _fundamental_reconciliation_options(
    params: dict[str, Any],
) -> FundamentalReconciliationRefreshOptions:
    default = FundamentalReconciliationRefreshOptions()
    return FundamentalReconciliationRefreshOptions(
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _fact_disagreement_options(params: dict[str, Any]) -> FactDisagreementOptions:
    default = FactDisagreementOptions()
    return FactDisagreementOptions(
        source=params.get("source") or default.source,
        baseline_source=params.get("baseline_source") or default.baseline_source,
        vendor=params.get("vendor") or default.vendor,
        baseline_path=params.get("baseline_path") or default.baseline_path,
        as_of_ts=_datetime_or_none(params.get("as_of_ts")),
        tolerance_abs=float(params.get("tolerance_abs", default.tolerance_abs)),
        tolerance_rel=float(params.get("tolerance_rel", default.tolerance_rel)),
        run_id=params.get("run_id") or default.run_id,
    )


def _thirteenf_position_metrics_options(params: dict[str, Any]) -> ThirteenFPositionMetricsOptions:
    default = ThirteenFPositionMetricsOptions()
    return ThirteenFPositionMetricsOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _thirteenf_option_metrics_options(params: dict[str, Any]) -> ThirteenFOptionMetricsOptions:
    default = ThirteenFOptionMetricsOptions()
    return ThirteenFOptionMetricsOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _thirteenf_concentration_metrics_options(params: dict[str, Any]) -> ThirteenFConcentrationMetricsOptions:
    default = ThirteenFConcentrationMetricsOptions()
    return ThirteenFConcentrationMetricsOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _corporate_action_dividend_metrics_options(params: dict[str, Any]) -> CorporateActionDividendMetricsOptions:
    default = CorporateActionDividendMetricsOptions()
    return CorporateActionDividendMetricsOptions(
        source=params.get("source") or default.source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _corporate_action_split_metrics_options(params: dict[str, Any]) -> CorporateActionSplitMetricsOptions:
    default = CorporateActionSplitMetricsOptions()
    return CorporateActionSplitMetricsOptions(
        source=params.get("source") or default.source,
        factor_source=params.get("factor_source") or default.factor_source,
        daily_adjustment_source=params.get("daily_adjustment_source") or default.daily_adjustment_source,
        bar_source=params.get("bar_source") or default.bar_source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _corporate_action_factor_reconciliation_options(
    params: dict[str, Any],
) -> CorporateActionFactorReconciliationOptions:
    default = CorporateActionFactorReconciliationOptions()
    return CorporateActionFactorReconciliationOptions(
        source=params.get("source") or default.source,
        factor_source=params.get("factor_source") or default.factor_source,
        daily_adjustment_source=params.get("daily_adjustment_source") or default.daily_adjustment_source,
        bar_source=params.get("bar_source") or default.bar_source,
        symbols=_tuple_or_none(params.get("symbols")) or default.symbols,
        run_id=params.get("run_id") or default.run_id,
    )


def _fred_macro_options(params: dict[str, Any]) -> FredMacroOptions:
    default = FredMacroOptions()
    return FredMacroOptions(
        series_ids=_tuple_or_none(params.get("series_ids", default.series_ids)) or default.series_ids,
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        request_timeout=int(params.get("request_timeout", default.request_timeout)),
        user_agent=params.get("user_agent", default.user_agent),
    )


DATASET_REGISTRY: dict[str, tuple[type[Dataset], OptionFactory]] = {
    SecurityMasterDataset.dataset_id: (SecurityMasterDataset, _security_master_options),
    TickerHistoryDataset.dataset_id: (TickerHistoryDataset, _ticker_history_options),
    BulkBarsDataset.dataset_id: (BulkBarsDataset, _bulk_bars_options),
    BulkBarsBackfillDataset.dataset_id: (
        BulkBarsBackfillDataset,
        _bulk_bars_backfill_options,
    ),
    CorporateActionsDataset.dataset_id: (CorporateActionsDataset, _corporate_actions_options),
    AdjustmentFactorHistoryDataset.dataset_id: (
        AdjustmentFactorHistoryDataset,
        _adjustment_factor_options,
    ),
    DailyAdjustmentFactorDataset.dataset_id: (
        DailyAdjustmentFactorDataset,
        _daily_adjustment_factor_options,
    ),
    DelistingEventDataset.dataset_id: (DelistingEventDataset, _delisting_event_options),
    DelistingReturnObservationDataset.dataset_id: (
        DelistingReturnObservationDataset,
        _delisting_return_observation_options,
    ),
    FinraShortInterestDataset.dataset_id: (FinraShortInterestDataset, _finra_options),
    ShortInterestFeatureDataset.dataset_id: (ShortInterestFeatureDataset, _short_interest_feature_options),
    ThirteenFDataSet.dataset_id: (ThirteenFDataSet, _thirteenf_options),
    FilerAliasDataset.dataset_id: (FilerAliasDataset, _filer_alias_options),
    FinraOffExchangeDataset.dataset_id: (FinraOffExchangeDataset, _offexchange_volume_options),
    OffExchangeSecurityPeriodDataset.dataset_id: (
        OffExchangeSecurityPeriodDataset,
        _offexchange_volume_options,
    ),
    FinraShortVolumeDataset.dataset_id: (FinraShortVolumeDataset, _finra_short_volume_options),
    ShortVolumeMetricsDataset.dataset_id: (ShortVolumeMetricsDataset, _finra_short_volume_options),
    OffExchangeQualityReportDataset.dataset_id: (
        OffExchangeQualityReportDataset,
        _offexchange_quality_report_options,
    ),
    OwnershipFeatureDataset.dataset_id: (OwnershipFeatureDataset, _ownership_feature_options),
    InsiderOwnershipDataset.dataset_id: (InsiderOwnershipDataset, _insider_ownership_options),
    InsiderTransactionMetricsDataset.dataset_id: (
        InsiderTransactionMetricsDataset,
        _insider_transaction_metrics_options,
    ),
    SecurityListingMetricsDataset.dataset_id: (
        SecurityListingMetricsDataset,
        _security_listing_metrics_options,
    ),
    Form144IntentDataset.dataset_id: (Form144IntentDataset, _form144_options),
    Form144ReconciliationDataset.dataset_id: (Form144ReconciliationDataset, _form144_options),
    BlockholderOwnershipDataset.dataset_id: (BlockholderOwnershipDataset, _blockholder_ownership_options),
    IdentifierResolutionCandidateDataset.dataset_id: (
        IdentifierResolutionCandidateDataset,
        _identifier_resolution_options,
    ),
    IdentifierResolutionDecisionDataset.dataset_id: (
        IdentifierResolutionDecisionDataset,
        _identifier_decision_options,
    ),
    SecCompanyFactsDataset.dataset_id: (SecCompanyFactsDataset, _company_facts_options),
    SharesOutstandingHistoryDataset.dataset_id: (
        SharesOutstandingHistoryDataset,
        _shares_outstanding_options,
    ),
    XbrlTaxonomyDataset.dataset_id: (XbrlTaxonomyDataset, _xbrl_taxonomy_options),
    XbrlFilingContextDataset.dataset_id: (XbrlFilingContextDataset, _xbrl_filing_context_options),
    XbrlValidationDataset.dataset_id: (XbrlValidationDataset, _xbrl_validation_options),
    SecSubmissionsDataset.dataset_id: (SecSubmissionsDataset, _submissions_options),
    NasdaqSymbolDirectoryDataset.dataset_id: (NasdaqSymbolDirectoryDataset, _symbol_directory_options),
    NasdaqListingEventsDataset.dataset_id: (NasdaqListingEventsDataset, _listing_events_options),
    ListingStatusIntervalDataset.dataset_id: (ListingStatusIntervalDataset, _listing_status_options),
    FredMacroDataset.dataset_id: (FredMacroDataset, _fred_macro_options),
    EquityDailyFeatureDataset.dataset_id: (EquityDailyFeatureDataset, _features_options),
    FundamentalFeatureDataset.dataset_id: (FundamentalFeatureDataset, _fundamental_features_options),
    FundamentalRatiosDataset.dataset_id: (FundamentalRatiosDataset, _fundamental_ratios_options),
    ProviderCoverageDataset.dataset_id: (ProviderCoverageDataset, _provider_coverage_options),
    FundamentalGrowthDataset.dataset_id: (FundamentalGrowthDataset, _fundamental_growth_options),
    FundamentalStandardizationDataset.dataset_id: (
        FundamentalStandardizationDataset,
        _fundamental_standardization_options,
    ),
    FundamentalReconciliationDataset.dataset_id: (
        FundamentalReconciliationDataset,
        _fundamental_reconciliation_options,
    ),
    FundamentalXbrlMetricDataset.dataset_id: (FundamentalXbrlMetricDataset, _fundamental_xbrl_metric_options),
    SegmentDataset.dataset_id: (SegmentDataset, _segment_options),
    FootnoteDataset.dataset_id: (FootnoteDataset, _footnote_options),
    ShortInterestMetricsDataset.dataset_id: (ShortInterestMetricsDataset, _short_interest_metrics_options),
    MacroMetricsDataset.dataset_id: (MacroMetricsDataset, _macro_metrics_options),
    EquityPriceMetricsDataset.dataset_id: (EquityPriceMetricsDataset, _equity_price_metrics_options),
    MarketCapDataset.dataset_id: (MarketCapDataset, _market_cap_options),
    EnterpriseValueDataset.dataset_id: (EnterpriseValueDataset, _enterprise_value_options),
    ValuationMultiplesDataset.dataset_id: (ValuationMultiplesDataset, _valuation_multiples_options),
    FactDisagreementDataset.dataset_id: (FactDisagreementDataset, _fact_disagreement_options),
    ThirteenFPositionMetricsDataset.dataset_id: (ThirteenFPositionMetricsDataset, _thirteenf_position_metrics_options),
    ThirteenFOptionMetricsDataset.dataset_id: (ThirteenFOptionMetricsDataset, _thirteenf_option_metrics_options),
    ThirteenFConcentrationMetricsDataset.dataset_id: (
        ThirteenFConcentrationMetricsDataset,
        _thirteenf_concentration_metrics_options,
    ),
    CorporateActionDividendMetricsDataset.dataset_id: (
        CorporateActionDividendMetricsDataset,
        _corporate_action_dividend_metrics_options,
    ),
    CorporateActionSplitMetricsDataset.dataset_id: (
        CorporateActionSplitMetricsDataset,
        _corporate_action_split_metrics_options,
    ),
    CorporateActionFactorReconciliationDataset.dataset_id: (
        CorporateActionFactorReconciliationDataset,
        _corporate_action_factor_reconciliation_options,
    ),
    AlphaResearchDataset.dataset_id: (AlphaResearchDataset, _alpha_research_options),
    TradingCalendarDataset.dataset_id: (TradingCalendarDataset, _calendar_options),
    UniverseMembershipDataset.dataset_id: (UniverseMembershipDataset, _universe_options),
    SicTaxonomyDataset.dataset_id: (SicTaxonomyDataset, lambda p: SicTaxonomyOptions()),
    FamaFrenchTaxonomyDataset.dataset_id: (FamaFrenchTaxonomyDataset, lambda p: FamaFrenchTaxonomyOptions()),
    NaicsTaxonomyDataset.dataset_id: (NaicsTaxonomyDataset, lambda p: NaicsTaxonomyOptions()),
    EntityClassificationDataset.dataset_id: (
        EntityClassificationDataset,
        _entity_classification_options,
    ),
    EstimateMeasureSeedDataset.dataset_id: (
        EstimateMeasureSeedDataset,
        lambda p: EstimateMeasureSeedOptions(),
    ),
    EstimateActualsDataset.dataset_id: (
        EstimateActualsDataset,
        lambda p: EstimateActualsOptions(
            measure_codes=_tuple_or_none(p.get("measure_codes")),
            security_ids=_tuple_or_none(p.get("security_ids")),
        ),
    ),
    EstimateSurpriseDataset.dataset_id: (
        EstimateSurpriseDataset,
        lambda p: EstimateSurpriseOptions(
            measure_codes=_tuple_or_none(p.get("measure_codes")),
            window=int(p.get("window", 8)),
            min_obs=int(p.get("min_obs", 4)),
            model=p.get("model", "srw_drift"),
        ),
    ),
    EstimateDetailDataset.dataset_id: (
        EstimateDetailDataset,
        _estimate_detail_options,
    ),
    EstimateConsensusDataset.dataset_id: (
        EstimateConsensusDataset,
        _estimate_consensus_options,
    ),
    EstimateGuidanceDataset.dataset_id: (
        EstimateGuidanceDataset,
        _estimate_guidance_options,
    ),
    PressReleaseDataset.dataset_id: (
        PressReleaseDataset,
        _press_release_options,
    ),
    EstimateRecommendationDataset.dataset_id: (
        EstimateRecommendationDataset,
        _estimate_recommendation_options,
    ),
    EstimateRecommendationSummaryDataset.dataset_id: (
        EstimateRecommendationSummaryDataset,
        _estimate_recommendation_summary_options,
    ),
    EstimateSecurityLinkDataset.dataset_id: (
        EstimateSecurityLinkDataset,
        _estimate_security_link_options,
    ),
}

DATASET_DEPENDENCIES: dict[str, tuple[str, ...]] = {
    "adjustment_factor_history": ("corporate_actions",),
    "alpha_research": ("equity_daily_features", "tbltickerhistory_daily"),
    "corporate_action_dividend_metrics": ("corporate_actions", "tbltickerhistory_daily"),
    "corporate_action_factor_reconciliation": (
        "adjustment_factor_history",
        "daily_adjustment_factors",
    ),
    "corporate_action_split_metrics": (
        "adjustment_factor_history",
        "daily_adjustment_factors",
    ),
    "corporate_actions": ("tbltickerhistory_daily",),
    "daily_adjustment_factors": ("adjustment_factor_history",),
    "delisting_events": ("listing_status_intervals",),
    "entity_classification": (
        "fama_french_taxonomy",
        "naics_taxonomy",
        "sec_security_master",
        "sic_taxonomy",
    ),
    "equity_daily_features": ("tbltickerhistory_daily",),
    "equity_price_metrics": ("tbltickerhistory_daily",),
    "est_actual": ("est_measure", "sec_company_facts"),
    "est_security_link": (
        "est_consensus",
        "est_detail",
        "est_recommendation",
        "est_recommendation_summary",
        "identifier_resolution_decisions",
    ),
    "est_surprise": ("est_actual", "est_consensus"),
    "press_release_facts": ("est_actual",),
    "finra_short_interest": ("sec_security_master",),
    "finra_short_interest_features": ("finra_short_interest",),
    "finra_short_volume": ("sec_security_master",),
    "filer_13f_cik_alias": ("sec_13f",),
    "form144_intent": ("sec_insider_ownership",),
    "form144_to_form4_link": ("form144_intent", "sec_insider_ownership"),
    "fundamental_ratios": ("fundamental_standardized",),
    "provider_schema_coverage": (
        "fundamental_ratios",
        "fundamental_reconciliation",
        "fundamental_standardized",
        "tbltickerhistory_daily",
    ),
    "fundamental_growth": ("fundamental_xbrl_metric", "sec_company_facts"),
    "fundamental_standardized": ("fundamental_xbrl_metric", "sec_company_facts"),
    "fundamental_reconciliation": (
        "fundamental_standardized",
        "xbrl_filing_contexts",
    ),
    "fundamental_xbrl_metric": ("xbrl_filing_contexts",),
    "segments": ("fundamental_xbrl_metric", "xbrl_filing_contexts"),
    "footnotes": ("xbrl_filing_contexts",),
    "identifier_resolution_candidates": ("sec_13f",),
    "identifier_resolution_decisions": ("identifier_resolution_candidates",),
    "insider_transaction_metrics": ("sec_insider_ownership",),
    "listing_status_intervals": (
        "nasdaq_listing_events",
        "nasdaq_symbol_directory",
        "sec_security_master",
    ),
    "macro_metrics": ("fred_macro",),
    "market_cap": ("tbltickerhistory_daily", "shares_outstanding_history"),
    "enterprise_value": (
        "market_cap",
        "sec_company_facts",
    ),
    "valuation_multiples": (
        "market_cap",
        "fundamental_xbrl_metric",
        "sec_company_facts",
    ),
    "fact_disagreement": ("fundamental_standardized",),
    "nasdaq_listing_events": ("sec_security_master",),
    "offexchange_quality_report": (
        "offexchange_security_period",
        "short_volume_metrics",
    ),
    "offexchange_security_period": ("offexchange_volume",),
    "offexchange_volume": ("sec_security_master",),
    "sec_13f": ("sec_security_master",),
    "sec_13f_ownership_features": ("sec_13f",),
    "sec_blockholder_ownership": ("sec_submissions",),
    "sec_company_facts": ("sec_security_master",),
    "sec_fundamental_features": ("sec_company_facts", "tbltickerhistory_daily"),
    "sec_insider_ownership": ("sec_submissions",),
    "sec_submissions": ("sec_security_master",),
    "security_listing_metrics": ("listing_status_intervals",),
    "shares_outstanding_history": ("sec_company_facts",),
    "short_interest_metrics": (
        "finra_short_interest",
        "shares_outstanding_history",
    ),
    "short_volume_metrics": ("finra_short_volume",),
    "thirteenf_concentration_metrics": ("sec_13f_ownership_features",),
    "thirteenf_option_metrics": ("sec_13f_ownership_features",),
    "thirteenf_position_metrics": ("sec_13f_ownership_features",),
    "tbltickerhistory_daily": ("sec_security_master",),
    "bulk_daily_bars_backfill": ("sec_security_master",),
    "trading_calendar": ("tbltickerhistory_daily",),
    "universe_memberships": ("tbltickerhistory_daily",),
    "xbrl_filing_contexts": ("sec_submissions",),
    "xbrl_validation": ("xbrl_filing_contexts", "xbrl_taxonomy"),
}


def _apply_dataset_dependencies() -> None:
    unknown = sorted(set(DATASET_DEPENDENCIES) - set(DATASET_REGISTRY))
    if unknown:
        raise RuntimeError(f"Unknown dataset dependency metadata keys: {unknown}")
    for dataset_id, (dataset_cls, _option_factory) in DATASET_REGISTRY.items():
        setattr(dataset_cls, "depends_on", DATASET_DEPENDENCIES.get(dataset_id, ()))


_apply_dataset_dependencies()


def normalize_params(params: Any) -> dict[str, Any]:
    if params is None:
        return {}
    if isinstance(params, str):
        return json.loads(params) if params.strip() else {}
    if is_dataclass(params):
        return asdict(params)
    return dict(params)


def normalize_dependencies(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        if not value.strip():
            return []
        loaded = json.loads(value)
    else:
        loaded = value
    if loaded is None:
        return []
    if not isinstance(loaded, list):
        raise ValueError("dependencies_json must decode to a list")
    return [str(item) for item in loaded if str(item)]


_MERGED_SEQUENCE_PARAM_KEYS = frozenset(
    {
        "accession_numbers",
        "concepts",
        "cusips",
        "forms",
        "measure_codes",
        "package_urls",
        "provider_names",
        "security_ids",
        "series_ids",
        "source_files",
        "source_urls",
        "symbols",
        "vendor_security_id_types",
    }
)


def _sequence_param_values(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, str):
        return [part.strip() for part in value.split(",") if part.strip()]
    if isinstance(value, (list, tuple, set, frozenset)):
        values = sorted(value, key=str) if isinstance(value, (set, frozenset)) else value
        return [item for item in values if str(item).strip()]
    return [value]


def _dedupe_key(value: Any) -> str:
    if isinstance(value, str):
        return value.strip().upper()
    try:
        return json.dumps(value, default=str, sort_keys=True)
    except TypeError:
        return str(value)


def _merge_sequence_params(left: Any, right: Any) -> list[Any]:
    merged: list[Any] = []
    seen: set[str] = set()
    for value in [*_sequence_param_values(left), *_sequence_param_values(right)]:
        key = _dedupe_key(value)
        if key in seen:
            continue
        seen.add(key)
        merged.append(value)
    return merged


def _params_with_implicit_dataset_defaults(
    dataset_id: str,
    params: dict[str, Any],
) -> dict[str, Any]:
    if dataset_id != SecSubmissionsDataset.dataset_id:
        return params
    enriched = dict(params)
    defaults = SecSubmissionsOptions()
    if "symbols" not in enriched:
        enriched["symbols"] = defaults.symbols
    if "forms" not in enriched:
        enriched["forms"] = defaults.forms
    return enriched


def _merge_orchestrator_job_params(
    dataset_id: str,
    existing: dict[str, Any],
    incoming: dict[str, Any],
) -> dict[str, Any]:
    merged = _params_with_implicit_dataset_defaults(dataset_id, existing)
    incoming = _params_with_implicit_dataset_defaults(dataset_id, incoming)
    for key, value in incoming.items():
        if key not in merged:
            merged[key] = value
            continue
        current = merged[key]
        if key in _MERGED_SEQUENCE_PARAM_KEYS:
            merged[key] = None if current is None or value is None else _merge_sequence_params(current, value)
        elif isinstance(current, bool) and isinstance(value, bool):
            merged[key] = current or value
        elif current in (None, "") and value not in (None, ""):
            merged[key] = value
    return merged


def current_git_sha(repo_root: Path | None = None) -> str | None:
    root = repo_root or Path(__file__).resolve().parents[2]
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True,
            check=True,
            text=True,
            timeout=5,
        )
    except Exception:
        return None
    sha = result.stdout.strip()
    return sha or None


def refresh_quant_warehouse(
    store: DuckDBStore,
    *,
    params: dict[str, Any] | None = None,
    registry: dict[str, tuple[type[Dataset], OptionFactory]] | None = None,
    run_id: str | None = None,
    resume: str | None = None,
    full_rebuild: bool = False,
    gate: bool = False,
    git_sha: str | None = None,
    actor: str = "warehouse_jobs",
) -> OrchestratorResult:
    if run_id and resume:
        raise ValueError("Use either run_id or resume, not both")
    selected_registry = DATASET_REGISTRY if registry is None else registry
    orchestrator = DatasetOrchestrator(
        store,
        selected_registry,
        actor=actor,
        watermark_refresher=refresh_warehouse_watermarks,
    )
    run_params = normalize_params(params)
    if resume:
        return orchestrator.resume(
            resume,
            params=run_params,
            full_rebuild=full_rebuild,
            gate=gate,
        )
    return orchestrator.run(
        run_id=run_id,
        params=run_params,
        git_sha=git_sha if git_sha is not None else current_git_sha(),
        full_rebuild=full_rebuild,
        gate=gate,
    )


def orchestrator_step_results(
    store: DuckDBStore,
    run_id: str,
    dataset_order: tuple[str, ...] | None = None,
) -> list[DatasetLoadResult]:
    rows = store.con.execute(
        """
        SELECT dataset_id, status, rows, started_at, finished_at,
               watermark_before, watermark_after, error
        FROM etl_job_steps
        WHERE run_id = ?
        """,
        [run_id],
    ).fetchall()
    by_dataset = {
        str(dataset_id): (
            str(status),
            rows_loaded,
            started_at,
            finished_at,
            watermark_before,
            watermark_after,
            error,
        )
        for (
            dataset_id,
            status,
            rows_loaded,
            started_at,
            finished_at,
            watermark_before,
            watermark_after,
            error,
        ) in rows
    }
    order = list(dataset_order or tuple(sorted(by_dataset)))
    results: list[DatasetLoadResult] = []
    for dataset_id in order:
        if dataset_id not in by_dataset:
            continue
        (
            status,
            rows_loaded,
            started_at,
            finished_at,
            watermark_before,
            watermark_after,
            error,
        ) = by_dataset[dataset_id]
        details = {
            "status": status,
            "started_at": started_at,
            "finished_at": finished_at,
            "watermark_before": watermark_before,
            "watermark_after": watermark_after,
        }
        if error:
            details["error"] = error
        results.append(
            DatasetLoadResult(
                dataset_id=dataset_id,
                rows_loaded=int(rows_loaded or 0),
                source=f"orchestrator:{status}",
                details=details,
                run_id=run_id,
            )
        )
    return results


class JobManager:
    def __init__(self, store: DuckDBStore) -> None:
        self.store = store

    def register_job(
        self,
        *,
        job_name: str,
        dataset_id: str,
        params: dict[str, Any] | None = None,
        enabled: bool = True,
        schedule: str | None = None,
        dependencies: list[str] | None = None,
        max_retries: int = 0,
        retry_delay_seconds: float = 0.0,
    ) -> None:
        if dataset_id not in DATASET_REGISTRY:
            raise KeyError(f"Unknown dataset_id {dataset_id!r}")
        max_retries = _nonnegative_int(max_retries, "max_retries")
        retry_delay_seconds = _nonnegative_float(retry_delay_seconds, "retry_delay_seconds")
        self.store.con.execute("DELETE FROM etl_job_definitions WHERE job_name = ?", [job_name])
        self.store.con.execute(
            """
            INSERT INTO etl_job_definitions (
                job_name,
                dataset_id,
                params_json,
                enabled,
                schedule,
                max_retries,
                retry_delay_seconds,
                dependencies_json,
                created_at,
                updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, now(), now())
            """,
            [
                job_name,
                dataset_id,
                json_dumps(params or {}),
                enabled,
                schedule,
                max_retries,
                retry_delay_seconds,
                json_dumps(dependencies or []),
            ],
        )

    def seed_default_jobs(
        self,
        *,
        symbols: tuple[str, ...] = ("AAPL",),
        ticker_history_zip: Path | None = None,
        max_chunks: int | None = None,
        max_retries: int = 1,
        retry_delay_seconds: float = 10.0,
    ) -> None:
        retry_policy = {
            "max_retries": _nonnegative_int(max_retries, "max_retries"),
            "retry_delay_seconds": _nonnegative_float(retry_delay_seconds, "retry_delay_seconds"),
        }
        self.register_job(job_name="security_master", dataset_id="sec_security_master", **retry_policy)
        self.register_job(job_name="nasdaq_symbol_directory", dataset_id="nasdaq_symbol_directory", **retry_policy)
        self.register_job(
            job_name="nasdaq_listing_events",
            dataset_id="nasdaq_listing_events",
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="listing_status_intervals",
            dataset_id="listing_status_intervals",
            dependencies=["security_master", "nasdaq_symbol_directory", "nasdaq_listing_events"],
            **retry_policy,
        )
        self.register_job(
            job_name="security_listing_metrics",
            dataset_id="security_listing_metrics",
            dependencies=["listing_status_intervals"],
            **retry_policy,
        )
        self.register_job(
            job_name="delisting_events",
            dataset_id="delisting_events",
            dependencies=["listing_status_intervals"],
            **retry_policy,
        )
        self.register_job(
            job_name="daily_bars",
            dataset_id="tbltickerhistory_daily",
            params={
                "zip_path": str(ticker_history_zip) if ticker_history_zip else None,
                "symbols": symbols,
                "max_chunks": max_chunks,
            },
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="corporate_actions",
            dataset_id="corporate_actions",
            dependencies=["daily_bars"],
            **retry_policy,
        )
        self.register_job(
            job_name="adjustment_factor_history",
            dataset_id="adjustment_factor_history",
            dependencies=["corporate_actions"],
            **retry_policy,
        )
        self.register_job(
            job_name="daily_adjustment_factors",
            dataset_id="daily_adjustment_factors",
            dependencies=["adjustment_factor_history"],
            **retry_policy,
        )
        self.register_job(
            job_name="finra_short_interest",
            dataset_id="finra_short_interest",
            params={"symbol": symbols[0] if symbols else None},
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="finra_short_interest_features",
            dataset_id="finra_short_interest_features",
            dependencies=["finra_short_interest"],
            **retry_policy,
        )
        self.register_job(job_name="sec_13f", dataset_id="sec_13f", dependencies=["security_master"], **retry_policy)
        self.register_job(
            job_name="sec_13f_ownership_features",
            dataset_id="sec_13f_ownership_features",
            dependencies=["sec_13f"],
            **retry_policy,
        )
        self.register_job(
            job_name="filer_13f_cik_alias",
            dataset_id="filer_13f_cik_alias",
            dependencies=["sec_13f"],
            **retry_policy,
        )
        self.register_job(
            job_name="offexchange_volume",
            dataset_id="offexchange_volume",
            params={"period_type": "weekly"},
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="offexchange_security_period",
            dataset_id="offexchange_security_period",
            params={"period_type": "weekly"},
            dependencies=["offexchange_volume"],
            **retry_policy,
        )
        self.register_job(
            job_name="finra_short_volume",
            dataset_id="finra_short_volume",
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="short_volume_metrics",
            dataset_id="short_volume_metrics",
            dependencies=["finra_short_volume"],
            **retry_policy,
        )
        self.register_job(
            job_name="offexchange_quality_report",
            dataset_id="offexchange_quality_report",
            dependencies=["offexchange_security_period", "short_volume_metrics"],
            **retry_policy,
        )
        self.register_job(
            job_name="sec_ownership_submissions",
            dataset_id="sec_submissions",
            params={"symbols": symbols, "forms": DEFAULT_FORMS},
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="sec_insider_ownership",
            dataset_id="sec_insider_ownership",
            params={"symbols": symbols, "forms": DEFAULT_FORMS, "max_filings": 25},
            dependencies=["sec_ownership_submissions"],
            **retry_policy,
        )
        self.register_job(
            job_name="insider_transaction_metrics",
            dataset_id="insider_transaction_metrics",
            params={"symbols": symbols},
            dependencies=["sec_insider_ownership"],
            **retry_policy,
        )
        self.register_job(
            job_name="form144_intent",
            dataset_id="form144_intent",
            params={"source_files": None, "reconcile": True},
            dependencies=["sec_insider_ownership"],
            **retry_policy,
        )
        self.register_job(
            job_name="form144_to_form4_link",
            dataset_id="form144_to_form4_link",
            dependencies=["form144_intent", "sec_insider_ownership"],
            **retry_policy,
        )
        self.register_job(
            job_name="sec_blockholder_submissions",
            dataset_id="sec_submissions",
            params={"symbols": symbols, "forms": DEFAULT_BLOCKHOLDER_FORMS},
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="sec_blockholder_ownership",
            dataset_id="sec_blockholder_ownership",
            params={"symbols": symbols, "forms": DEFAULT_BLOCKHOLDER_FORMS, "max_filings": 25},
            dependencies=["sec_blockholder_submissions"],
            **retry_policy,
        )
        self.register_job(
            job_name="identifier_resolution_candidates",
            dataset_id="identifier_resolution_candidates",
            dependencies=["sec_13f"],
            **retry_policy,
        )
        self.register_job(
            job_name="identifier_resolution_decisions",
            dataset_id="identifier_resolution_decisions",
            dependencies=["identifier_resolution_candidates"],
            **retry_policy,
        )
        self.register_job(
            job_name="sec_company_facts",
            dataset_id="sec_company_facts",
            params={"symbols": symbols},
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="shares_outstanding_history",
            dataset_id="shares_outstanding_history",
            dependencies=["sec_company_facts"],
            **retry_policy,
        )
        self.register_job(
            job_name="market_cap",
            dataset_id="market_cap",
            params={"symbols": symbols},
            dependencies=["daily_bars", "shares_outstanding_history"],
            **retry_policy,
        )
        self.register_job(
            job_name="enterprise_value",
            dataset_id="enterprise_value",
            params={"symbols": symbols},
            dependencies=["market_cap", "sec_company_facts"],
            **retry_policy,
        )
        self.register_job(
            job_name="valuation_multiples",
            dataset_id="valuation_multiples",
            params={"symbols": symbols},
            dependencies=["market_cap", "fundamental_xbrl_metric", "sec_company_facts"],
            **retry_policy,
        )
        self.register_job(job_name="xbrl_taxonomy", dataset_id="xbrl_taxonomy", **retry_policy)
        self.register_job(
            job_name="sec_fundamental_features",
            dataset_id="sec_fundamental_features",
            params={"symbols": symbols},
            dependencies=["sec_company_facts", "daily_bars"],
            **retry_policy,
        )
        self.register_job(
            job_name="fundamental_xbrl_metric",
            dataset_id="fundamental_xbrl_metric",
            dependencies=["xbrl_filing_contexts"],
            **retry_policy,
        )
        self.register_job(
            job_name="fundamental_standardized",
            dataset_id="fundamental_standardized",
            params={"symbols": symbols},
            dependencies=["sec_company_facts", "fundamental_xbrl_metric"],
            **retry_policy,
        )
        self.register_job(
            job_name="fact_disagreement",
            dataset_id="fact_disagreement",
            dependencies=["fundamental_standardized"],
            **retry_policy,
        )
        self.register_job(
            job_name="segments",
            dataset_id="segments",
            dependencies=["xbrl_filing_contexts", "fundamental_xbrl_metric"],
            **retry_policy,
        )
        self.register_job(
            job_name="footnotes",
            dataset_id="footnotes",
            dependencies=["xbrl_filing_contexts"],
            **retry_policy,
        )
        self.register_job(
            job_name="fundamental_ratios",
            dataset_id="fundamental_ratios",
            dependencies=["fundamental_standardized"],
            **retry_policy,
        )
        self.register_job(
            job_name="fundamental_reconciliation",
            dataset_id="fundamental_reconciliation",
            params={"symbols": symbols},
            dependencies=["fundamental_standardized", "xbrl_filing_contexts"],
            **retry_policy,
        )
        self.register_job(
            job_name="provider_schema_coverage",
            dataset_id="provider_schema_coverage",
            dependencies=[
                "fundamental_ratios",
                "fundamental_reconciliation",
                "fundamental_standardized",
                "daily_bars",
            ],
            **retry_policy,
        )
        self.register_job(
            job_name="short_interest_metrics",
            dataset_id="short_interest_metrics",
            dependencies=["finra_short_interest", "shares_outstanding_history"],
            **retry_policy,
        )
        self.register_job(
            job_name="sec_submissions",
            dataset_id="sec_submissions",
            params={"symbols": symbols},
            dependencies=["security_master"],
            **retry_policy,
        )
        self.register_job(
            job_name="xbrl_filing_contexts",
            dataset_id="xbrl_filing_contexts",
            params={"symbols": symbols, "max_filings": 3},
            dependencies=["sec_submissions"],
            **retry_policy,
        )
        self.register_job(
            job_name="xbrl_validation",
            dataset_id="xbrl_validation",
            dependencies=["xbrl_taxonomy", "xbrl_filing_contexts"],
            **retry_policy,
        )
        self.register_job(job_name="fred_macro", dataset_id="fred_macro", **retry_policy)
        self.register_job(
            job_name="macro_metrics",
            dataset_id="macro_metrics",
            dependencies=["fred_macro"],
            **retry_policy,
        )
        self.register_job(
            job_name="equity_price_metrics",
            dataset_id="equity_price_metrics",
            dependencies=["daily_bars"],
            **retry_policy,
        )
        self.register_job(
            job_name="thirteenf_position_metrics",
            dataset_id="thirteenf_position_metrics",
            dependencies=["sec_13f_ownership_features"],
            **retry_policy,
        )
        self.register_job(
            job_name="thirteenf_option_metrics",
            dataset_id="thirteenf_option_metrics",
            dependencies=["sec_13f_ownership_features"],
            **retry_policy,
        )
        self.register_job(
            job_name="thirteenf_concentration_metrics",
            dataset_id="thirteenf_concentration_metrics",
            dependencies=["sec_13f_ownership_features"],
            **retry_policy,
        )
        self.register_job(
            job_name="corporate_action_dividend_metrics",
            dataset_id="corporate_action_dividend_metrics",
            dependencies=["corporate_actions", "daily_bars"],
            **retry_policy,
        )
        self.register_job(
            job_name="corporate_action_split_metrics",
            dataset_id="corporate_action_split_metrics",
            dependencies=["adjustment_factor_history", "daily_adjustment_factors"],
            **retry_policy,
        )
        self.register_job(
            job_name="corporate_action_factor_reconciliation",
            dataset_id="corporate_action_factor_reconciliation",
            dependencies=["adjustment_factor_history", "daily_adjustment_factors"],
            **retry_policy,
        )
        self.register_job(
            job_name="trading_calendar",
            dataset_id="trading_calendar",
            dependencies=["daily_bars"],
            **retry_policy,
        )
        self.register_job(
            job_name="universe_memberships",
            dataset_id="universe_memberships",
            params={"symbols": symbols},
            dependencies=["daily_bars"],
            **retry_policy,
        )
        self.register_job(
            job_name="equity_daily_features",
            dataset_id="equity_daily_features",
            params={"symbols": symbols},
            dependencies=["daily_bars"],
            **retry_policy,
        )
        self.register_job(job_name="sic_taxonomy", dataset_id="sic_taxonomy", **retry_policy)
        self.register_job(job_name="fama_french_taxonomy", dataset_id="fama_french_taxonomy", **retry_policy)
        self.register_job(job_name="naics_taxonomy", dataset_id="naics_taxonomy", **retry_policy)
        self.register_job(
            job_name="entity_classification",
            dataset_id="entity_classification",
            params={"symbols": symbols},
            dependencies=["security_master", "sic_taxonomy", "fama_french_taxonomy", "naics_taxonomy"],
            **retry_policy,
        )
        # S2 estimates jobs
        self.register_job(job_name="est_measure", dataset_id="est_measure", **retry_policy)
        self.register_job(
            job_name="est_actual",
            dataset_id="est_actual",
            params={"symbols": symbols},
            # depends on sec_company_facts being loaded
            dependencies=["sec_company_facts", "est_measure"],
            **retry_policy,
        )
        self.register_job(
            job_name="est_consensus",
            dataset_id="est_consensus",
            dependencies=[],
            **retry_policy,
        )
        self.register_job(
            job_name="est_surprise",
            dataset_id="est_surprise",
            # est_consensus is default-empty but runs first when configured so surprise can use it.
            dependencies=["est_actual", "est_consensus"],
            **retry_policy,
        )
        self.register_job(
            job_name="press_release_facts",
            dataset_id="press_release_facts",
            dependencies=["est_actual"],
            **retry_policy,
        )
        self.register_job(job_name="est_detail", dataset_id="est_detail", **retry_policy)
        self.register_job(job_name="est_guidance", dataset_id="est_guidance", **retry_policy)
        self.register_job(job_name="est_recommendation", dataset_id="est_recommendation", **retry_policy)
        self.register_job(
            job_name="est_recommendation_summary",
            dataset_id="est_recommendation_summary",
            **retry_policy,
        )
        self.register_job(
            job_name="est_security_link",
            dataset_id="est_security_link",
            dependencies=[
                "identifier_resolution_decisions",
                "est_detail",
                "est_consensus",
                "est_recommendation",
                "est_recommendation_summary",
            ],
            **retry_policy,
        )

    def run_job(
        self,
        job_name: str,
        *,
        override_params: dict[str, Any] | None = None,
        max_retries: int | None = None,
        retry_delay_seconds: float | None = None,
    ) -> DatasetLoadResult:
        row = self.store.con.execute(
            """
            SELECT
                dataset_id,
                params_json,
                enabled,
                coalesce(max_retries, 0) AS max_retries,
                coalesce(retry_delay_seconds, 0) AS retry_delay_seconds
            FROM etl_job_definitions
            WHERE job_name = ?
            """,
            [job_name],
        ).fetchone()
        if row is None:
            raise KeyError(f"No ETL job definition named {job_name!r}")
        dataset_id, params_json, enabled, stored_max_retries, stored_retry_delay_seconds = row
        if not enabled:
            raise RuntimeError(f"ETL job {job_name!r} is disabled")
        params = normalize_params(params_json)
        params.update(override_params or {})
        return self.run_dataset(
            dataset_id,
            params,
            job_name=job_name,
            max_retries=stored_max_retries if max_retries is None else max_retries,
            retry_delay_seconds=(stored_retry_delay_seconds if retry_delay_seconds is None else retry_delay_seconds),
        )

    def run_dataset(
        self,
        dataset_id: str,
        params: dict[str, Any] | None = None,
        *,
        job_name: str | None = None,
        max_retries: int = 0,
        retry_delay_seconds: float = 0.0,
    ) -> DatasetLoadResult:
        if dataset_id not in DATASET_REGISTRY:
            raise KeyError(f"Unknown dataset_id {dataset_id!r}")
        max_retries = _nonnegative_int(max_retries, "max_retries")
        retry_delay_seconds = _nonnegative_float(retry_delay_seconds, "retry_delay_seconds")
        dataset_cls, option_factory = DATASET_REGISTRY[dataset_id]
        dataset = dataset_cls()
        options = option_factory(normalize_params(params))
        job_run_id = str(uuid.uuid4())
        started_at = now_utc_naive()
        name = job_name or dataset_id
        self.store.con.execute(
            """
            INSERT INTO etl_job_runs (
                job_run_id,
                job_name,
                dataset_id,
                status,
                started_at,
                attempt_count,
                max_retries,
                retry_delay_seconds,
                params_json
            )
            VALUES (?, ?, ?, 'running', ?, 0, ?, ?, ?)
            """,
            [
                job_run_id,
                name,
                dataset_id,
                started_at,
                max_retries,
                retry_delay_seconds,
                json_dumps(params or {}),
            ],
        )
        max_attempts = max_retries + 1
        for attempt in range(1, max_attempts + 1):
            self.store.con.execute(
                """
                UPDATE etl_job_runs
                SET status = 'running',
                    attempt_count = ?
                WHERE job_run_id = ?
                """,
                [attempt, job_run_id],
            )
            self.event(
                job_run_id,
                "INFO",
                f"Starting {dataset_id} attempt {attempt}",
                {"job_name": name, "attempt": attempt, "max_attempts": max_attempts},
            )
            try:
                result = dataset.run(self.store, options)
            except Exception as exc:
                error_message = f"{exc}\n{traceback.format_exc(limit=20)}"
                if attempt < max_attempts:
                    self.store.con.execute(
                        """
                        UPDATE etl_job_runs
                        SET status = 'retrying',
                            attempt_count = ?,
                            error_message = ?
                        WHERE job_run_id = ?
                        """,
                        [attempt, error_message, job_run_id],
                    )
                    self.event(
                        job_run_id,
                        "WARN",
                        f"Retrying {dataset_id} after attempt {attempt} failed",
                        {
                            "attempt": attempt,
                            "max_attempts": max_attempts,
                            "retry_delay_seconds": retry_delay_seconds,
                            "error": str(exc),
                        },
                    )
                    if retry_delay_seconds > 0:
                        time.sleep(retry_delay_seconds)
                    continue

                finished_at = now_utc_naive()
                self.store.con.execute(
                    """
                    UPDATE etl_job_runs
                    SET status = 'failed',
                        finished_at = ?,
                        attempt_count = ?,
                        error_message = ?
                    WHERE job_run_id = ?
                    """,
                    [finished_at, attempt, error_message, job_run_id],
                )
                self.event(
                    job_run_id,
                    "ERROR",
                    f"Failed {dataset_id} after {attempt} attempt(s)",
                    {"attempt": attempt, "max_attempts": max_attempts, "error": str(exc)},
                )
                raise
            break

        finished_at = now_utc_naive()
        self.store.con.execute(
            """
            UPDATE etl_job_runs
            SET status = 'succeeded',
                finished_at = ?,
                dataset_run_id = ?,
                rows_loaded = ?,
                error_message = NULL
            WHERE job_run_id = ?
            """,
            [finished_at, result.run_id, result.rows_loaded, job_run_id],
        )
        self.event(
            job_run_id,
            "INFO",
            f"Completed {dataset_id}",
            {"rows_loaded": result.rows_loaded, "attempt_count": attempt},
        )
        return result

    def event(
        self,
        job_run_id: str,
        level: str,
        message: str,
        details: dict[str, Any] | None = None,
    ) -> None:
        self.store.con.execute(
            """
            INSERT INTO etl_job_events (
                job_run_id,
                event_time,
                level,
                message,
                details_json
            )
            VALUES (?, ?, ?, ?, ?)
            """,
            [job_run_id, now_utc_naive(), level, message, json_dumps(details or {})],
        )

    def _enabled_orchestrator_registry_and_params(
        self,
    ) -> tuple[dict[str, tuple[type[Dataset], OptionFactory]], dict[str, dict[str, Any]]]:
        rows = self.store.con.execute(
            """
            SELECT job_name, dataset_id, params_json
            FROM etl_job_definitions
            WHERE enabled
            ORDER BY
                CASE WHEN job_name = dataset_id THEN 0 ELSE 1 END,
                created_at,
                job_name
            """
        ).fetchall()
        registry: dict[str, tuple[type[Dataset], OptionFactory]] = {}
        params_by_dataset: dict[str, dict[str, Any]] = {}
        for job_name, dataset_id, params_json in rows:
            dataset_id = str(dataset_id)
            if dataset_id not in DATASET_REGISTRY:
                continue
            registry.setdefault(dataset_id, DATASET_REGISTRY[dataset_id])
            params_by_dataset[dataset_id] = _merge_orchestrator_job_params(
                dataset_id,
                params_by_dataset.get(dataset_id, {}),
                normalize_params(params_json),
            )
        return registry, params_by_dataset

    def _orchestrator_registry_for_run(
        self,
        run_id: str,
    ) -> dict[str, tuple[type[Dataset], OptionFactory]]:
        rows = self.store.con.execute(
            """
            SELECT dataset_id
            FROM etl_job_steps
            WHERE run_id = ?
            ORDER BY dataset_id
            """,
            [run_id],
        ).fetchall()
        if not rows:
            raise KeyError(f"No orchestrator steps found for run_id {run_id!r}")
        dataset_ids = [str(row[0]) for row in rows]
        missing = sorted(dataset_id for dataset_id in dataset_ids if dataset_id not in DATASET_REGISTRY)
        if missing:
            raise KeyError("Orchestrator run references unknown dataset_id(s): " + ", ".join(missing))
        return {dataset_id: DATASET_REGISTRY[dataset_id] for dataset_id in dataset_ids}

    def _orchestrator_params_for_run(self, run_id: str) -> dict[str, Any]:
        row = self.store.con.execute(
            """
            SELECT params_json
            FROM etl_job_runs
            WHERE run_id = ? AND run_kind = 'orchestrator'
            """,
            [run_id],
        ).fetchone()
        if row is None:
            raise KeyError(f"No orchestrator run found for run_id {run_id!r}")
        return normalize_params(row[0])

    def refresh_quant_warehouse(
        self,
        *,
        params: dict[str, Any] | None = None,
        registry: dict[str, tuple[type[Dataset], OptionFactory]] | None = None,
        run_id: str | None = None,
        resume: str | None = None,
        full_rebuild: bool = False,
        git_sha: str | None = None,
        actor: str = "warehouse_jobs",
    ) -> OrchestratorResult:
        run_params = normalize_params(params)
        selected_registry = registry
        if selected_registry is None:
            if resume:
                selected_registry = self._orchestrator_registry_for_run(resume)
                if not run_params:
                    run_params = self._orchestrator_params_for_run(resume)
            else:
                selected_registry, dataset_params = self._enabled_orchestrator_registry_and_params()
                if dataset_params:
                    merged_dataset_params = dict(run_params.get(DATASET_PARAMS_KEY, {}))
                    merged_dataset_params.update(dataset_params)
                    run_params[DATASET_PARAMS_KEY] = merged_dataset_params
        return refresh_quant_warehouse(
            self.store,
            params=run_params,
            registry=selected_registry,
            run_id=run_id,
            resume=resume,
            full_rebuild=full_rebuild,
            git_sha=git_sha,
            actor=actor,
        )

    def run_all_enabled(
        self,
        *,
        full_rebuild: bool = False,
        resume: str | None = None,
        registry: dict[str, tuple[type[Dataset], OptionFactory]] | None = None,
        run_id: str | None = None,
        git_sha: str | None = None,
    ) -> OrchestratorResult:
        return self.refresh_quant_warehouse(
            registry=registry,
            run_id=run_id,
            resume=resume,
            full_rebuild=full_rebuild,
            git_sha=git_sha,
        )

    def run_all_results(
        self,
        run_id: str,
        dataset_order: tuple[str, ...] | None = None,
    ) -> list[DatasetLoadResult]:
        return orchestrator_step_results(self.store, run_id, dataset_order)

    def enabled_job_order(self) -> list[str]:
        rows = self.store.con.execute(
            """
            SELECT job_name, dependencies_json
            FROM etl_job_definitions
            WHERE enabled
            ORDER BY created_at, job_name
            """
        ).fetchall()
        dependencies = {job_name: normalize_dependencies(dependencies_json) for job_name, dependencies_json in rows}
        missing = sorted(
            {
                dependency
                for job_dependencies in dependencies.values()
                for dependency in job_dependencies
                if dependency not in dependencies
            }
        )
        if missing:
            raise RuntimeError(f"Enabled ETL jobs have missing or disabled dependencies: {missing}")

        ordered: list[str] = []
        visiting: set[str] = set()
        visited: set[str] = set()

        def visit(job_name: str, stack: list[str]) -> None:
            if job_name in visited:
                return
            if job_name in visiting:
                cycle = " -> ".join([*stack, job_name])
                raise RuntimeError(f"Cycle detected in ETL job dependencies: {cycle}")
            visiting.add(job_name)
            stack.append(job_name)
            for dependency in dependencies[job_name]:
                visit(dependency, stack)
            stack.pop()
            visiting.remove(job_name)
            visited.add(job_name)
            ordered.append(job_name)

        for job_name in dependencies:
            visit(job_name, [])
        return ordered
