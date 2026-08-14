"""Polars-native ATX factor research and mega-alpha admission."""

from .backtest import BacktestResult, run_backtest, run_weight_backtest
from .capacity import CapacityFrontier, CapacityPoint, evaluate_capacity_frontier
from .config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from .exploration import (
    DEFAULT_VARIANTS,
    ExplorationConfig,
    ExplorationDecision,
    ExplorationGate,
    PortfolioVariant,
    explore_candidate,
)
from .mega_alpha import AcceptanceGate, CandidateDecision, evaluate_candidate
from .metrics import PerformanceMetrics, compute_performance_metrics
from .portfolio import build_target_weights, normalize_weight_scores
from .replacement import ReplacementDecision, ReplacementGate, evaluate_replacement
from .schema import PanelValidationError, validate_panel
from .walk_forward import WalkForwardResult, walk_forward_backtest

__all__ = [
    "DEFAULT_VARIANTS",
    "AcceptanceGate",
    "BacktestConfig",
    "BacktestResult",
    "CandidateDecision",
    "CapacityFrontier",
    "CapacityPoint",
    "CostModel",
    "ExplorationConfig",
    "ExplorationDecision",
    "ExplorationGate",
    "PanelValidationError",
    "PerformanceMetrics",
    "PortfolioConfig",
    "PortfolioVariant",
    "ReplacementDecision",
    "ReplacementGate",
    "WalkForwardConfig",
    "WalkForwardResult",
    "build_target_weights",
    "compute_performance_metrics",
    "evaluate_candidate",
    "evaluate_capacity_frontier",
    "evaluate_replacement",
    "explore_candidate",
    "normalize_weight_scores",
    "run_backtest",
    "run_weight_backtest",
    "validate_panel",
    "walk_forward_backtest",
]
