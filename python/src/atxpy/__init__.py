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
from .backtest import BacktestResult, run_backtest

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
    # backtest facade
    "BacktestResult",
    "run_backtest",
    "__version__",
]
