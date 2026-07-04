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
    est_recommendation_summary_asof,
    est_security_links_asof,
    est_surprise_asof,
    features_asof,
    filer_aliases_asof,
    form144_intents_asof,
    form144_reconciliation_asof,
    fundamental_periods_asof,
    fundamental_ratios_asof,
    fundamental_ratios_asof_month,
    fundamental_xbrl_metric_asof,
    pit_snapshot_asof,
    short_interest_metrics_asof,
    macro_metrics_asof,
    equity_price_metrics_asof,
    thirteenf_concentration_metrics_asof,
    thirteenf_position_metrics_asof,
    thirteenf_option_metrics_asof,
    corporate_action_dividend_metrics_asof,
    corporate_action_factor_reconciliation_asof,
    corporate_action_split_metrics_asof,
    finra_short_volume_asof,
    offexchange_quality_report_asof,
    offexchange_security_period_asof,
    offexchange_volume_asof,
    short_volume_metrics_asof,
    fundamental_statements_asof,
    fundamental_ttm_asof,
    fundamentals_asof,
    identifier_decisions_asof,
    insider_transaction_metrics_asof,
    macro_asof,
    security_listing_metrics_asof,
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
    parser.add_argument("--as-of-month", type=dt.date.fromisoformat, help="Optional month boundary for month-snapshot views.")
    parser.add_argument("--as-of-ts", type=dt.datetime.fromisoformat, help="Optional availability timestamp.")
    parser.add_argument(
        "--view",
        choices=[
            "securities",
            "fundamentals",
            "fundamental-statements",
            "fundamental-ttm",
            "fundamental-ratios",
            "fundamental-ratios-asof-month",
            "pit-snapshot",
            "fundamental-xbrl-metric",
            "short-interest-metrics",
            "macro-metrics",
            "equity-price-metrics",
            "thirteenf-concentration-metrics",
            "thirteenf-position-metrics",
            "thirteenf-option-metrics",
            "corporate-action-dividend-metrics",
            "corporate-action-factor-reconciliation",
            "corporate-action-split-metrics",
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
            "estimate-recommendation-summary",
            "estimate-security-links",
            "features",
            "short-interest",
            "macro",
            "universe",
            "corporate-actions",
            "13f-positioning",
            "filer-aliases",
            "insider-transaction-metrics",
            "security-listing-metrics",
            "form144-intents",
            "form144-reconciliation",
            "finra-short-volume",
            "offexchange-volume",
            "offexchange-security-period",
            "offexchange-quality-report",
            "short-volume-metrics",
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
    parser.add_argument("--recommendation-event-types", help="Comma-separated recommendation event-type filter.")
    parser.add_argument("--recommendation-summary-source-tables", help="Comma-separated recommendation summary source-table filter.")
    parser.add_argument("--estimate-vendor-id-types", help="Comma-separated estimate vendor security-id types.")
    parser.add_argument("--estimate-link-statuses", default="accepted", help="Comma-separated estimate security-link statuses.")
    parser.add_argument(
        "--include-stale-estimates",
        action="store_true",
        help="Include stale consensus estimate snapshots in estimate-consensus.",
    )
    parser.add_argument(
        "--estimate-stale-days",
        type=int,
        default=105,
        help="Consensus stale window in days for estimate-consensus (default: 105).",
    )
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
    parser.add_argument("--alias-cik", help="Filter filer-aliases to a single filing-manager CIK.")
    parser.add_argument("--alias-types", help="Comma-separated filer-alias type filter (SELF, NAME_HISTORY, ...).")
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.0,
        help="Minimum filer-alias confidence to include (default 0.0 = all, including candidates).",
    )
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
    elif args.view == "fundamental-ratios":
        frame = fundamental_ratios_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            ratio_codes=parse_csv(args.metrics),
        )
    elif args.view == "fundamental-ratios-asof-month":
        frame = fundamental_ratios_asof_month(
            args.as_of_month or args.as_of_date,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            ratio_codes=parse_csv(args.metrics),
        )
    elif args.view == "pit-snapshot":
        frame = pit_snapshot_asof(
            args.as_of_month or args.as_of_date,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            metrics=parse_csv(args.metrics),
        )
    elif args.view == "fundamental-xbrl-metric":
        frame = fundamental_xbrl_metric_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            canonical_metrics=parse_csv(args.metrics),
        )
    elif args.view == "short-interest-metrics":
        frame = short_interest_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "macro-metrics":
        frame = macro_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            series_ids=parse_csv(args.metrics) or parse_csv(args.symbols),
        )
    elif args.view == "equity-price-metrics":
        frame = equity_price_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "thirteenf-concentration-metrics":
        frame = thirteenf_concentration_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "thirteenf-position-metrics":
        frame = thirteenf_position_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "thirteenf-option-metrics":
        frame = thirteenf_option_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "corporate-action-dividend-metrics":
        frame = corporate_action_dividend_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "corporate-action-split-metrics":
        frame = corporate_action_split_metrics_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "corporate-action-factor-reconciliation":
        frame = corporate_action_factor_reconciliation_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
            event_types=parse_csv(args.event_types),
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
            symbols=parse_csv(args.symbols),
            measure_codes=parse_csv(args.measure_codes),
            providers=parse_csv(args.providers),
            include_stale=args.include_stale_estimates,
            stale_after_days=args.estimate_stale_days,
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
            symbols=parse_csv(args.symbols),
            providers=parse_csv(args.providers),
            event_types=parse_csv(args.recommendation_event_types),
            broker_ids=parse_id_csv(args.brokers),
            analyst_ids=parse_id_csv(args.analysts),
        )
    elif args.view == "estimate-recommendation-summary":
        frame = est_recommendation_summary_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            symbols=parse_csv(args.symbols),
            providers=parse_csv(args.providers),
            source_vendor_tables=parse_csv(args.recommendation_summary_source_tables),
        )
    elif args.view == "estimate-security-links":
        frame = est_security_links_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            providers=parse_csv(args.providers),
            vendor_security_id_types=parse_csv(args.estimate_vendor_id_types),
            link_statuses=parse_csv(args.estimate_link_statuses),
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
    elif args.view == "filer-aliases":
        frame = filer_aliases_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            alias_cik=args.alias_cik,
            alias_types=parse_csv(args.alias_types),
            min_confidence=args.min_confidence,
        )
    elif args.view == "insider-transaction-metrics":
        frame = insider_transaction_metrics_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "security-listing-metrics":
        frame = security_listing_metrics_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "form144-intents":
        frame = form144_intents_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "form144-reconciliation":
        frame = form144_reconciliation_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            security_ids=parse_id_csv(args.security_ids),
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "finra-short-volume":
        frame = finra_short_volume_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "offexchange-volume":
        frame = offexchange_volume_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "offexchange-security-period":
        frame = offexchange_security_period_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
        )
    elif args.view == "offexchange-quality-report":
        frame = offexchange_quality_report_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            surfaces=parse_id_csv(args.metrics),
        )
    elif args.view == "short-volume-metrics":
        frame = short_volume_metrics_asof(
            as_of_date=args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            symbols=parse_csv(args.symbols),
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
