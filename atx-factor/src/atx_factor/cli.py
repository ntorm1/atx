"""Operator CLI for governed candidate evaluation."""

from __future__ import annotations

import argparse
from pathlib import Path

from .atx_db import load_factor_panel, select_signal
from .config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from .mega_alpha import AcceptanceGate, evaluate_candidate
from .registry import MegaAlphaRegistry, atomic_write_text

DEFAULT_BASELINE_ID = "composite_operating_profitability_or_net_issuance"


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="atx-factor",
        description="Polars-native ATX factor backtesting and mega-alpha admission",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    evaluate = subparsers.add_parser(
        "evaluate-candidate",
        help="run costed walk-forward evaluation against the production mega-alpha",
    )
    evaluate.add_argument("--db-path", type=Path, required=True)
    evaluate.add_argument("--candidate", required=True)
    evaluate.add_argument("--baseline", default=DEFAULT_BASELINE_ID)
    evaluate.add_argument("--horizon-days", type=int, default=21)
    evaluate.add_argument("--aum-usd", type=float, default=100_000_000.0)
    evaluate.add_argument("--commission-bps", type=float, default=0.25)
    evaluate.add_argument("--half-spread-bps", type=float, default=2.0)
    evaluate.add_argument("--impact-bps", type=float, default=10.0)
    evaluate.add_argument("--borrow-bps", type=float, default=50.0)
    evaluate.add_argument("--max-participation", type=float, default=0.10)
    evaluate.add_argument("--gross", type=float, default=1.0)
    evaluate.add_argument("--name-cap", type=float, default=0.05)
    evaluate.add_argument("--minimum-names", type=int, default=20)
    evaluate.add_argument("--minimum-train-periods", type=int, default=60)
    evaluate.add_argument("--test-periods", type=int, default=12)
    evaluate.add_argument("--embargo-periods", type=int, default=1)
    evaluate.add_argument("--minimum-folds", type=int, default=3)
    evaluate.add_argument("--trial-count", type=int, default=32)
    evaluate.add_argument("--candidate-allocation", type=float, default=0.20)
    evaluate.add_argument("--output", type=Path)
    evaluate.add_argument("--registry", type=Path)
    return parser


def _evaluate(args: argparse.Namespace) -> int:
    panel = load_factor_panel(
        args.db_path,
        [args.candidate, args.baseline],
        horizon_days=args.horizon_days,
    )
    candidate = select_signal(panel, args.candidate)
    baseline = select_signal(panel, args.baseline)
    portfolio = PortfolioConfig(
        gross_leverage=args.gross,
        name_cap=args.name_cap,
        minimum_names=args.minimum_names,
    )
    costs = CostModel(
        commission_bps=args.commission_bps,
        half_spread_bps=args.half_spread_bps,
        impact_bps=args.impact_bps,
        aum_usd=args.aum_usd,
        annual_borrow_bps=args.borrow_bps,
        max_participation=args.max_participation,
    )
    backtest = BacktestConfig(
        portfolio=portfolio,
        costs=costs,
        holding_period_days=args.horizon_days,
        trial_count=args.trial_count,
    )
    walk_forward = WalkForwardConfig(
        minimum_train_periods=args.minimum_train_periods,
        test_periods=args.test_periods,
        step_periods=args.test_periods,
        embargo_periods=args.embargo_periods,
        minimum_folds=args.minimum_folds,
    )
    gate = AcceptanceGate(candidate_allocation=args.candidate_allocation)
    decision = evaluate_candidate(
        candidate,
        baseline,
        candidate_id=args.candidate,
        baseline_id=args.baseline,
        backtest_config=backtest,
        walk_forward_config=walk_forward,
        gate=gate,
    )
    serialized = decision.to_json() + "\n"
    if args.output:
        atomic_write_text(args.output, serialized)
    if args.registry and decision.accepted:
        MegaAlphaRegistry(args.registry).admit(
            decision,
            allocation=args.candidate_allocation,
        )
    print(serialized, end="")
    return 0 if decision.accepted else 2


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.command == "evaluate-candidate":
        return _evaluate(args)
    parser.error(f"unsupported command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
