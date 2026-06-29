from __future__ import annotations

import datetime as dt
import json
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
from .finra import FinraShortInterestDataset, FinraShortInterestOptions, parse_date
from .fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions
from .identifier_decisions import IdentifierResolutionDecisionDataset, IdentifierResolutionDecisionOptions
from .identifier_resolution import IdentifierResolutionCandidateDataset, IdentifierResolutionOptions
from .insider_ownership import (
    DEFAULT_BLOCKHOLDER_FORMS,
    DEFAULT_FORMS,
    BlockholderOwnershipDataset,
    BlockholderOwnershipOptions,
    InsiderOwnershipDataset,
    InsiderOwnershipOptions,
)
from .listing_status import ListingStatusIntervalDataset, ListingStatusIntervalOptions
from .macro import FredMacroDataset, FredMacroOptions
from .ownership import OwnershipFeatureDataset, OwnershipFeatureOptions
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
            _lower_tuple_or_none(params.get("accept_candidate_statuses"))
            or default.accept_candidate_statuses
        ),
        review_candidate_statuses=(
            _lower_tuple_or_none(params.get("review_candidate_statuses"))
            or default.review_candidate_statuses
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


def _entity_classification_options(params: dict[str, Any]) -> EntityClassificationOptions:
    _default = EntityClassificationOptions()
    return EntityClassificationOptions(
        symbols=_tuple_or_none(params.get("symbols")),
        user_agent=params.get("user_agent", _default.user_agent),
        request_timeout=int(params.get("request_timeout", _default.request_timeout)),
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
    OwnershipFeatureDataset.dataset_id: (OwnershipFeatureDataset, _ownership_feature_options),
    InsiderOwnershipDataset.dataset_id: (InsiderOwnershipDataset, _insider_ownership_options),
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
        lambda p: EstimateGuidanceOptions(),
    ),
    EstimateRecommendationDataset.dataset_id: (
        EstimateRecommendationDataset,
        _estimate_recommendation_options,
    ),
}


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
        self.register_job(job_name="xbrl_taxonomy", dataset_id="xbrl_taxonomy", **retry_policy)
        self.register_job(
            job_name="sec_fundamental_features",
            dataset_id="sec_fundamental_features",
            params={"symbols": symbols},
            dependencies=["sec_company_facts", "daily_bars"],
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
        self.register_job(job_name="est_detail", dataset_id="est_detail", **retry_policy)
        self.register_job(job_name="est_guidance", dataset_id="est_guidance", **retry_policy)
        self.register_job(
            job_name="est_recommendation", dataset_id="est_recommendation", **retry_policy
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
            retry_delay_seconds=(
                stored_retry_delay_seconds
                if retry_delay_seconds is None
                else retry_delay_seconds
            ),
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

    def run_all_enabled(self) -> list[DatasetLoadResult]:
        return [self.run_job(job_name) for job_name in self.enabled_job_order()]

    def enabled_job_order(self) -> list[str]:
        rows = self.store.con.execute(
            """
            SELECT job_name, dependencies_json
            FROM etl_job_definitions
            WHERE enabled
            ORDER BY created_at, job_name
            """
        ).fetchall()
        dependencies = {
            job_name: normalize_dependencies(dependencies_json)
            for job_name, dependencies_json in rows
        }
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
