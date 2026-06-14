"""atxpy - Python wrapper for the atx-engine quant backtesting engine."""

from __future__ import annotations

from . import _core
from ._core import (
    AtxError,
    BacktestParams,
    BacktestResult as _CoreBacktestResult,
    BarsForSymbol,
    CommissionCfg,
    CommissionMode,
    Decimal,
    FillCfg,
    ImpactCfg,
    InstrumentStats,
    LatencyCfg,
    OrderType,
    SlippageCfg,
    SlippageMode,
    Symbol,
    SymbolTable,
    Transform,
    VolumeCapCfg,
    WeightPolicy,
)
from ._core import Library, Program  # alpha DSL handles
from ._core import FactorModel, OptimizerConfig, PortfolioOptimizer  # risk
from .alpha import compile_alpha, evaluate_alpha
from .backtest import BacktestResult, run_backtest
from . import eval as eval  # noqa: A004  (submodule: atxpy.eval.performance, ...)
from . import risk as risk
from .eval import Metrics, performance, return_metrics
from .risk import factor_model, optimize
from . import mining as mining
from .mining import mine_alphas, combine_pool
from . import learn as learn
from . import fund as fund

__version__ = "0.1.0"

__all__ = [
    "_core",
    "AtxError",
    "Decimal",
    "Symbol",
    "SymbolTable",
    # config / vocabulary
    "WeightPolicy",
    "Transform",
    "SlippageCfg",
    "SlippageMode",
    "CommissionCfg",
    "CommissionMode",
    "ImpactCfg",
    "FillCfg",
    "LatencyCfg",
    "VolumeCapCfg",
    "InstrumentStats",
    "OrderType",
    "BarsForSymbol",
    "BacktestParams",
    # alpha DSL
    "Library",
    "Program",
    "compile_alpha",
    "evaluate_alpha",
    # backtest facade
    "BacktestResult",
    "run_backtest",
    # eval
    "eval",
    "Metrics",
    "performance",
    "return_metrics",
    # risk
    "risk",
    "FactorModel",
    "OptimizerConfig",
    "PortfolioOptimizer",
    "factor_model",
    "optimize",
    # mining / combine
    "mining",
    "mine_alphas",
    "combine_pool",
    # learn (ML alphas) + fund (meta-book)
    "learn",
    "fund",
    "__version__",
]
