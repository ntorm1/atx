#!/usr/bin/env python
"""Evaluate governed PIT factors with governed return targets and robust inference."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.signal_eval import (
    DEFAULT_SURVIVORSHIP_SAFE_SOURCE,
    DEFAULT_UNIVERSE_ID,
    IC_HORIZONS,
    SUPPORTED_RETURN_TARGETS,
    evaluate_panel,
)


def _date(value: str | None) -> dt.date | None:
    return None if value is None else dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Evaluate factors from v_factor_panel using governed adjusted-price or "
            "survivorship-safe forward returns, optional PIT industry neutralization, "
            "decile spreads, turnover, breadth, and HAC inference."
        )
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--factor-ids", nargs="+", required=True)
    parser.add_argument("--horizons", nargs="+", type=int, default=list(IC_HORIZONS))
    parser.add_argument("--n-quantiles", type=int, default=10)
    parser.add_argument("--universe-id", default=DEFAULT_UNIVERSE_ID)
    parser.add_argument("--start-date", type=_date, default=None)
    parser.add_argument("--end-date", type=_date, default=None)
    parser.add_argument(
        "--return-target",
        choices=SUPPORTED_RETURN_TARGETS,
        default="adjusted_prices",
        help="Scoring target: adjusted price returns or the governed delisting-stitched surface.",
    )
    parser.add_argument(
        "--survivorship-safe-source",
        default=DEFAULT_SURVIVORSHIP_SAFE_SOURCE,
        help="Source partition used when --return-target=survivorship_safe.",
    )
    parser.add_argument(
        "--neutralize-taxonomy",
        default=None,
        help="Optional PIT taxonomy code for within-industry ranks, e.g. FAMA_FRENCH_12.",
    )
    parser.add_argument("--neutralization-min-group-size", type=int, default=5)
    parser.add_argument("--neutralization-min-coverage", type=float, default=0.80)
    parser.add_argument(
        "--run-id",
        default=None,
        help="Stable research run identifier; defaults to a UTC timestamp.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run_id = args.run_id or dt.datetime.now(dt.UTC).strftime("signal-eval-%Y%m%dT%H%M%SZ")
    factor_ids = tuple(dict.fromkeys(args.factor_ids))
    horizons = tuple(dict.fromkeys(args.horizons))
    with DuckDBStore(args.db_path) as store:
        store.initialize()
        counts = evaluate_panel(
            store,
            n_quantiles=args.n_quantiles,
            horizons=horizons,
            universe_id=args.universe_id,
            start_date=args.start_date,
            end_date=args.end_date,
            factor_ids=factor_ids,
            return_target=args.return_target,
            survivorship_safe_source=args.survivorship_safe_source,
            neutralize_taxonomy=args.neutralize_taxonomy,
            neutralization_min_group_size=args.neutralization_min_group_size,
            neutralization_min_coverage=args.neutralization_min_coverage,
            run_id=run_id,
        )
        store.con.register(
            "signal_eval_cli_factors",
            pd.DataFrame({"factor_id": list(factor_ids)}),
        )
        try:
            ic = store.con.execute(
                """
                SELECT
                    i.factor_id,
                    i.horizon,
                    i.mean_rank_ic,
                    i.ic_tstat,
                    i.hac_lags,
                    i.hac_tstat,
                    i.sign_consistency,
                    i.n_dates,
                    i.mean_names
                FROM factor_ic i
                JOIN signal_eval_cli_factors f USING (factor_id)
                WHERE i.run_id = ?
                ORDER BY i.factor_id, i.horizon
                """,
                [run_id],
            ).df()
        finally:
            store.con.unregister("signal_eval_cli_factors")

    print(
        json.dumps(
            {
                "run_id": run_id,
                "universe_id": args.universe_id,
                "factor_ids": list(factor_ids),
                "horizons": list(horizons),
                "return_target": args.return_target,
                "neutralize_taxonomy": args.neutralize_taxonomy,
                "rows_persisted": counts,
                "information_coefficient": ic.to_dict(orient="records"),
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
