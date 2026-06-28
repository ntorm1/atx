"""DuckDB-backed dataset utilities for atx-impl."""

from .asof import (
    corporate_actions_asof,
    daily_panel_asof,
    features_asof,
    fundamental_periods_asof,
    fundamental_statements_asof,
    fundamental_ttm_asof,
    fundamentals_asof,
    identifier_decisions_asof,
    listing_status_asof,
    macro_asof,
    ownership_asof,
    security_master_asof,
    short_interest_asof,
    thirteenf_positioning_asof,
    universe_asof,
)
from .alpha_research import AlphaResearchDataset, AlphaResearchOptions
from .connection import DEFAULT_DB_PATH, DuckDBStore, connect
from .corporate_actions import CorporateActionsDataset, CorporateActionsOptions
from .dataset import Dataset, DatasetLoadResult
from .features import (
    EquityDailyFeatureDataset,
    FeatureBuildOptions,
    FundamentalFeatureBuildOptions,
    FundamentalFeatureDataset,
    refresh_feature_lineage,
)
from .finra import FinraShortInterestDataset, FinraShortInterestOptions
from .fundamental_statements import (
    refresh_fundamental_periods,
    refresh_fundamental_statement_points,
    refresh_fundamental_ttm_points,
    seed_fundamental_statement_map,
)
from .fundamentals import (
    COMPANY_FACT_SYMBOL_SOURCES,
    SecCompanyFactsDataset,
    SecCompanyFactsOptions,
    refresh_fundamental_fact_revisions,
    refresh_xbrl_concept_catalog,
    resolve_companyfacts_targets,
)
from .identifier_decisions import IdentifierResolutionDecisionDataset, IdentifierResolutionDecisionOptions
from .identifier_resolution import IdentifierResolutionCandidateDataset, IdentifierResolutionOptions
from .jobs import JobManager
from .lake import LakeValidationProblem, LakeValidationSummary, validate_lake_export
from .listing_status import ListingStatusIntervalDataset, ListingStatusIntervalOptions, build_listing_status_intervals
from .macro import FredMacroDataset, FredMacroOptions
from .ownership import OwnershipFeatureDataset, OwnershipFeatureOptions
from .quality import QualityResult, run_warehouse_quality_checks
from .queries import SHORT_INTEREST_WITH_13F_SQL, short_interest_with_13f_positioning
from .sec_submissions import SecSubmissionsDataset, SecSubmissionsOptions
from .security_master import SecurityMasterDataset, SecurityMasterOptions
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
from .migrations import MIGRATIONS, Migration, apply_pending_migrations
from .watermarks import WatermarkRefreshResult, refresh_warehouse_watermarks
from .xbrl_filing_contexts import XbrlFilingContextDataset, XbrlFilingContextOptions, archive_primary_document_url
from .xbrl_taxonomy import (
    XbrlTaxonomyDataset,
    XbrlTaxonomyOptions,
    refresh_xbrl_fact_frames,
    refresh_xbrl_taxonomy,
)

__all__ = [
    "DEFAULT_DB_PATH",
    "AlphaResearchDataset",
    "AlphaResearchOptions",
    "CorporateActionsDataset",
    "CorporateActionsOptions",
    "COMPANY_FACT_SYMBOL_SOURCES",
    "Dataset",
    "DatasetLoadResult",
    "DuckDBStore",
    "EquityDailyFeatureDataset",
    "FeatureBuildOptions",
    "FinraShortInterestDataset",
    "FinraShortInterestOptions",
    "FredMacroDataset",
    "FredMacroOptions",
    "FundamentalFeatureBuildOptions",
    "FundamentalFeatureDataset",
    "fundamental_statements_asof",
    "fundamental_ttm_asof",
    "IdentifierResolutionDecisionDataset",
    "IdentifierResolutionDecisionOptions",
    "IdentifierResolutionCandidateDataset",
    "IdentifierResolutionOptions",
    "JobManager",
    "MIGRATIONS",
    "Migration",
    "apply_pending_migrations",
    "LakeValidationProblem",
    "LakeValidationSummary",
    "ListingStatusIntervalDataset",
    "ListingStatusIntervalOptions",
    "NasdaqListingEventsDataset",
    "NasdaqListingEventsOptions",
    "NasdaqSymbolDirectoryDataset",
    "NasdaqSymbolDirectoryOptions",
    "OwnershipFeatureDataset",
    "OwnershipFeatureOptions",
    "QualityResult",
    "SHORT_INTEREST_WITH_13F_SQL",
    "SecCompanyFactsDataset",
    "SecCompanyFactsOptions",
    "SecSubmissionsDataset",
    "SecSubmissionsOptions",
    "SecurityMasterDataset",
    "SecurityMasterOptions",
    "ShortInterestFeatureDataset",
    "ShortInterestFeatureOptions",
    "ThirteenFDataSet",
    "ThirteenFOptions",
    "TickerHistoryDataset",
    "TickerHistoryOptions",
    "UniverseBuildOptions",
    "UniverseMembershipDataset",
    "WatermarkRefreshResult",
    "XbrlFilingContextDataset",
    "XbrlFilingContextOptions",
    "XbrlTaxonomyDataset",
    "XbrlTaxonomyOptions",
    "archive_primary_document_url",
    "build_listing_status_intervals",
    "connect",
    "corporate_actions_asof",
    "daily_panel_asof",
    "features_asof",
    "fundamental_periods_asof",
    "fundamentals_asof",
    "identifier_decisions_asof",
    "listing_status_asof",
    "macro_asof",
    "ownership_asof",
    "run_warehouse_quality_checks",
    "refresh_fundamental_fact_revisions",
    "refresh_fundamental_periods",
    "refresh_fundamental_statement_points",
    "refresh_fundamental_ttm_points",
    "refresh_warehouse_watermarks",
    "refresh_feature_lineage",
    "refresh_xbrl_concept_catalog",
    "refresh_xbrl_fact_frames",
    "refresh_xbrl_taxonomy",
    "resolve_companyfacts_targets",
    "security_master_asof",
    "short_interest_asof",
    "short_interest_with_13f_positioning",
    "seed_fundamental_statement_map",
    "thirteenf_positioning_asof",
    "universe_asof",
    "validate_lake_export",
]
