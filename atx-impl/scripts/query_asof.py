#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH
from db.asof import (
    adjustment_factors_asof,
    corporate_actions_asof,
    daily_adjustment_factors_asof,
    daily_panel_asof,
    delisting_events_asof,
    delisting_return_observations_asof,
    est_actual_asof,
    est_consensus_asof,
    est_detail_asof,
    est_guidance_asof,
    est_recommendation_asof,
    est_surprise_asof,
    features_asof,
    fundamental_periods_asof,
    fundamental_statements_asof,
    fundamental_ttm_asof,
    fundamentals_asof,
    identifier_decisions_asof,
    macro_asof,
    security_master_asof,
    shares_outstanding_asof,
    short_interest_asof,
    thirteenf_positioning_asof,
    universe_asof,
)


def parse_csv(value: str | None) -> tuple[str, ...] | None:
    if not value:
        return None
    parsed = tuple(part.strip().upper() for part in value.split(",") if part.strip())
    return parsed or None


def parse_id_csv(value: str | None) -> tuple[str, ...] | None:
    if not value:
        return None
    parsed = tuple(part.strip() for part in value.split(",") if part.strip())
    return parsed or None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run common point-in-time warehouse queries.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--as-of-date", type=dt.date.fromisoformat, required=True)
    parser.add_argument("--as-of-ts", type=dt.datetime.fromisoformat, help="Optional availability timestamp.")
    parser.add_argument(
        "--view",
        choices=[
            "securities",
            "fundamentals",
            "fundamental-statements",
            "fundamental-ttm",
            "fundamental-periods",
            "shares-outstanding",
            "adjustment-factors",
            "daily-adjustments",
            "daily-panel",
            "delistings",
            "delisting-returns",
            "estimate-actuals",
            "estimate-surprises",
            "estimate-consensus",
            "estimate-guidance",
            "estimate-details",
            "estimate-recommendations",
            "features",
            "short-interest",
            "macro",
            "universe",
            "corporate-actions",
            "13f-positioning",
            "identifier-decisions",
        ],
        default="securities",
    )
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--symbols", help="Comma-separated ticker filter where supported.")
    parser.add_argument("--security-ids", help="Comma-separated warehouse security_id filter where supported.")
    parser.add_argument("--metrics", help="Comma-separated fundamental metric filter.")
    parser.add_argument("--measure-codes", help="Comma-separated estimate measure-code filter.")
    parser.add_argument("--brokers", help="Comma-separated estimate broker_id filter.")
    parser.add_argument("--analysts", help="Comma-separated estimate analyst_id filter.")
    parser.add_argument("--statement-types", help="Comma-separated statement type filter.")
    parser.add_argument("--period-types", help="Comma-separated normalized fundamental period type filter.")
    parser.add_argument("--share-count-types", help="Comma-separated share-count type filter.")
    parser.add_argument("--event-types", help="Comma-separated corporate-action adjustment event type filter.")
    parser.add_argument("--delist-codes", help="Comma-separated public delisting proxy code filter.")
    parser.add_argument("--providers", help="Comma-separated provider filter for observed return views.")
    parser.add_argument("--feature-set", default="equity_daily_v1")
    parser.add_argument("--features", help="Comma-separated feature-name filter.")
    parser.add_argument("--series-ids", help="Comma-separated macro/FRED series filter.")
    parser.add_argument("--universe-id", default="us_liquid_equity_v1")
    parser.add_argument("--cusips", help="Comma-separated CUSIP filter for 13F positioning.")
    parser.add_argument("--decision-statuses", help="Comma-separated identifier decision statuses.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.view == "securities":
        frame = security_master_asof(args.as_of_date, as_of_ts=args.as_of_ts, db_path=args.db_path)
    elif args.view == "fundamentals":
        frame = fundamentals_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            metrics=parse_csv(args.metrics),
        )
    elif args.view == "fundamental-statements":
        frame = fundamental_statements_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            metrics=parse_csv(args.metrics),
            statement_types=parse_csv(args.statement_types),
        )
    elif args.view == "fundamental-ttm":
        frame = fundamental_ttm_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            metrics=parse_csv(args.metrics),
            statement_types=parse_csv(args.statement_types),
        )
    elif args.view == "fundamental-periods":
        frame = fundamental_periods_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            normalized_period_types=parse_csv(args.period_types),
        )
    elif args.view == "shares-outstanding":
        frame = shares_outstanding_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            share_count_types=parse_csv(args.share_count_types),
        )
    elif args.view == "adjustment-factors":
        frame = adjustment_factors_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            event_types=parse_csv(args.event_types),
        )
    elif args.view == "daily-adjustments":
        frame = daily_adjustment_factors_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "daily-panel":
        frame = daily_panel_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "delistings":
        frame = delisting_events_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            delist_codes=parse_csv(args.delist_codes),
        )
    elif args.view == "delisting-returns":
        frame = delisting_return_observations_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            providers=parse_csv(args.providers),
        )
    elif args.view == "estimate-actuals":
        frame = est_actual_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            measure_codes=parse_csv(args.measure_codes),
        )
    elif args.view == "estimate-surprises":
        frame = est_surprise_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            measure_codes=parse_csv(args.measure_codes),
        )
    elif args.view == "estimate-consensus":
        frame = est_consensus_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            measure_codes=parse_csv(args.measure_codes),
        )
    elif args.view == "estimate-guidance":
        frame = est_guidance_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            measure_codes=parse_csv(args.measure_codes),
        )
    elif args.view == "estimate-details":
        frame = est_detail_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            symbols=parse_csv(args.symbols),
            measure_codes=parse_csv(args.measure_codes),
            providers=parse_csv(args.providers),
            broker_ids=parse_id_csv(args.brokers),
            analyst_ids=parse_id_csv(args.analysts),
        )
    elif args.view == "estimate-recommendations":
        frame = est_recommendation_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            broker_ids=parse_id_csv(args.brokers),
            analyst_ids=parse_id_csv(args.analysts),
        )
    elif args.view == "features":
        frame = features_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            feature_set=args.feature_set,
            symbols=parse_csv(args.symbols),
            feature_names=parse_csv(args.features),
        )
    elif args.view == "short-interest":
        frame = short_interest_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "macro":
        frame = macro_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            series_ids=parse_csv(args.series_ids),
        )
    elif args.view == "universe":
        frame = universe_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            universe_id=args.universe_id,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "corporate-actions":
        frame = corporate_actions_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "13f-positioning":
        frame = thirteenf_positioning_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            cusips=parse_csv(args.cusips),
        )
    else:
        frame = identifier_decisions_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            decision_statuses=parse_csv(args.decision_statuses),
        )
    print(frame.head(args.limit).to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
