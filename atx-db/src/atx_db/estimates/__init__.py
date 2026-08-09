"""Estimates store datasets for the atx-db DuckDB warehouse.

Provides:
- EstimateMeasureSeedDataset  ? static dimension seed (5 measures)
- EstimateActualsDataset       ? REAL: reads sec_company_facts, maps XBRL?measure
- EstimateSurpriseDataset      ? REAL derived: Standardized Unexpected Earnings (SUE)
                                  Foster-Olsen-Shevlin (1984) seasonal-random-walk-with-drift.
- EstimateConsensusDataset     ? injectable; default-empty (licensed: IBES/FactSet/Zacks)
- EstimateGuidanceDataset      ? injectable plus local SEC 8-K guidance text extraction
- EstimateRecommendationDataset ? injectable; default-empty (licensed vendor)

PIT contract
------------
est_actual   : available_at CARRIED from sec_company_facts (filing availability, UTC-naive).
               Do NOT restamp to now().
est_surprise : available_at CARRIED from the originally-reported actual_t (SUE is knowable
               when period t is filed).  Prior-period restated values do NOT alter earlier SUE rows.
est_consensus: available_at carried from provider/source row if supplied; injectable
               files fall back to snapshot_date/statpers end-of-day; callables fall
               back to now_utc_naive() for backward compatibility.
est_guidance : available_at carried from provider/source row if supplied; local
               SEC 8-K text extraction falls back to acceptance_datetime or
               guidance_date end-of-day. Callable rows fall back to now_utc_naive().
est_recommendation: available_at carried from provider/source row if supplied; injectable
                    files fall back to activation/anndats/rating_date timestamps;
                    callables fall back to now_utc_naive() for compatibility.

Revenue concept preference
--------------------------
For measure_code=REVENUE we prefer concept "Revenues".  If a security/fy/fp/accession has
no "Revenues" value we fall back to "RevenueFromContractWithCustomerExcludingAssessedTax".
Implemented via a SQL window function that ranks concepts per PK group.
"""
from __future__ import annotations

import sys as _sys
import types as _types

from . import _columns as _columns
from . import _common as _common
from . import measure_actuals as _measure_actuals
from . import surprise as _surprise
from . import detail as _detail
from . import consensus as _consensus
from . import guidance as _guidance
from . import recommendations as _recommendations
from ._common import (
    Any,
    Callable,
    Dataset,
    DatasetLoadResult,
    DuckDBStore,
    Iterable,
    Path,
    _bool_series,
    _canonical_measure,
    _date_series,
    _derive_party_id,
    _hash_id,
    _insert_or_replace_frame,
    _integer_series,
    _numeric_series,
    _period_type_from_fpi,
    _quarter_label,
    _raw_payloads,
    _safe_provider,
    _series_clean,
    _string_series,
    _time_series,
    _timestamp_from_date_time,
    _timestamp_from_dates_end_of_day,
    _timestamp_series,
    dataclass,
    dt,
    file_sha256,
    hashlib,
    insert_frame,
    json,
    json_dumps,
    math,
    now_utc_naive,
    pd,
    quality_check,
    re,
    record_source_file,
    security_id_for_symbol,
    snake_case,
    symbol_key,
)
from ._columns import (
    CONSENSUS_COLUMN_ALIASES,
    DETAIL_COLUMN_ALIASES,
    ESTIMATE_ANALYST_ALIAS_COLUMNS,
    ESTIMATE_ANALYST_COLUMNS,
    ESTIMATE_BROKER_ALIAS_COLUMNS,
    ESTIMATE_BROKER_COLUMNS,
    ESTIMATE_CONSENSUS_COLUMNS,
    ESTIMATE_DETAIL_COLUMNS,
    ESTIMATE_GUIDANCE_COLUMNS,
    ESTIMATE_PERIOD_COLUMNS,
    ESTIMATE_RECOMMENDATION_COLUMNS,
    ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS,
    GUIDANCE_COLUMN_ALIASES,
    GUIDANCE_MEASURE_PATTERNS,
    GUIDANCE_SOURCE_CUE_RE,
    GUIDANCE_TEXT_COLUMNS,
    GUIDANCE_VALUE_RE,
    IBES_MEASURE_MAP,
    RECOMMENDATION_COLUMN_ALIASES,
    RECOMMENDATION_LABELS,
    RECOMMENDATION_SUMMARY_COLUMN_ALIASES,
    RECOMMENDATION_TEXT_MAP,
    _MEASURE_ROWS,
)
from .measure_actuals import (
    EstimateActualsDataset,
    EstimateActualsOptions,
    EstimateMeasureSeedDataset,
    EstimateMeasureSeedOptions,
    _build_concept_map,
)
from .surprise import (
    EstimateSurpriseDataset,
    EstimateSurpriseOptions,
    _compute_sue_series,
    _normalize_basis_tag,
)
from .detail import (
    EstimateDetailDataset,
    EstimateDetailOptions,
    _empty_estimate_detail_frame,
    _estimate_detail_dimensions,
    _normalize_detail_columns,
    load_estimate_detail_rows,
    normalize_estimate_detail_rows,
)
from .consensus import (
    EstimateConsensusDataset,
    EstimateConsensusOptions,
    _derive_consensus_fiscal_period,
    _empty_estimate_consensus_frame,
    _estimate_consensus_periods,
    _normalize_consensus_columns,
    _provider_name_series,
    _source_series,
    _stale_after_series,
    _write_estimate_consensus_frame,
    load_estimate_consensus_rows,
    normalize_estimate_consensus_rows,
)
from .guidance import (
    EstimateGuidanceDataset,
    EstimateGuidanceOptions,
    _clean_guidance_text,
    _empty_estimate_guidance_frame,
    _extract_guidance_rows_from_record,
    _guidance_basis,
    _guidance_confidence,
    _guidance_date_value,
    _guidance_period_from_text,
    _guidance_record_value,
    _guidance_scale,
    _guidance_source_item,
    _guidance_ts_value,
    _guidance_type,
    _guidance_values_after,
    _normalize_guidance_columns,
    _period_end_from_fiscal_fields,
    _quarter_end,
    _read_guidance_source_file,
    _write_estimate_guidance_frame,
    normalize_estimate_guidance_rows,
)
from .recommendations import (
    EstimateRecommendationDataset,
    EstimateRecommendationOptions,
    EstimateRecommendationSummaryDataset,
    EstimateRecommendationSummaryOptions,
    _canonical_recommendation_mean,
    _count_series,
    _empty_estimate_recommendation_frame,
    _empty_estimate_recommendation_summary_frame,
    _estimate_recommendation_dimensions,
    _infer_recommendation_summary_source_table,
    _normalize_recommendation_columns,
    _normalize_recommendation_summary_columns,
    _recommendation_action,
    _recommendation_code,
    _recommendation_label,
    _scale_direction_values,
    _standardized_rating,
    _sum_count_series,
    _write_estimate_recommendation_frame,
    _write_estimate_recommendation_summary_frame,
    load_estimate_recommendation_rows,
    load_estimate_recommendation_summary_rows,
    normalize_estimate_recommendation_rows,
    normalize_estimate_recommendation_summary_rows,
)

__estimate_public_names__ = [
    "Any",
    "CONSENSUS_COLUMN_ALIASES",
    "Callable",
    "DETAIL_COLUMN_ALIASES",
    "Dataset",
    "DatasetLoadResult",
    "DuckDBStore",
    "ESTIMATE_ANALYST_ALIAS_COLUMNS",
    "ESTIMATE_ANALYST_COLUMNS",
    "ESTIMATE_BROKER_ALIAS_COLUMNS",
    "ESTIMATE_BROKER_COLUMNS",
    "ESTIMATE_CONSENSUS_COLUMNS",
    "ESTIMATE_DETAIL_COLUMNS",
    "ESTIMATE_GUIDANCE_COLUMNS",
    "ESTIMATE_PERIOD_COLUMNS",
    "ESTIMATE_RECOMMENDATION_COLUMNS",
    "ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS",
    "EstimateActualsDataset",
    "EstimateActualsOptions",
    "EstimateConsensusDataset",
    "EstimateConsensusOptions",
    "EstimateDetailDataset",
    "EstimateDetailOptions",
    "EstimateGuidanceDataset",
    "EstimateGuidanceOptions",
    "EstimateMeasureSeedDataset",
    "EstimateMeasureSeedOptions",
    "EstimateRecommendationDataset",
    "EstimateRecommendationOptions",
    "EstimateRecommendationSummaryDataset",
    "EstimateRecommendationSummaryOptions",
    "EstimateSurpriseDataset",
    "EstimateSurpriseOptions",
    "GUIDANCE_COLUMN_ALIASES",
    "GUIDANCE_MEASURE_PATTERNS",
    "GUIDANCE_SOURCE_CUE_RE",
    "GUIDANCE_TEXT_COLUMNS",
    "GUIDANCE_VALUE_RE",
    "IBES_MEASURE_MAP",
    "Iterable",
    "Path",
    "RECOMMENDATION_COLUMN_ALIASES",
    "RECOMMENDATION_LABELS",
    "RECOMMENDATION_SUMMARY_COLUMN_ALIASES",
    "RECOMMENDATION_TEXT_MAP",
    "_MEASURE_ROWS",
    "_bool_series",
    "_build_concept_map",
    "_canonical_measure",
    "_canonical_recommendation_mean",
    "_clean_guidance_text",
    "_compute_sue_series",
    "_count_series",
    "_date_series",
    "_derive_consensus_fiscal_period",
    "_derive_party_id",
    "_empty_estimate_consensus_frame",
    "_empty_estimate_detail_frame",
    "_empty_estimate_guidance_frame",
    "_empty_estimate_recommendation_frame",
    "_empty_estimate_recommendation_summary_frame",
    "_estimate_consensus_periods",
    "_estimate_detail_dimensions",
    "_estimate_recommendation_dimensions",
    "_extract_guidance_rows_from_record",
    "_guidance_basis",
    "_guidance_confidence",
    "_guidance_date_value",
    "_guidance_period_from_text",
    "_guidance_record_value",
    "_guidance_scale",
    "_guidance_source_item",
    "_guidance_ts_value",
    "_guidance_type",
    "_guidance_values_after",
    "_hash_id",
    "_infer_recommendation_summary_source_table",
    "_insert_or_replace_frame",
    "_integer_series",
    "_normalize_basis_tag",
    "_normalize_consensus_columns",
    "_normalize_detail_columns",
    "_normalize_guidance_columns",
    "_normalize_recommendation_columns",
    "_normalize_recommendation_summary_columns",
    "_numeric_series",
    "_period_end_from_fiscal_fields",
    "_period_type_from_fpi",
    "_provider_name_series",
    "_quarter_end",
    "_quarter_label",
    "_raw_payloads",
    "_read_guidance_source_file",
    "_recommendation_action",
    "_recommendation_code",
    "_recommendation_label",
    "_safe_provider",
    "_scale_direction_values",
    "_series_clean",
    "_source_series",
    "_stale_after_series",
    "_standardized_rating",
    "_string_series",
    "_sum_count_series",
    "_time_series",
    "_timestamp_from_date_time",
    "_timestamp_from_dates_end_of_day",
    "_timestamp_series",
    "_write_estimate_consensus_frame",
    "_write_estimate_guidance_frame",
    "_write_estimate_recommendation_frame",
    "_write_estimate_recommendation_summary_frame",
    "annotations",
    "dataclass",
    "dt",
    "file_sha256",
    "hashlib",
    "insert_frame",
    "json",
    "json_dumps",
    "load_estimate_consensus_rows",
    "load_estimate_detail_rows",
    "load_estimate_recommendation_rows",
    "load_estimate_recommendation_summary_rows",
    "math",
    "normalize_estimate_consensus_rows",
    "normalize_estimate_detail_rows",
    "normalize_estimate_guidance_rows",
    "normalize_estimate_recommendation_rows",
    "normalize_estimate_recommendation_summary_rows",
    "now_utc_naive",
    "pd",
    "quality_check",
    "re",
    "record_source_file",
    "security_id_for_symbol",
    "snake_case",
    "symbol_key",
]

__estimate_sync_modules__ = (
    _columns,
    _common,
    _measure_actuals,
    _surprise,
    _detail,
    _consensus,
    _guidance,
    _recommendations,
)

for _name in __estimate_public_names__:
    _obj = globals().get(_name)
    if getattr(_obj, "__module__", "").startswith(f"{__name__}."):
        _obj.__module__ = __name__


class __EstimateModule(_types.ModuleType):
    def __setattr__(self, name: str, value: object) -> None:
        super().__setattr__(name, value)
        for __module in __estimate_sync_modules__:
            if name in __module.__dict__:
                setattr(__module, name, value)

    def __dir__(self) -> list[str]:
        return sorted(
            set(__estimate_public_names__) | {name for name in globals() if name.startswith("__")}
        )


_sys.modules[__name__].__class__ = __EstimateModule

for __estimate_hidden_name in (
    "_columns",
    "_common",
    "_measure_actuals",
    "_surprise",
    "_detail",
    "_consensus",
    "_guidance",
    "_recommendations",
    "_sys",
    "_types",
    "consensus",
    "detail",
    "guidance",
    "measure_actuals",
    "recommendations",
    "surprise",
):
    globals().pop(__estimate_hidden_name, None)

del _name, _obj, __estimate_hidden_name
