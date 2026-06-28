#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.calendar import TradingCalendarDataset, TradingCalendarOptions
from db.corporate_actions import CorporateActionsDataset, CorporateActionsOptions
from db.features import (
    EquityDailyFeatureDataset,
    FeatureBuildOptions,
    FundamentalFeatureBuildOptions,
    FundamentalFeatureDataset,
)
from db.finra import FinraShortInterestDataset, FinraShortInterestOptions, parse_date
from db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions
from db.identifier_decisions import IdentifierResolutionDecisionDataset, IdentifierResolutionDecisionOptions
from db.identifier_resolution import IdentifierResolutionCandidateDataset, IdentifierResolutionOptions
from db.macro import FredMacroDataset, FredMacroOptions
from db.sec_submissions import SecSubmissionsDataset, SecSubmissionsOptions
from db.security_master import SecurityMasterDataset, SecurityMasterOptions
from db.symbol_directory import NasdaqSymbolDirectoryDataset, NasdaqSymbolDirectoryOptions
from db.thirteenf import ThirteenFDataSet, ThirteenFOptions
from db.ticker_history import DEFAULT_TICKER_HISTORY_ZIP, TickerHistoryDataset, TickerHistoryOptions
from db.universes import UniverseBuildOptions, UniverseMembershipDataset
from db.warehouse import symbol_key


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a Python/DuckDB public-data quant equity warehouse for atx-impl."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--symbols", default="AAPL", help="Comma-separated symbol list for focused loads.")
    parser.add_argument("--start-date", type=parse_date)
    parser.add_argument("--end-date", type=parse_date)
    parser.add_argument("--ticker-chunk-size", type=int, default=200_000)
    parser.add_argument("--ticker-max-chunks", type=int, help="Smoke-test cap; omit for full ZIP scan.")
    parser.add_argument("--skip-fundamentals", action="store_true")
    parser.add_argument("--skip-fundamental-features", action="store_true")
    parser.add_argument("--skip-submissions", action="store_true")
    parser.add_argument("--skip-13f", action="store_true")
    parser.add_argument("--skip-identifier-resolution", action="store_true")
    parser.add_argument("--skip-identifier-decisions", action="store_true")
    parser.add_argument("--skip-corporate-actions", action="store_true")
    parser.add_argument("--skip-symbol-directory", action="store_true")
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
            if not args.skip_fundamental_features:
                results.append(
                    FundamentalFeatureDataset().run(
                        store,
                        FundamentalFeatureBuildOptions(symbols=symbols),
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
