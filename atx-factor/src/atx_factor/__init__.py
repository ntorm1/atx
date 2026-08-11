"""Polars-native ATX factor research and mega-alpha admission."""

from .backtest import BacktestResult, run_backtest, run_weight_backtest
from .config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from .mega_alpha import AcceptanceGate, CandidateDecision, evaluate_candidate
from .metrics import PerformanceMetrics, compute_performance_metrics
from .portfolio import build_target_weights, normalize_weight_scores
from .schema import PanelValidationError, validate_panel
from .walk_forward import WalkForwardResult, walk_forward_backtest

__all__ = [
    "AcceptanceGate",
    "BacktestConfig",
    "BacktestResult",
    "CandidateDecision",
    "CostModel",
    "PanelValidationError",
    "PerformanceMetrics",
    "PortfolioConfig",
    "WalkForwardConfig",
    "WalkForwardResult",
    "build_target_weights",
    "compute_performance_metrics",
    "evaluate_candidate",
    "normalize_weight_scores",
    "run_backtest",
    "run_weight_backtest",
    "validate_panel",
    "walk_forward_backtest",
]
