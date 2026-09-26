"""Static guard (tier-1 v2 node 2.2): as-of SQL never trusts today's latest flag or a NULL clock.

``is_latest_revision`` is today's knowledge: an as-of read that filters on it drops the original
row at every cutoff between it and a later restatement (and a projected flag tells a historical
read which values will be restated). ``available_at IS NULL OR ...`` makes a row with no clock
visible at every cutoff. Every SQL constant of ``atx_db.asof`` and every line of its sources
(inline f-string SQL included) must be free of both.
"""

from __future__ import annotations

import importlib
import pkgutil
import re
from pathlib import Path

import atx_db.asof as asof_package

FORBIDDEN = ("is_latest_revision", "available_at IS NULL OR")
_CONSTANT = re.compile(r"^[A-Z][A-Z0-9_]*_(SQL|CTE|JOIN)$")


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
