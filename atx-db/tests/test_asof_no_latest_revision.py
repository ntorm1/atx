"""Static guard (tier-1 v2 node 2.2): as-of SQL never trusts today's latest flag or a NULL clock.

``is_latest_revision`` means one of two things, and only one of them may be filtered on:

- **Revision flag** (most tables): several revisions of one key coexist and the writer marks the
  newest as latest. That is today's knowledge: an as-of read that filters on it drops the
  original row at every cutoff between it and a later restatement (and a projected flag tells a
  historical read which values will be restated). As-of reads pick the newest revision visible
  at the cutoff instead.
- **Retirement marker** (``fundamental_factor_values``, and ``cross_domain_factor_values`` for
  parity with ``v_factor_panel``): every writer keys one row per (source, factor, security,
  as_of_date), so there is never an older revision to hide; ``derived_factor_projection``
  sets the flag false on the legacy source's rows of a projected factor to retire them. A
  reader of these tables must keep the filter, or retired legacy values re-enter the panel
  and can override the governed projection value.

``available_at IS NULL OR ...`` makes a row with no clock visible at every cutoff. Every SQL
constant of ``atx_db.asof`` and every line of its sources (inline f-string SQL included) must be
free of both. ``signal_eval`` may carry the flag only as the two retirement predicates, one per
factor table; any other non-prose use fails.
"""

from __future__ import annotations

import ast
import importlib
import pkgutil
import re
from pathlib import Path

import atx_db.asof as asof_package
import atx_db.signal_eval as signal_eval_module

FORBIDDEN = ("is_latest_revision", "available_at IS NULL OR")
_CONSTANT = re.compile(r"^[A-Z][A-Z0-9_]*_(SQL|CTE|JOIN)$")
RETIREMENT_TABLES = ("fundamental_factor_values", "cross_domain_factor_values")
_FROM_TABLE = re.compile(r"\bFROM\s+([A-Za-z_][A-Za-z0-9_]*)")


def test_asof_sql_never_filters_on_latest_flag_or_null_clock() -> None:
    constants: dict[str, str] = {}
    for module_info in pkgutil.iter_modules(asof_package.__path__):
        module = importlib.import_module(f"{asof_package.__name__}.{module_info.name}")
        for name, value in vars(module).items():
            if _CONSTANT.match(name) and isinstance(value, str):
                constants[f"{module_info.name}.{name}"] = value
    assert len(constants) >= 40, sorted(constants)  # the package's readers were all imported
    offending = sorted(
        f"{name}: {token!r}" for name, sql in constants.items() for token in FORBIDDEN if token in sql
    )
    assert offending == []

    source_hits = sorted(
        f"{path.name}:{number}: {line.strip()}"
        for path in Path(asof_package.__path__[0]).glob("*.py")
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1)
        if any(token in line for token in FORBIDDEN)
    )
    assert source_hits == []


def _docstring_lines(source: str) -> set[int]:
    lines: set[int] = set()
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.body:
            first = node.body[0]
            if (
                isinstance(first, ast.Expr)
                and isinstance(first.value, ast.Constant)
                and isinstance(first.value.value, str)
            ):
                lines.update(range(first.lineno, (first.end_lineno or first.lineno) + 1))
    return lines


def test_signal_eval_keeps_only_the_factor_retirement_filters() -> None:
    path = Path(signal_eval_module.__file__)
    source = path.read_text(encoding="utf-8")
    prose = _docstring_lines(source)
    retirement_predicates: list[str] = []
    offending: list[str] = []
    table = None
    for number, line in enumerate(source.splitlines(), start=1):
        stripped = line.strip()
        is_prose = number in prose or stripped.startswith(("#", "--"))
        if not is_prose:
            tables = _FROM_TABLE.findall(line)
            if tables:
                table = tables[-1]
        if is_prose or not any(token in line for token in FORBIDDEN):
            continue
        if stripped == "AND is_latest_revision" and table in RETIREMENT_TABLES:
            retirement_predicates.append(table)
        else:
            offending.append(f"{path.name}:{number}: {stripped}")
    assert offending == []
    # Exactly one retirement predicate per factor table (load_panel_for_eval, factor_ids path).
    assert sorted(retirement_predicates) == sorted(RETIREMENT_TABLES)
