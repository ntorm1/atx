"""Facade for the alpha DSL: compile + evaluate over a research panel."""

from __future__ import annotations

import numpy as np

from . import _core


def compile_alpha(expr):
    """Compile a DSL expression string into a _core.Program."""
    return _core.compile_alpha(expr)


def evaluate_alpha(expr_or_program, panel):
    """Evaluate an alpha over a research panel.

    expr_or_program: a DSL string or a compiled _core.Program.
    panel: dict {field_name: 2D ndarray [dates, instruments]}.
    Returns a (dates, instruments) float64 ndarray (program root 0).
    """
    prog = (
        _core.compile_alpha(expr_or_program)
        if isinstance(expr_or_program, str)
        else expr_or_program
    )
    fields = {k: np.ascontiguousarray(v, dtype=np.float64) for k, v in panel.items()}
    return _core.evaluate_program(prog, fields)
