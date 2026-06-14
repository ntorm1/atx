"""atxpy - Python wrapper for the atx-engine quant backtesting engine."""

from __future__ import annotations

from . import _core
from ._core import AtxError, Decimal, Symbol, SymbolTable

__version__ = "0.1.0"

__all__ = ["_core", "AtxError", "Decimal", "Symbol", "SymbolTable", "__version__"]
