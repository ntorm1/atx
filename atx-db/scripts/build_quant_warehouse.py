#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.adjustment_factors import AdjustmentFactorHistoryDataset, AdjustmentFactorHistoryOptions
from atx_db.calendar import TradingCalendarDataset, TradingCalendarOptions
from atx_db.corporate_actions import CorporateActionsDataset, CorporateActionsOptions
from atx_db.daily_adjustments import DailyAdjustmentFactorDataset, DailyAdjustmentFactorOptions
from atx_db.delisting import (
    DelistingEventDataset,
    DelistingEventOptions,
    DelistingReturnObservationDataset,
    DelistingReturnObservationOptions,
)
from atx_db.features import (
    EquityDailyFeatureDataset,
    FeatureBuildOptions,
    FundamentalFeatureBuildOptions,
    FundamentalFeatureDataset,
)
from atx_db.estimates import (
    EstimateActualsDataset,
    EstimateActualsOptions,
    EstimateConsensusDataset,
    EstimateConsensusOptions,
    EstimateDetailDataset,
    EstimateDetailOptions,
    EstimateGuidanceDataset,
    EstimateGuidanceOptions,
    EstimateMeasureSeedDataset,
    EstimateMeasureSeedOptions,
    EstimateRecommendationDataset,
    EstimateRecommendationOptions,
    EstimateRecommendationSummaryDataset,
    EstimateRecommendationSummaryOptions,
    EstimateSurpriseDataset,
    EstimateSurpriseOptions,
)
from atx_db.estimate_security_links import EstimateSecurityLinkDataset, EstimateSecurityLinkOptions
from atx_db.finra import FinraShortInterestDataset, FinraShortInterestOptions, parse_date
from atx_db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions
from atx_db.identifier_decisions import IdentifierResolutionDecisionDataset, IdentifierResolutionDecisionOptions
from atx_db.identifier_resolution import IdentifierResolutionCandidateDataset, IdentifierResolutionOptions
from atx_db.macro import FredMacroDataset, FredMacroOptions
from atx_db.sec_submissions import SecSubmissionsDataset, SecSubmissionsOptions
from atx_db.security_master import SecurityMasterDataset, SecurityMasterOptions
from atx_db.shares_outstanding import SharesOutstandingHistoryDataset, SharesOutstandingHistoryOptions
from atx_db.symbol_directory import NasdaqSymbolDirectoryDataset, NasdaqSymbolDirectoryOptions
from atx_db.symbol_directory import NasdaqListingEventsDataset, NasdaqListingEventsOptions
from atx_db.listing_status import ListingStatusIntervalDataset, ListingStatusIntervalOptions
from atx_db.thirteenf import ThirteenFDataSet, ThirteenFOptions
from atx_db.ticker_history import DEFAULT_TICKER_HISTORY_ZIP, TickerHistoryDataset, TickerHistoryOptions
from atx_db.universes import UniverseBuildOptions, UniverseMembershipDataset
from atx_db.warehouse import symbol_key


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a Python/DuckDB public-data quant equity warehouse for atx-db."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--symbols", default="AAPL", help="Comma-separated symbol list for focused loads.")
    parser.add_argument("--start-date", type=parse_date)
    parser.add_argument("--end-date", type=parse_date)
    parser.add_argument("--ticker-chunk-size", type=int, default=200_000)
    parser.add_argument("--ticker-max-chunks", type=int, help="Smoke-test cap; omit for full ZIP scan.")
    parser.add_argument("--skip-fundamentals", action="store_true")
    parser.add_argument("--skip-shares-outstanding", action="store_true")
    parser.add_argument("--skip-fundamental-features", action="store_true")
    parser.add_argument("--skip-estimates", action="store_true")
    parser.add_argument("--estimate-guidance-file", type=Path)
    parser.add_argument("--estimate-guidance-source", default="sec_8k_guidance_regex_v1")
    parser.add_argument(
        "--estimate-guidance-min-confidence",
        type=float,
        default=EstimateGuidanceOptions().min_confidence,
    )
    parser.add_argument("--estimate-consensus-file", type=Path)
    parser.add_argument("--estimate-consensus-provider", default=EstimateConsensusOptions().provider_name)
    parser.add_argument(
        "--estimate-consensus-vendor-id-type",
        default=EstimateConsensusOptions().vendor_security_id_type,
    )
    parser.add_argument(
        "--estimate-consensus-stale-days",
        type=int,
        default=EstimateConsensusOptions().stale_after_days,
    )
    parser.add_argument("--estimate-detail-file", type=Path)
    parser.add_argument("--estimate-detail-provider", default=EstimateDetailOptions().provider)
    parser.add_argument(
        "--estimate-detail-vendor-id-type",
        default=EstimateDetailOptions().vendor_security_id_type,
    )
    parser.add_argument("--estimate-recommendation-file", type=Path)
    parser.add_argument("--estimate-recommendation-provider", default=EstimateRecommendationOptions().provider_name)
    parser.add_argument(
        "--estimate-recommendation-vendor-id-type",
        default=EstimateRecommendationOptions().vendor_security_id_type,
    )
    parser.add_argument(
        "--estimate-recommendation-source-table",
        default=EstimateRecommendationOptions().source_vendor_table,
    )
    parser.add_argument("--estimate-recommendation-summary-file", type=Path)
    parser.add_argument(
        "--estimate-recommendation-summary-provider",
        default=EstimateRecommendationSummaryOptions().provider_name,
    )
    parser.add_argument(
        "--estimate-recommendation-summary-vendor-id-type",
        default=EstimateRecommendationSummaryOptions().vendor_security_id_type,
    )
    parser.add_argument(
        "--estimate-recommendation-summary-source-table",
        default=EstimateRecommendationSummaryOptions().source_vendor_table,
    )
    parser.add_argument(
        "--estimate-recommendation-summary-rating-scale",
        default=EstimateRecommendationSummaryOptions().rating_scale,
    )
    parser.add_argument(
        "--estimate-recommendation-summary-scale-direction",
        default=EstimateRecommendationSummaryOptions().scale_direction,
        choices=("LOWER_IS_BULLISH", "HIGHER_IS_BULLISH"),
    )
    parser.add_argument("--skip-estimate-security-links", action="store_true")
    parser.add_argument(
        "--estimate-security-link-min-confidence",
        type=float,
        default=EstimateSecurityLinkOptions().min_confidence,
    )
    parser.add_argument(
        "--skip-estimate-security-link-identifier-history",
        action="store_true",
        help="Do not promote accepted estimate vendor-id links into security_identifier_history.",
    )
    parser.add_argument("--skip-submissions", action="store_true")
    parser.add_argument("--skip-13f", action="store_true")
    parser.add_argument("--skip-identifier-resolution", action="store_true")
    parser.add_argument("--skip-identifier-decisions", action="store_true")
    parser.add_argument("--skip-corporate-actions", action="store_true")
    parser.add_argument("--skip-adjustment-factors", action="store_true")
    parser.add_argument("--skip-daily-adjustments", action="store_true")
    parser.add_argument("--skip-symbol-directory", action="store_true")
    parser.add_argument("--skip-listing-events", action="store_true")
    parser.add_argument("--skip-listing-status", action="store_true")
    parser.add_argument("--skip-delistings", action="store_true")
    parser.add_argument("--impute-delist-returns", action="store_true")
    parser.add_argument("--delisting-return-file", type=Path)
    parser.add_argument("--delisting-return-provider", default=DelistingReturnObservationOptions().provider)
    parser.add_argument(
        "--delisting-return-vendor-id-type",
        default=DelistingReturnObservationOptions().vendor_security_id_type,
    )
    parser.add_argument("--skip-macro", action="store_true")
    parser.add_argument("--skip-universe", action="store_true")
    parser.add_argument("--macro-series", default="DGS10,DGS2,FEDFUNDS,UNRATE,CPIAUCSL,VIXCLS")
    parser.add_argument("--full-13f-holdings", action="store_true")
    parser.add_argument("--identifier-resolution-min-confidence", type=float, default=IdentifierResolutionOptions().min_confidence)
    parser.add_argument("--identifier-decision-accept-proposed", action="store_true")
    parser.add_argument("--universe-id", default=UniverseBuildOptions().universe_id)
    parser.add_argument("--universe-lookback-days", type=int, default=UniverseBuildOptions().lookback_days)
    parser.add_argument("--universe-min-history-days", type=int, default=UniverseBuildOptions().min_history_days)
    parser.add_argument("--universe-min-price", type=float, default=UniverseBuildOptions().min_price)
    parser.add_argument("--universe-min-dollar-volume", type=float, default=UniverseBuildOptions().min_dollar_volume)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(symbol_key(part) for part in args.symbols.split(",") if symbol_key(part))
    if not symbols:
        raise ValueError("--symbols must include at least one symbol")

    with DuckDBStore(args.db_path) as store:
        results = []
        results.append(SecurityMasterDataset().run(store, SecurityMasterOptions()))
        if not args.skip_symbol_directory:
            results.append(NasdaqSymbolDirectoryDataset().run(store, NasdaqSymbolDirectoryOptions()))
        if not args.skip_listing_events:
            results.append(NasdaqListingEventsDataset().run(store, NasdaqListingEventsOptions()))
        if not args.skip_listing_status:
            results.append(ListingStatusIntervalDataset().run(store, ListingStatusIntervalOptions()))
            if not args.skip_delistings:
                if args.delisting_return_file:
                    results.append(
                        DelistingReturnObservationDataset().run(
                            store,
                            DelistingReturnObservationOptions(
                                source_file=args.delisting_return_file,
                                provider=args.delisting_return_provider,
                                vendor_security_id_type=args.delisting_return_vendor_id_type,
                            ),
                        )
                    )
                results.append(
                    DelistingEventDataset().run(
                        store,
                        DelistingEventOptions(
                            apply_shumway_warther_imputation=args.impute_delist_returns,
                        ),
                    )
                )
        results.append(
            TickerHistoryDataset().run(
                store,
                TickerHistoryOptions(
                    zip_path=args.ticker_history_zip,
                    symbols=symbols,
                    start_date=args.start_date,
                    end_date=args.end_date,
                    chunk_size=args.ticker_chunk_size,
                    max_chunks=args.ticker_max_chunks,
                ),
            )
        )
        if not args.skip_corporate_actions:
            results.append(CorporateActionsDataset().run(store, CorporateActionsOptions()))
            if not args.skip_adjustment_factors:
                results.append(
                    AdjustmentFactorHistoryDataset().run(
                        store,
                        AdjustmentFactorHistoryOptions(),
                    )
                )
                if not args.skip_daily_adjustments:
                    results.append(
                        DailyAdjustmentFactorDataset().run(
                            store,
                            DailyAdjustmentFactorOptions(),
                        )
                    )
        results.append(
            FinraShortInterestDataset().run(
                store,
                FinraShortInterestOptions(symbol=symbols[0]),
            )
        )
        if not args.skip_13f:
            results.append(
                ThirteenFDataSet().run(
                    store,
                    ThirteenFOptions(cusips=None if args.full_13f_holdings else ThirteenFOptions().cusips),
                )
            )
        if not args.skip_identifier_resolution:
            results.append(
                IdentifierResolutionCandidateDataset().run(
                    store,
                    IdentifierResolutionOptions(
                        min_confidence=args.identifier_resolution_min_confidence,
                    ),
                )
            )
        if not args.skip_identifier_decisions:
            accept_statuses = ("already_mapped", "proposed") if args.identifier_decision_accept_proposed else ("already_mapped",)
            results.append(
                IdentifierResolutionDecisionDataset().run(
                    store,
                    IdentifierResolutionDecisionOptions(
                        accept_candidate_statuses=accept_statuses,
                    ),
                )
            )
        if not args.skip_fundamentals:
            results.append(
                SecCompanyFactsDataset().run(
                    store,
                    SecCompanyFactsOptions(symbols=symbols),
                )
            )
            if not args.skip_shares_outstanding:
                results.append(
                    SharesOutstandingHistoryDataset().run(
                        store,
                        SharesOutstandingHistoryOptions(),
                    )
                )
            if not args.skip_fundamental_features:
                results.append(
                    FundamentalFeatureDataset().run(
                        store,
                        FundamentalFeatureBuildOptions(symbols=symbols),
                    )
                )
            if not args.skip_estimates:
                results.append(EstimateMeasureSeedDataset().run(store, EstimateMeasureSeedOptions()))
                results.append(
                    EstimateActualsDataset().run(
                        store,
                        EstimateActualsOptions(security_ids=None),
                    )
                )
                if args.estimate_consensus_file:
                    results.append(
                        EstimateConsensusDataset().run(
                            store,
                            EstimateConsensusOptions(
                                source_file=args.estimate_consensus_file,
                                provider_name=args.estimate_consensus_provider,
                                vendor_security_id_type=args.estimate_consensus_vendor_id_type,
                                stale_after_days=args.estimate_consensus_stale_days,
                            ),
                        )
                    )
                results.append(EstimateSurpriseDataset().run(store, EstimateSurpriseOptions()))
                if args.estimate_detail_file:
                    results.append(
                        EstimateDetailDataset().run(
                            store,
                            EstimateDetailOptions(
                                source_file=args.estimate_detail_file,
                                provider=args.estimate_detail_provider,
                                vendor_security_id_type=args.estimate_detail_vendor_id_type,
                            ),
                        )
                    )
                if args.estimate_guidance_file:
                    results.append(
                        EstimateGuidanceDataset().run(
                            store,
                            EstimateGuidanceOptions(
                                source_file=args.estimate_guidance_file,
                                source=args.estimate_guidance_source,
                                min_confidence=args.estimate_guidance_min_confidence,
                            ),
                        )
                    )
                if args.estimate_recommendation_file:
                    results.append(
                        EstimateRecommendationDataset().run(
                            store,
                            EstimateRecommendationOptions(
                                source_file=args.estimate_recommendation_file,
                                provider_name=args.estimate_recommendation_provider,
                                vendor_security_id_type=args.estimate_recommendation_vendor_id_type,
                                source_vendor_table=args.estimate_recommendation_source_table,
                            ),
                        )
                    )
                if args.estimate_recommendation_summary_file:
                    results.append(
                        EstimateRecommendationSummaryDataset().run(
                            store,
                            EstimateRecommendationSummaryOptions(
                                source_file=args.estimate_recommendation_summary_file,
                                provider_name=args.estimate_recommendation_summary_provider,
                                vendor_security_id_type=args.estimate_recommendation_summary_vendor_id_type,
                                source_vendor_table=args.estimate_recommendation_summary_source_table,
                                rating_scale=args.estimate_recommendation_summary_rating_scale,
                                scale_direction=args.estimate_recommendation_summary_scale_direction,
                            ),
                        )
                    )
                if not args.skip_estimate_security_links:
                    results.append(
                        EstimateSecurityLinkDataset().run(
                            store,
                            EstimateSecurityLinkOptions(
                                min_confidence=args.estimate_security_link_min_confidence,
                                apply_to_security_identifier_history=(
                                    not args.skip_estimate_security_link_identifier_history
                                ),
                            ),
                        )
                    )
        if not args.skip_submissions:
            results.append(
                SecSubmissionsDataset().run(
                    store,
                    SecSubmissionsOptions(symbols=symbols),
                )
            )
        if not args.skip_macro:
            macro_series = tuple(part.strip().upper() for part in args.macro_series.split(",") if part.strip())
            results.append(
                FredMacroDataset().run(
                    store,
                    FredMacroOptions(series_ids=macro_series),
                )
            )
        results.append(TradingCalendarDataset().run(store, TradingCalendarOptions()))
        if not args.skip_universe:
            results.append(
                UniverseMembershipDataset().run(
                    store,
                    UniverseBuildOptions(
                        universe_id=args.universe_id,
                        symbols=symbols,
                        start_date=args.start_date,
                        end_date=args.end_date,
                        lookback_days=args.universe_lookback_days,
                        min_history_days=args.universe_min_history_days,
                        min_price=args.universe_min_price,
                        min_dollar_volume=args.universe_min_dollar_volume,
                    ),
                )
            )
        results.append(
            EquityDailyFeatureDataset().run(
                store,
                FeatureBuildOptions(symbols=symbols),
            )
        )

    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "results": [
                    {
                        "dataset_id": result.dataset_id,
                        "rows_loaded": result.rows_loaded,
                        "source": result.source,
                        "run_id": result.run_id,
                        "details": result.details,
                    }
                    for result in results
                ],
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
