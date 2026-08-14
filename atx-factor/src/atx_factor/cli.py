"""Operator CLI for governed candidate evaluation."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

from .atx_db import load_factor_panel, select_signal
from .capacity import evaluate_capacity_frontier
from .config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from .exploration import (
    DEFAULT_COMBINATION_MODES,
    DEFAULT_VARIANTS,
    INTEGRATED_SCORE_MODEL,
    SELECTION_SUPPORT_MODEL,
    SLEEVE_CAPACITY_MODEL,
    ExplorationConfig,
    ExplorationGate,
    explore_candidate,
)
from .mega_alpha import AcceptanceGate, evaluate_candidate
from .registry import MegaAlphaRegistry, ShadowAlphaRegistry, atomic_write_text
from .replacement import ReplacementGate, evaluate_replacement
from .trial_ledger import ResearchTrialLedger

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
    evaluate.add_argument(
        "--summary-only",
        action="store_true",
        help="print a compact decision while retaining the complete output artifact",
    )
    explore = subparsers.add_parser(
        "explore-candidate",
        help="select a bounded portfolio construction, then evaluate one untouched holdout",
    )
    explore.add_argument("--db-path", type=Path, required=True)
    explore.add_argument("--candidate", required=True)
    explore.add_argument("--baseline", default=DEFAULT_BASELINE_ID)
    explore.add_argument(
        "--horizon-days",
        type=int,
        choices=(21,),
        default=21,
        help=(
            "Forward-return horizon; construction exploration currently requires "
            "non-overlapping monthly 21-trading-day returns."
        ),
    )
    explore.add_argument("--aum-usd", type=float, default=50_000_000.0)
    explore.add_argument("--commission-bps", type=float, default=0.25)
    explore.add_argument("--half-spread-bps", type=float, default=2.0)
    explore.add_argument("--impact-bps", type=float, default=10.0)
    explore.add_argument("--borrow-bps", type=float, default=50.0)
    explore.add_argument("--max-participation", type=float, default=0.10)
    explore.add_argument("--gross", type=float, default=1.0)
    explore.add_argument("--name-cap", type=float, default=0.05)
    explore.add_argument("--minimum-names", type=int, default=20)
    explore.add_argument("--selection-fraction", type=float, default=0.60)
    explore.add_argument("--embargo-periods", type=int, default=1)
    explore.add_argument("--minimum-selection-periods", type=int, default=60)
    explore.add_argument("--minimum-validation-periods", type=int, default=48)
    explore.add_argument("--fold-periods", type=int, default=12)
    explore.add_argument("--historical-trial-count", type=int, default=66)
    explore.add_argument(
        "--trial-ledger", type=Path, default=Path("research/trial-ledger.json")
    )
    explore.add_argument("--output", type=Path)
    explore.add_argument("--registry", type=Path)
    explore.add_argument(
        "--shadow-registry",
        type=Path,
        default=Path("research/shadow-alpha-registry.json"),
    )
    explore.add_argument("--summary-only", action="store_true")
    replacement = subparsers.add_parser(
        "evaluate-replacement",
        help="challenge the production incumbent with an independently tested factor",
    )
    replacement.add_argument("--db-path", type=Path, required=True)
    replacement.add_argument("--challenger", required=True)
    replacement.add_argument("--incumbent", default=DEFAULT_BASELINE_ID)
    replacement.add_argument("--horizon-days", type=int, default=21)
    replacement.add_argument("--aum-usd", type=float, default=50_000_000.0)
    replacement.add_argument("--commission-bps", type=float, default=0.25)
    replacement.add_argument("--half-spread-bps", type=float, default=2.0)
    replacement.add_argument("--impact-bps", type=float, default=10.0)
    replacement.add_argument("--borrow-bps", type=float, default=50.0)
    replacement.add_argument("--max-participation", type=float, default=0.10)
    replacement.add_argument("--gross", type=float, default=1.0)
    replacement.add_argument("--name-cap", type=float, default=0.05)
    replacement.add_argument("--minimum-names", type=int, default=20)
    replacement.add_argument("--minimum-train-periods", type=int, default=60)
    replacement.add_argument("--test-periods", type=int, default=12)
    replacement.add_argument("--embargo-periods", type=int, default=1)
    replacement.add_argument("--minimum-folds", type=int, default=3)
    replacement.add_argument("--trial-count", type=int, default=32)
    replacement.add_argument("--output", type=Path)
    replacement.add_argument(
        "--summary-only",
        action="store_true",
        help="print a compact decision while retaining the complete output artifact",
    )
    capacity = subparsers.add_parser(
        "capacity-frontier",
        help="evaluate a factor's execution feasibility over a fixed AUM grid",
    )
    capacity.add_argument("--db-path", type=Path, required=True)
    capacity.add_argument("--factor", default=DEFAULT_BASELINE_ID)
    capacity.add_argument("--horizon-days", type=int, default=21)
    capacity.add_argument("--aum-grid", type=float, nargs="+", required=True)
    capacity.add_argument("--commission-bps", type=float, default=0.25)
    capacity.add_argument("--half-spread-bps", type=float, default=2.0)
    capacity.add_argument("--impact-bps", type=float, default=10.0)
    capacity.add_argument("--borrow-bps", type=float, default=50.0)
    capacity.add_argument("--max-participation", type=float, default=0.10)
    capacity.add_argument("--minimum-gross-deployment", type=float, default=0.95)
    capacity.add_argument("--gross", type=float, default=1.0)
    capacity.add_argument("--name-cap", type=float, default=0.05)
    capacity.add_argument("--minimum-names", type=int, default=20)
    capacity.add_argument("--output", type=Path)
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
    if args.summary_only:
        print(
            json.dumps(
                {
                    "accepted": decision.accepted,
                    "baseline_id": decision.baseline_id,
                    "candidate_id": decision.candidate_id,
                    "candidate_sharpe": decision.candidate_metrics["sharpe"],
                    "combined_sharpe": decision.combined_metrics["sharpe"],
                    "evidence_sha256": decision.evidence_sha256,
                    "marginal_sharpe": decision.marginal_sharpe,
                    "rejection_reasons": decision.rejection_reasons,
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print(serialized, end="")
    return 0 if decision.accepted else 2


def _capacity_frontier(args: argparse.Namespace) -> int:
    panel = select_signal(
        load_factor_panel(
            args.db_path,
            [args.factor],
            horizon_days=args.horizon_days,
        ),
        args.factor,
    )
    frontier = evaluate_capacity_frontier(
        panel,
        args.aum_grid,
        portfolio=PortfolioConfig(
            gross_leverage=args.gross,
            name_cap=args.name_cap,
            minimum_names=args.minimum_names,
        ),
        costs=CostModel(
            commission_bps=args.commission_bps,
            half_spread_bps=args.half_spread_bps,
            impact_bps=args.impact_bps,
            annual_borrow_bps=args.borrow_bps,
            max_participation=args.max_participation,
        ),
        holding_period_days=args.horizon_days,
        maximum_participation=args.max_participation,
        minimum_gross_deployment=args.minimum_gross_deployment,
    )
    serialized = frontier.to_json() + "\n"
    if args.output:
        atomic_write_text(args.output, serialized)
    print(serialized, end="")
    return 0 if frontier.maximum_feasible_aum_usd is not None else 2


def _explore(args: argparse.Namespace) -> int:
    grid_trials = len(DEFAULT_VARIANTS) * 3 * len(DEFAULT_COMBINATION_MODES)
    gate = ExplorationGate()
    spec = {
        "candidate_id": args.candidate,
        "baseline_id": args.baseline,
        "horizon_days": args.horizon_days,
        "variants": [asdict(variant) for variant in DEFAULT_VARIANTS],
        "allocations": [0.10, 0.20, 0.30],
        "combination_modes": list(DEFAULT_COMBINATION_MODES),
        "selection_fraction": args.selection_fraction,
        "embargo_periods": args.embargo_periods,
        "sleeve_capacity_model": SLEEVE_CAPACITY_MODEL,
        "integrated_score_model": INTEGRATED_SCORE_MODEL,
        "selection_support_model": SELECTION_SUPPORT_MODEL,
        "acceptance_gate": asdict(gate),
    }
    _, total_trials = ResearchTrialLedger(args.trial_ledger).commit(
        spec,
        configuration_trials=grid_trials,
        historical_trial_count=args.historical_trial_count,
    )
    panel = load_factor_panel(
        args.db_path,
        [args.candidate, args.baseline],
        horizon_days=args.horizon_days,
    )
    backtest = BacktestConfig(
        portfolio=PortfolioConfig(
            gross_leverage=args.gross,
            name_cap=args.name_cap,
            minimum_names=args.minimum_names,
        ),
        costs=CostModel(
            commission_bps=args.commission_bps,
            half_spread_bps=args.half_spread_bps,
            impact_bps=args.impact_bps,
            aum_usd=args.aum_usd,
            annual_borrow_bps=args.borrow_bps,
            max_participation=args.max_participation,
        ),
        holding_period_days=args.horizon_days,
        trial_count=total_trials,
    )
    exploration = ExplorationConfig(
        selection_fraction=args.selection_fraction,
        embargo_periods=args.embargo_periods,
        minimum_selection_periods=args.minimum_selection_periods,
        minimum_validation_periods=args.minimum_validation_periods,
        fold_periods=args.fold_periods,
        prior_trial_count=total_trials - grid_trials,
    )
    decision = explore_candidate(
        select_signal(panel, args.candidate),
        select_signal(panel, args.baseline),
        candidate_id=args.candidate,
        baseline_id=args.baseline,
        backtest_config=backtest,
        exploration_config=exploration,
        gate=gate,
    )
    serialized = decision.to_json() + "\n"
    if args.output:
        atomic_write_text(args.output, serialized)
    if args.registry and decision.accepted:
        MegaAlphaRegistry(args.registry).admit(
            decision,
            allocation=decision.selected_allocation,
        )
    elif decision.shadow_eligible:
        ShadowAlphaRegistry(args.shadow_registry).admit(decision)
    if args.summary_only:
        print(
            json.dumps(
                {
                    "accepted": decision.accepted,
                    "disposition": decision.disposition,
                    "shadow_eligible": decision.shadow_eligible,
                    "candidate_id": decision.candidate_id,
                    "selected_variant": decision.selected_variant,
                    "selected_allocation": decision.selected_allocation,
                    "selected_combination_mode": decision.selected_combination_mode,
                    "selection_feasible_trial_count": (
                        decision.selection_feasible_trial_count
                    ),
                    "validation_median_signal_breadth": (
                        decision.validation_candidate_breadth["median_names"]
                    ),
                    "validation_median_effective_breadth": (
                        decision.validation_candidate_portfolio_breadth[
                            "median_effective_breadth"
                        ]
                    ),
                    "committed_trials": decision.committed_trials,
                    "validation_candidate_sharpe": decision.validation_candidate_metrics[
                        "sharpe"
                    ],
                    "validation_combined_sharpe": decision.validation_combined_metrics[
                        "sharpe"
                    ],
                    "validation_marginal_sharpe": decision.validation_marginal_sharpe,
                    "positive_fold_fraction": decision.positive_fold_fraction,
                    "rejection_reasons": decision.rejection_reasons,
                    "evidence_sha256": decision.evidence_sha256,
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print(serialized, end="")
    return 0 if decision.accepted else 2


def _evaluate_replacement(args: argparse.Namespace) -> int:
    panel = load_factor_panel(
        args.db_path,
        [args.challenger, args.incumbent],
        horizon_days=args.horizon_days,
    )
    backtest = BacktestConfig(
        portfolio=PortfolioConfig(
            gross_leverage=args.gross,
            name_cap=args.name_cap,
            minimum_names=args.minimum_names,
        ),
        costs=CostModel(
            commission_bps=args.commission_bps,
            half_spread_bps=args.half_spread_bps,
            impact_bps=args.impact_bps,
            aum_usd=args.aum_usd,
            annual_borrow_bps=args.borrow_bps,
            max_participation=args.max_participation,
        ),
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
    decision = evaluate_replacement(
        select_signal(panel, args.challenger),
        select_signal(panel, args.incumbent),
        challenger_id=args.challenger,
        incumbent_id=args.incumbent,
        backtest_config=backtest,
        walk_forward_config=walk_forward,
        gate=ReplacementGate(),
    )
    serialized = decision.to_json() + "\n"
    if args.output:
        atomic_write_text(args.output, serialized)
    if args.summary_only:
        print(
            json.dumps(
                {
                    "accepted": decision.accepted,
                    "challenger_id": decision.challenger_id,
                    "challenger_sharpe": decision.challenger_metrics["sharpe"],
                    "evidence_sha256": decision.evidence_sha256,
                    "incumbent_id": decision.incumbent_id,
                    "incumbent_sharpe": decision.incumbent_metrics["sharpe"],
                    "rejection_reasons": decision.rejection_reasons,
                    "sharpe_improvement": decision.sharpe_improvement,
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print(serialized, end="")
    return 0 if decision.accepted else 2


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.command == "evaluate-candidate":
        return _evaluate(args)
    if args.command == "explore-candidate":
        return _explore(args)
    if args.command == "evaluate-replacement":
        return _evaluate_replacement(args)
    if args.command == "capacity-frontier":
        return _capacity_frontier(args)
    parser.error(f"unsupported command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
