"""The research window: the one source of TRAIN and the seal for every research tool (platform v8 W0-1).

The values live in ``atx-impl/strategies/research_window.json`` (schema ``atx.research-window/v2``), found relative
to this file, so a checkout needs no installed package. ``atx-engine/include/atx/engine/data/research_window.hpp`` is
the C++ mirror; ``test_research_window.py`` and the gtest ``ResearchWindow.HeaderMatchesJson`` pin both to the JSON.

  TRAIN   decision sessions in [TRAIN_BEGIN_DATE, TRAIN_END_DATE)
  seal    nothing dated on or after SEAL_DATE (a session, a source row, a file or a statistic) is opened or used

Import it as ``import research_window as rw`` from ``atx-engine/tools`` (same directory) and as
``from engine_tools import research_window as rw`` from ``atx-impl/tools``. No tool hard-codes a TRAIN end or a seal
date. The module constants are the repository window; ``bind`` exists for test harnesses only (the synthetic
fixtures of the ``atx-engine/tools`` field builders are dated before the superseded seal; see ``conftest.py``).
"""
from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

SCHEMA = "atx.research-window/v2"
PATH = Path(__file__).resolve().parents[2] / "atx-impl" / "strategies" / "research_window.json"
DAY_NS = 86_400_000_000_000
_EPOCH = dt.date(1970, 1, 1)
_DATE_KEYS = ("train_begin", "train_end_exclusive", "seal_begin")


class SealError(ValueError):
    """A refusal: a role, field, file or statistic reaches the research seal."""


def date_ns(text: str) -> int:
    """Nanoseconds since the epoch of ``YYYY-MM-DD`` at 00:00 UTC (a session label)."""
    return (dt.date.fromisoformat(text) - _EPOCH).days * DAY_NS


def window_id(schema: str) -> str:
    """The window id named in outputs and errors: ``atx.research-window/v2`` -> ``research-window-v2``."""
    if not schema.startswith("atx.") or schema.count("/") != 1:
        raise ValueError(f"research window: unexpected schema {schema!r}")
    return schema[len("atx."):].replace("/", "-")


def _iso(value, key: str) -> str:
    if not isinstance(value, str) or dt.date.fromisoformat(value).isoformat() != value:
        raise ValueError(f"research window: {key} is not a YYYY-MM-DD date")
    return value


def load(path: Path | str | None = None) -> dict:
    """The validated window document (default: the repository's ``research_window.json``)."""
    doc = json.loads(Path(PATH if path is None else path).read_text(encoding="utf-8"))
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        raise ValueError(f"research window: schema is not {SCHEMA}")
    for key in _DATE_KEYS:
        _iso(doc.get(key), key)
    if not doc["train_begin"] < doc["train_end_exclusive"] <= doc["seal_begin"]:
        raise ValueError("research window: needs train_begin < train_end_exclusive <= seal_begin")
    return doc


def window_values(window: str, train_begin: str, train_end: str, seal: str) -> dict:
    """Every module constant of one window, keyed by constant name."""
    for key, value in (("train_begin", train_begin), ("train_end_exclusive", train_end), ("seal_begin", seal)):
        _iso(value, key)
    if not train_begin < train_end <= seal:
        raise ValueError("research window: needs train_begin < train_end_exclusive <= seal_begin")
    seal_day = dt.date.fromisoformat(seal)
    return {"WINDOW_ID": window, "TRAIN_BEGIN_DATE": train_begin, "TRAIN_END_DATE": train_end, "SEAL_DATE": seal,
            "SEAL": seal_day, "TRAIN_BEGIN_NS": date_ns(train_begin), "TRAIN_END_NS": date_ns(train_end),
            "SEAL_NS": date_ns(seal), "FIRST_SEALED_YEAR": seal_day.year}


def current(doc: dict | None = None) -> dict:
    """The constants of the repository window (``research-window-v2``)."""
    doc = load() if doc is None else doc
    return window_values(window_id(doc["schema"]), doc["train_begin"], doc["train_end_exclusive"], doc["seal_begin"])


def superseded(doc: dict | None = None) -> dict:
    """The constants of the window the repository window supersedes (``research-seal-v1``: TRAIN end 2023, seal
    2025). Only test harnesses bind it, for synthetic fixtures written under it."""
    doc = load() if doc is None else doc
    old = doc["supersedes"]
    return window_values(str(old["schema"]), doc["train_begin"], old["train_end_exclusive"], old["seal_begin"])


_REPOSITORY = current()
WINDOW_ID: str = _REPOSITORY["WINDOW_ID"]
TRAIN_BEGIN_DATE: str = _REPOSITORY["TRAIN_BEGIN_DATE"]
TRAIN_END_DATE: str = _REPOSITORY["TRAIN_END_DATE"]        # exclusive
SEAL_DATE: str = _REPOSITORY["SEAL_DATE"]                  # the first sealed date
SEAL: dt.date = _REPOSITORY["SEAL"]
TRAIN_BEGIN_NS: int = _REPOSITORY["TRAIN_BEGIN_NS"]
TRAIN_END_NS: int = _REPOSITORY["TRAIN_END_NS"]
SEAL_NS: int = _REPOSITORY["SEAL_NS"]
FIRST_SEALED_YEAR: int = _REPOSITORY["FIRST_SEALED_YEAR"]  # the first calendar year holding a sealed day


def bind(values: dict) -> None:
    """Rebind this module's constants to ``values`` (``current()`` or ``superseded()``). Test harnesses only, and
    before the tools that copy the constants are imported; production code never calls it."""
    if set(values) != set(_REPOSITORY):
        raise ValueError("research window: bind needs every constant of window_values()")
    globals().update(values)


def is_sealed(ns: int) -> bool:
    """True when a session label (or any UTC instant, in ns) is at or after the seal."""
    return int(ns) >= SEAL_NS


def seal_message(what: str) -> str:
    """The refusal text every tool uses: it names the window id and the seal date."""
    return f"{what} at or after the research seal {SEAL_DATE} ({WINDOW_ID}); refusing (sealed)"


def refuse_sealed(ns: int, what: str) -> None:
    """Raise ``SealError`` when ``ns`` is at or after the seal."""
    if is_sealed(ns):
        raise SealError(seal_message(what))


def period_begin(year: int, quarter: int | None = None) -> dt.date:
    """The first day of a calendar year, or of its quarter 1..4."""
    if quarter is not None and quarter not in (1, 2, 3, 4):
        raise ValueError(f"research window: quarter {quarter!r} is not 1..4")
    return dt.date(int(year), 1 if quarter is None else 3 * (quarter - 1) + 1, 1)


def partition_is_sealed(year: int, quarter: int | None = None) -> bool:
    """True when a year (or year-quarter) partition begins on or after the seal: it can hold only sealed rows and is
    never opened. A partition that begins before the seal may be read; its rows on or after the seal are dropped."""
    return period_begin(year, quarter) >= SEAL


def last_quarter_before_seal() -> tuple[int, int]:
    """(year, quarter) of the last calendar quarter that ends before the seal: (2023, 4) for a 2024-01-01 seal."""
    year, quarter = SEAL.year, (SEAL.month - 1) // 3 + 1  # the quarter holding the seal day ends on or after it
    return (year, quarter - 1) if quarter > 1 else (year - 1, 4)
