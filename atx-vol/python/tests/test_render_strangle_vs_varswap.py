"""Gate test for tools/render_strangle_vs_varswap.py.

The renderer's input is the track TSV `atx-vol-varswap-compare-example` writes
through `atx::vol::write_backtest_pnl_tsv`: a `# key=value` meta header, the 27
pinned series columns, then one dynamic column per signal — the eight
comparison signals the strategy publishes plus the tail the example attaches
(that tail is deliberately absent from the frozen serialized column set, so the
example rides it in as signals rather than touching `kBacktestSeriesColumns`).

THE ATTACHED TAIL IS NOT SPELLED OUT HERE. It is parsed at run time out of the
C++ that emits it, because a hand-copied list is exactly how task F-8 landed
eight P&L-explain columns in the engine that never reached the TSV: the C++ lane
emitted them, the Python lane did not know they existed, and nothing was red.

WHICH FILE IS PARSED CHANGED IN FIX ROUND 4, and the reason is the point. The
example used to hand-list all ten attaches, so this module read that table and
compared it against `backtest.hpp`'s declarations. The example now DERIVES its
explain tail from `swap_explain_columns()` — the engine's single roster — so the
C++ compiler guarantees that comparison and re-asserting it here would be a
tautology dressed as coverage. What is parsed instead:

  * `ROSTER_COLUMNS` from `src/backtest.cpp`'s `kSwapExplainColumns`, checked
    against the header's declarations. This is the pairing C++ cannot check:
    there is no reflection, so a row naming one column beside another's member
    compiles and ships a mislabelled TSV column.
  * `DRIVER_SIGNAL_COLUMNS` from the example, now only its two literal lead
    attaches — asserted to contain NO explain column, which is the
    anti-regression for the unification itself.

The fixture below is hand-built rather than produced by a run, and it encodes
the four data facts the renderer has to survive:

  * a ONE-LEGGED TAIL. The last cycle of a corpus whose calendar runs out
    mid-tenor carries no swap, so every `swap_*` signal is NaN on those rows.
    That is data, not an error.
  * `swap_theta` is NaN ON ITS OWN while the swap is LIVE (`deriv_greeks`
    declines the roll stencil inside one bump width of expiry), so liveness is
    keyed off `swap_vega` and never off `swap_theta`.
  * `skipped_restrikes` / `skipped_swaps` are CUMULATIVE, so the per-session
    event count is the consecutive-row difference.
  * the P&L EXPLAIN IS OPT-IN. `RunConfig::swap_pnl_explain` is off by default
    because it costs up to 20 repricings per live lot per step, and off, the
    engine leaves those columns EMPTY rather than zero-filled — the distinction
    between "not computed" and "computed as zero". A track without them is
    therefore ordinary input, and reading their absence as a flat attribution
    would destroy exactly the distinction the C++ side paid to keep. The fixture
    writes tracks both ways.
"""

from __future__ import annotations

import importlib.util
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

import pytest


_ATX_VOL_ROOT = pathlib.Path(__file__).resolve().parents[2]
_TOOL = _ATX_VOL_ROOT / "tools" / "render_strangle_vs_varswap.py"

# The renderer imports matplotlib + pandas at MODULE scope, and this suite
# declares neither: `atx-vol/python/pyproject.toml`'s test extra is
# `["pytest>=7", "numpy>=1.23"]`. Executing it unconditionally would turn a
# missing OPTIONAL dependency into a COLLECTION error, and a collection error in
# one module reds the WHOLE `atx-vol-python` ctest lane — every other module in
# this directory would report failure because a plotting library is absent.
#
# Skipping at module level is the policy the lane already applies one level up:
# `_ctest_pytest_driver.py` exits `SKIP_RETURN_CODE` 77 when the compiled
# extension is missing, so the lane reads as Skipped-with-a-reason rather than
# red. An absent prerequisite is a skip; only a present one that misbehaves is a
# failure.
#
# WIDENING THE SHARED TEST EXTRA IS DELIBERATELY NOT THE FIX. That list is the
# install contract for every consumer of `pip install .[test]`, including the
# scikit-build-core wheel build, and adding a plotting stack to it to serve one
# module charges every other module for a dependency none of them import.
# `pytest` is itself declared, so this guard adds no new dependency of its own.
try:
    import matplotlib  # noqa: F401
    import pandas  # noqa: F401
except ImportError as exc:  # pragma: no cover — depends on the host environment
    pytest.skip(
        "tools/render_strangle_vs_varswap.py needs matplotlib + pandas, which the "
        f"atx-vol Python test extra does not declare: {exc}",
        allow_module_level=True,
    )

_spec = importlib.util.spec_from_file_location("render_strangle_vs_varswap", _TOOL)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load renderer from {_TOOL}")
renderer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = renderer
_spec.loader.exec_module(renderer)


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

META = {
    "strategy": "xom_strangle_vs_varswap",
    "symbol": "XOM",
    "data_source": "surface_db",
    "db_root": "C:/atx-data/surface-db/sp100-2026",
    "window_start": "2026-01-02",
    "window_end": "2026-01-08",
    "n_steps": "5",
    "delta_target": "0.4",
    "tenor_days": "91",
    "contracts": "100",
    "hedge": "delta_to_zero_daily",
    "skipped_restrikes": "1",
    "skipped_swaps": "1",
    "total_return": "700.0",
    "sharpe": "0.9",
    "max_drawdown": "800.0",
}

# The 27 pinned columns of write_backtest_pnl_tsv (date, ts_ns, then the 25 F64
# series of backtest_series_columns.hpp), in order.
PINNED_COLUMNS = [
    "date", "ts_ns", "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna",
    "pnl_volga", "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega", "n_open_lots",
    "n_unpriced_lots", "n_unpriced_greeks",
]

# ── the dynamic signal tail, derived from the C++ that emits it ─────────────
#
# The strategy's eight come out of `src/strategy.cpp` / `src/swap_leg.cpp`, one
# `out.emplace_back` per probe greek plus the two cumulative counters. The rest
# of the tail is whatever `examples/varswap_compare_example.cpp` attaches, and is
# READ FROM THAT FILE rather than copied — see the module docstring.
STRATEGY_SIGNAL_COLUMNS = [
    "swap_delta", "swap_gamma", "swap_vega", "swap_theta", "swap_rho", "strangle_vega",
    "skipped_restrikes", "skipped_swaps",
]

_HEADER = _ATX_VOL_ROOT / "include" / "atx" / "vol" / "backtest.hpp"
_EXAMPLE = _ATX_VOL_ROOT / "examples" / "varswap_compare_example.cpp"
_ROSTER = _ATX_VOL_ROOT / "src" / "backtest.cpp"

_EXPLAIN_PREFIX = "swap_explain_"


def _leading_identifier(text: str) -> str:
    """The C identifier `text` starts with, or "" if it does not start with one.

    Hand-rolled rather than a regex ON PURPOSE. Every caller below reports a
    DISTINGUISHABLE failure — nothing declared, declared twice, a cell that will
    not parse — and a regex that simply does not match collapses all three into
    one indistinguishable negative. A silent non-match is the failure mode these
    parsers exist to replace, so it must not be reintroduced by the splitter.
    """
    end = 0
    while end < len(text) and (text[end].isalnum() or text[end] == "_"):
        end += 1
    return text[:end]


def _comment_body(line: str) -> str:
    """The prose of a `//` comment line, with the slashes and padding removed."""
    return line.strip().lstrip("/").strip()


def declared_explain_columns(source: str, where: str) -> list[str]:
    """The `swap_explain_*` members `BacktestResult` declares, in order.

    One entry per name on a `std::vector<double> a, b;` declaration line, so the
    engine's own field list is the source of truth for what the explain is.
    """
    lines = source.splitlines()
    found: list[str] = []
    for line in lines:
        text = line.strip()
        if text.startswith("//") or not text.startswith("std::vector<double>"):
            continue
        body = text[len("std::vector<double>"):].strip().rstrip(";")
        for part in body.split(","):
            name = _leading_identifier(part.strip())
            if name.startswith(_EXPLAIN_PREFIX):
                found.append(name)
    if not found:
        raise AssertionError(
            f"{where}: no `std::vector<double> {_EXPLAIN_PREFIX}*` member declared "
            f"in {len(lines)} lines scanned. This test reads the engine's own field "
            "list; if the explain moved, point it at the new home rather than "
            "hand-copying the names back in."
        )
    duplicated = sorted({n for n in found if found.count(n) > 1})
    if duplicated:
        raise AssertionError(
            f"{where}: {duplicated} declared more than once, so the column order "
            "this test derives is ambiguous."
        )
    return found


def identity_flow_columns(source: str, where: str) -> list[str]:
    """The summands of `BacktestResult`'s `... == swap_pnl` identity comment.

    That comment is the engine's statement of WHICH explain columns are dollar
    flows that decompose `swap_pnl`; the rest of `swap_explain_*` is not. The
    renderer must sum exactly these, so it is read from there rather than
    re-decided here.
    """
    lines = source.splitlines()
    hits = [
        i for i, line in enumerate(lines)
        if line.strip().startswith("//") and "== swap_pnl" in line
    ]
    if len(hits) != 1:
        raise AssertionError(
            f"{where}: {len(hits)} comment lines carry the `== swap_pnl` identity "
            f"({len(lines)} lines scanned); exactly one is required, and "
            f"{'none names the explain components' if not hits else 'more than one is ambiguous'}."
        )
    row = hits[0]
    if row == 0:
        raise AssertionError(f"{where}: the `== swap_pnl` identity has no summand line above it.")
    expression = f"{_comment_body(lines[row - 1])} {_comment_body(lines[row])}"
    left, _, right = expression.partition("==")
    if right.strip() != "swap_pnl":
        raise AssertionError(
            f"{where}: the identity reads `{expression}`, whose right-hand side is "
            f"{right.strip()!r} rather than `swap_pnl`."
        )
    summands: list[str] = []
    for part in left.split("+"):
        name = _leading_identifier(part.strip())
        if not name or name != part.strip():
            raise AssertionError(
                f"{where}: identity summand {part.strip()!r} is not a bare component "
                f"name, so the explain column it means cannot be derived "
                f"(identity read as `{expression}`)."
            )
        summands.append(_EXPLAIN_PREFIX + name)
    duplicated = sorted({n for n in summands if summands.count(n) > 1})
    if duplicated:
        raise AssertionError(f"{where}: the identity names {duplicated} twice.")
    return summands


def roster_columns(source: str, where: str) -> list[str]:
    """The `swap_explain_*` columns `kSwapExplainColumns` names, in table order.

    THE ROSTER IS THE ENGINE'S SINGLE LIST, and everything C++-side is driven
    from it: `validate()`, `push_row`, both `backtest_db` shape rules, and (since
    fix round 4) the example's attach loop. What C++ CANNOT check about it is the
    one thing this parser exists for -- the name STRING beside each member. C++
    has no reflection, so
    `{"swap_explain_skew", &BacktestResult::swap_explain_convexity}` compiles
    happily and ships a TSV column labelled `skew` carrying convexity dollars.
    Each row must name the same identifier twice.

    Read from `src/backtest.cpp` rather than copied. Order matters: it is the
    enum order, the TSV column order, and -- asserted below -- the declaration
    order.
    """
    lines = source.splitlines()
    found: list[str] = []
    for line in lines:
        text = line.strip()
        if not text.startswith('{"'):
            continue
        name, quote, rest = text[2:].partition('"')
        if not quote:
            raise AssertionError(f"{where}: unterminated column name in roster row `{text}`.")
        if not name.startswith(_EXPLAIN_PREFIX):
            continue
        _, marker, tail = rest.partition("&BacktestResult::")
        if not marker:
            raise AssertionError(
                f"{where}: roster row `{text}` names no `&BacktestResult::<field>` "
                "member, so the column cannot be tied back to the field it carries."
            )
        field = _leading_identifier(tail)
        if name != field:
            raise AssertionError(
                f"{where}: roster row `{text}` emits column {name!r} from field "
                f"{field!r}. They must match - the renderer finds the explain tail "
                "by the field-name prefix, and C++ cannot check this pairing."
            )
        found.append(name)
    if not found:
        raise AssertionError(
            f"{where}: no `{_EXPLAIN_PREFIX}*` roster rows naming a "
            f"`&BacktestResult::` member in {len(lines)} lines scanned. This test "
            "reads the engine's own roster; if it moved, point this at the new home "
            "rather than hand-copying the names back in."
        )
    duplicated = sorted({n for n in found if found.count(n) > 1})
    if duplicated:
        raise AssertionError(f"{where}: roster names {duplicated} more than once.")
    return found


def example_attached_columns(source: str, where: str) -> list[str]:
    """The signal columns the example HAND-LISTS, in emitted order.

    Since fix round 4 the example derives its explain tail from
    `swap_explain_columns()`, so the only literal attaches left are the two lead
    columns. Each reads `attach_one(r, "<name>", r.<field>)`, and the two halves
    must be the SAME identifier: the TSV column IS the result field, which is
    what lets the renderer discover the explain tail by prefix.

    This parser therefore no longer derives the tail -- `roster_columns` does.
    What it still establishes is that nothing has hand-listed an explain column
    back into this file.
    """
    marker_text = 'attach_one(r, "'
    lines = source.splitlines()
    attached: list[str] = []
    for line in lines:
        _, marker, rest = line.partition(marker_text)
        if not marker:
            continue
        name, quote, tail = rest.partition('"')
        if not quote:
            raise AssertionError(f"{where}: unterminated column name in attach `{line.strip()}`.")
        _, member, member_tail = tail.partition("r.")
        if not member:
            raise AssertionError(
                f"{where}: attach `{line.strip()}` names no `r.<field>` member, so the "
                "column cannot be tied back to the field it carries."
            )
        field = _leading_identifier(member_tail)
        if name != field:
            raise AssertionError(
                f"{where}: attach `{line.strip()}` emits column {name!r} from field "
                f"{field!r}. They must match - the renderer finds the explain tail "
                "by the field-name prefix, and a renamed column silently hides it."
            )
        attached.append(name)
    if not attached:
        raise AssertionError(
            f'{where}: no `attach_one(r, "name", r.field)` calls in {len(lines)} lines '
            "scanned, so what this fixture hand-lists cannot be determined."
        )
    return attached


EXPLAIN_COLUMNS = declared_explain_columns(_HEADER.read_text(encoding="utf-8"), str(_HEADER))
EXPLAIN_FLOW_COLUMNS = identity_flow_columns(_HEADER.read_text(encoding="utf-8"), str(_HEADER))
ROSTER_COLUMNS = roster_columns(_ROSTER.read_text(encoding="utf-8"), str(_ROSTER))
# What the example HAND-LISTS. Since fix round 4 that is only the two lead
# columns; the explain tail is a loop over the roster.
DRIVER_SIGNAL_COLUMNS = example_attached_columns(
    _EXAMPLE.read_text(encoding="utf-8"), str(_EXAMPLE)
)


def _explain_counter() -> str:
    """The one `swap_explain_*` column the identity does NOT sum: the counter.

    Derived by difference so neither list is written down twice. Two counters (or
    none) means the engine changed shape in a way the renderer's single exclusion
    rule no longer describes, and that has to be a loud failure rather than a
    silently wrong attribution.
    """
    rest = [c for c in EXPLAIN_COLUMNS if c not in EXPLAIN_FLOW_COLUMNS]
    if len(rest) != 1:
        raise AssertionError(
            f"{_HEADER}: expected exactly one `{_EXPLAIN_PREFIX}*` column outside the "
            f"`== swap_pnl` identity (the unattributed COUNTER); found {rest}. The "
            "renderer excludes exactly one column from the attribution sum."
        )
    return rest[0]


EXPLAIN_COUNTER = _explain_counter()

# The full dynamic tail, in TSV order: the strategy's signals, then what the
# example emits -- its two hand-listed lead columns followed by its loop over the
# roster. Assembled from the two halves in that order because that is literally
# what `attach_swap_columns` does; before fix round 4 the example hand-listed the
# whole thing and this was one parse.
EMITTED_SIGNAL_COLUMNS = DRIVER_SIGNAL_COLUMNS + ROSTER_COLUMNS
SIGNAL_COLUMNS = STRATEGY_SIGNAL_COLUMNS + EMITTED_SIGNAL_COLUMNS

DATES = ["2026-01-02", "2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08"]
BASE_TS = 1767398400000000000
DAY_NS = 86_400_000_000_000

PNL_TOTAL = [0.0, 1200.0, -800.0, 450.0, -150.0]
SWAP_PNL = [0.0, 500.0, -300.0, 100.0, 0.0]
SWAP_PV = [0.0, 500.0, 200.0, 0.0, 0.0]
GROSS_VEGA = [12000.0, 11800.0, 11500.0, 6000.0, 5800.0]
GROSS_DELTA = [0.0, 1e-7, -2e-7, 5e-8, 0.0]
GROSS_GAMMA = [3.2, 3.4, 3.6, 1.1, 1.0]
GROSS_THETA = [-450.0, -460.0, -470.0, -220.0, -215.0]
# Row 1 has a LIVE swap with a NaN theta; rows 3-4 are the one-legged tail.
SWAP_VEGA = [12000.0, 11750.0, 11400.0, math.nan, math.nan]
SWAP_THETA = [-120.0, math.nan, -125.0, math.nan, math.nan]
SWAP_DELTA = [-30.0, -28.0, -25.0, math.nan, math.nan]
SWAP_GAMMA = [0.9, 0.95, 1.0, math.nan, math.nan]
SWAP_RHO = [40.0, 41.0, 42.0, math.nan, math.nan]
STRANGLE_VEGA = [12000.0, 11800.0, 11500.0, 6000.0, 5800.0]
SKIPPED_RESTRIKES = [0.0, 0.0, 1.0, 1.0, 1.0]
SKIPPED_SWAPS = [0.0, 0.0, 0.0, 1.0, 1.0]
# Lot-steps whose move the explain could not attribute (a lot's first mark, a
# failed greek solve). NON-ZERO on two rows on purpose: it is the one
# `swap_explain_*` column that is a COUNT rather than a dollar flow, so a
# renderer that summed it into the attribution would break the identity here.
UNATTRIBUTED = [1.0, 0.0, 2.0, 0.0, 0.0]

# nav is the engine's running Σ step_total, i.e. the cumulative pnl_total.
NAV = [0.0, 1200.0, 400.0, 850.0, 700.0]


def explain_split(total: float) -> list[float]:
    """Attribution shares for one row that sum EXACTLY back to `total`.

    Thirty-seconds, so every share is exact in binary for this fixture's P&L
    values and the identity test measures the RENDERER's arithmetic rather than
    the fixture's own rounding. The shares are DISTINCT (1, 2, 3 ... units) so a
    renderer that mislabelled or reordered the components would not still agree
    with this fixture by accident.
    """
    unit = total / 32.0
    head = [unit * float(j + 1) for j in range(len(EXPLAIN_FLOW_COLUMNS) - 1)]
    return head + [total - math.fsum(head)]


_FIXTURE_SERIES = {
    "swap_delta": SWAP_DELTA,
    "swap_gamma": SWAP_GAMMA,
    "swap_vega": SWAP_VEGA,
    "swap_theta": SWAP_THETA,
    "swap_rho": SWAP_RHO,
    "strangle_vega": STRANGLE_VEGA,
    "skipped_restrikes": SKIPPED_RESTRIKES,
    "skipped_swaps": SKIPPED_SWAPS,
    "swap_pv": SWAP_PV,
    "swap_pnl": SWAP_PNL,
}


def _signal_value(name: str, i: int) -> float:
    if name in _FIXTURE_SERIES:
        return _FIXTURE_SERIES[name][i]
    if name == EXPLAIN_COUNTER:
        return UNATTRIBUTED[i]
    if name in EXPLAIN_FLOW_COLUMNS:
        return explain_split(SWAP_PNL[i])[EXPLAIN_FLOW_COLUMNS.index(name)]
    raise AssertionError(
        f"this fixture has no series for signal column {name!r}, which "
        f"{_EXAMPLE.name} now attaches. A track the renderer must survive is one "
        "this fixture can write, so give the new column data here."
    )


def _cell(v: float) -> str:
    # write_backtest_pnl_tsv writes %.17g, which renders a quiet NaN as "nan".
    return "nan" if isinstance(v, float) and math.isnan(v) else repr(float(v))


def _row(i: int, columns: list[str]) -> list[str]:
    pinned = {
        "date": DATES[i],
        "ts_ns": str(BASE_TS + i * DAY_NS),
        "pnl_total": _cell(PNL_TOTAL[i]),
        "pnl_delta": _cell(0.0),
        "pnl_gamma": _cell(0.4 * PNL_TOTAL[i]),
        "pnl_vega": _cell(0.3 * PNL_TOTAL[i]),
        "pnl_vanna": _cell(0.0),
        "pnl_volga": _cell(0.0),
        "pnl_theta": _cell(0.2 * PNL_TOTAL[i]),
        "pnl_rho": _cell(0.0),
        "pnl_charm": _cell(0.0),
        "pnl_unexplained": _cell(0.1 * PNL_TOTAL[i]),
        "pnl_settlement": _cell(0.0),
        "pnl_shares": _cell(0.0),
        "financing": _cell(0.0),
        "cost": _cell(0.0),
        "nav": _cell(NAV[i]),
        "cash": _cell(1_000_000.0 - NAV[i]),
        "gross_delta": _cell(GROSS_DELTA[i]),
        "gross_gamma": _cell(GROSS_GAMMA[i]),
        "gross_vega": _cell(GROSS_VEGA[i]),
        "gross_theta": _cell(GROSS_THETA[i]),
        "turnover_notional": _cell(250_000.0),
        "turnover_vega": _cell(4200.0),
        "n_open_lots": _cell(2.0),
        "n_unpriced_lots": _cell(0.0),
        "n_unpriced_greeks": _cell(0.0),
    }
    return [pinned[c] if c in pinned else _cell(_signal_value(c, i)) for c in columns]


def track_columns(*, with_swap: bool = True, with_explain: bool = True) -> list[str]:
    """The header row of a track written with (or without) each optional lane.

    `with_explain=False` is a run left on the `RunConfig::swap_pnl_explain`
    DEFAULT: the engine leaves those vectors empty and the example attaches
    nothing, so the columns are ABSENT from the TSV — not present and zero.
    """
    if not with_swap:
        return list(PINNED_COLUMNS)
    tail = [c for c in SIGNAL_COLUMNS if with_explain or not c.startswith(_EXPLAIN_PREFIX)]
    return PINNED_COLUMNS + tail


def write_track(
    path: pathlib.Path, *, with_swap: bool = True, with_explain: bool = True
) -> None:
    """The exact bytes write_backtest_pnl_tsv would emit for this fixture."""
    columns = track_columns(with_swap=with_swap, with_explain=with_explain)
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        for k, v in META.items():
            fh.write(f"# {k}={v}\n")
        fh.write("\t".join(columns) + "\n")
        for i in range(len(DATES)):
            fh.write("\t".join(_row(i, columns)) + "\n")


class ColumnDerivationTests(unittest.TestCase):
    """The one place the C++ and Python views of the signal tail are compared.

    Task F-8 landed eight `swap_explain_*` columns in the engine and nothing
    carried them to the TSV, because the two lanes agreed only by hand. These
    tests are the agreement.
    """

    # ── WHAT THIS CLASS STILL TESTS, after fix round 4 ─────────────────────
    #
    # It used to assert that the EXAMPLE attaches exactly what the HEADER
    # declares, because the two were independently hand-written. Round 4 drove
    # the example's attach loop from `swap_explain_columns()`, so the C++
    # compiler now guarantees that half. Re-asserting it here would be asserting
    # a derived thing against the thing it was derived from -- a tautology that
    # reads as coverage, which is worse than no test because it manufactures
    # exactly the confidence the unification was supposed to earn. That
    # assertion is DELETED, deliberately, and this paragraph is the record.
    #
    # What genuinely remains, and why C++ cannot do it:
    #   1. ROSTER row pairing. `{"name", &BacktestResult::field}` -- C++ has no
    #      reflection, so a transposed pairing compiles and mislabels a TSV
    #      column. Only text can check it. (`roster_columns` does, per row.)
    #   2. ROSTER order vs DECLARATION order. Round 4 pinned each roster index to
    #      its own member in C++, so the enum/roster half is a build error now;
    #      what stays here is the roster-against-declarations comparison, which
    #      is what the renderer's prefix discovery and this TSV's column order
    #      rest on.
    #   3. The example hand-lists NO explain column. That is the anti-regression
    #      for the unification itself -- someone re-adding a literal row would
    #      silently reintroduce the fifth copy.
    #   4. The renderer sums exactly the header's identity components (below).

    def test_the_roster_and_the_header_declare_the_same_explain_in_the_same_order(self) -> None:
        self.assertEqual(
            ROSTER_COLUMNS,
            EXPLAIN_COLUMNS,
            f"{_ROSTER} names the explain roster as {ROSTER_COLUMNS}, but {_HEADER} "
            f"declares {EXPLAIN_COLUMNS}. Everything C++-side is driven from the "
            "roster, so a column declared and not rostered never reaches a report, "
            "and the ORDER is the TSV's column order. The arity pin catches a count "
            "change; it is blind to a reorder, which is why this compares lists.",
        )

    def test_the_example_hand_lists_no_explain_column(self) -> None:
        # The unification's anti-regression. The example attaches the two lead
        # columns literally and derives the rest; a literal `swap_explain_*` row
        # reappearing here is the fifth roster copy coming back.
        self.assertEqual(
            DRIVER_SIGNAL_COLUMNS,
            ["swap_pv", "swap_pnl"],
            f"{_EXAMPLE} hand-lists {DRIVER_SIGNAL_COLUMNS}. Only swap_pv and "
            "swap_pnl may be literal -- they are the quantity being explained, not "
            "components of it. The explain tail must stay a loop over "
            "swap_explain_columns(), or it becomes a hand-kept copy of the roster "
            "again.",
        )

    def test_the_renderer_sums_exactly_the_headers_identity_and_excludes_the_counter(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            track = pathlib.Path(raw) / "track.tsv"
            write_track(track)
            _meta, df = renderer.read_track(track)

            self.assertEqual(list(renderer.explain_components(df)), EXPLAIN_FLOW_COLUMNS)
        self.assertEqual(
            renderer._EXPLAIN_COUNTER,
            EXPLAIN_COUNTER,
            "the renderer excludes one column from the attribution sum; it must be "
            "the one column backtest.hpp's `== swap_pnl` identity leaves out.",
        )

    def test_a_header_with_no_explain_declaration_is_a_named_failure(self) -> None:
        with self.assertRaises(AssertionError) as caught:
            declared_explain_columns("struct BacktestResult {\n  int n;\n};\n", "fake.hpp")

        self.assertIn("no `std::vector<double> swap_explain_*` member", str(caught.exception))
        self.assertIn("3 lines scanned", str(caught.exception))

    def test_a_twice_declared_explain_column_is_a_named_failure(self) -> None:
        source = (
            "  std::vector<double> swap_explain_carry, swap_explain_skew;\n"
            "  std::vector<double> swap_explain_carry;\n"
        )

        with self.assertRaises(AssertionError) as caught:
            declared_explain_columns(source, "fake.hpp")

        self.assertIn("['swap_explain_carry'] declared more than once", str(caught.exception))

    def test_a_missing_or_duplicated_identity_is_a_named_failure(self) -> None:
        with self.assertRaises(AssertionError) as caught:
            identity_flow_columns("// nothing here\n", "fake.hpp")
        self.assertIn("0 comment lines carry the `== swap_pnl` identity", str(caught.exception))

        twice = (
            "//   carry + residual\n"
            "//     == swap_pnl\n"
            "//   carry + residual\n"
            "//     == swap_pnl\n"
        )
        with self.assertRaises(AssertionError) as caught:
            identity_flow_columns(twice, "fake.hpp")
        self.assertIn("2 comment lines carry the `== swap_pnl` identity", str(caught.exception))

    def test_an_unparseable_identity_summand_is_a_named_failure(self) -> None:
        source = "//   carry + 2 * residual\n//     == swap_pnl\n"

        with self.assertRaises(AssertionError) as caught:
            identity_flow_columns(source, "fake.hpp")

        self.assertIn("'2 * residual' is not a bare component name", str(caught.exception))

    def test_a_roster_row_whose_column_renames_its_field_is_a_named_failure(self) -> None:
        # The one check C++ cannot make, so its negative control matters most.
        source = '    {"swap_explain_skew", &BacktestResult::swap_explain_convexity},\n'

        with self.assertRaises(AssertionError) as caught:
            roster_columns(source, "fake.cpp")

        self.assertIn("emits column 'swap_explain_skew' from field", str(caught.exception))
        self.assertIn("'swap_explain_convexity'", str(caught.exception))

    def test_a_roster_with_no_rows_is_a_named_failure(self) -> None:
        with self.assertRaises(AssertionError) as caught:
            roster_columns("int main() { return 0; }\n", "fake.cpp")

        self.assertIn("roster rows naming a `&BacktestResult::` member", str(caught.exception))
        self.assertIn("1 lines scanned", str(caught.exception))

    def test_a_twice_rostered_column_is_a_named_failure(self) -> None:
        source = (
            '    {"swap_explain_carry", &BacktestResult::swap_explain_carry},\n'
            '    {"swap_explain_carry", &BacktestResult::swap_explain_carry},\n'
        )

        with self.assertRaises(AssertionError) as caught:
            roster_columns(source, "fake.cpp")

        self.assertIn("roster names ['swap_explain_carry'] more than once", str(caught.exception))

    def test_an_attach_row_whose_column_renames_its_field_is_a_named_failure(self) -> None:
        source = '  ATX_TRY_VOID(attach_one(r, "swap_explain_carry", r.swap_explain_realized));\n'

        with self.assertRaises(AssertionError) as caught:
            example_attached_columns(source, "fake.cpp")

        self.assertIn("emits column 'swap_explain_carry' from field", str(caught.exception))
        self.assertIn("'swap_explain_realized'", str(caught.exception))

    def test_an_example_with_no_attach_table_is_a_named_failure(self) -> None:
        with self.assertRaises(AssertionError) as caught:
            example_attached_columns("int main() { return 0; }\n", "fake.cpp")

        self.assertIn('no `attach_one(r, "name", r.field)` calls in 1 lines', str(caught.exception))


class TrackReaderTests(unittest.TestCase):
    def test_reads_meta_header_and_tab_separated_series(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            track = pathlib.Path(raw) / "track.tsv"
            write_track(track)

            meta, df = renderer.read_track(track)

            self.assertEqual(meta["symbol"], "XOM")
            self.assertEqual(meta["strategy"], "xom_strangle_vs_varswap")
            self.assertEqual(len(df), 5)
            self.assertIn("swap_pnl", df.columns)
            self.assertIn("swap_vega", df.columns)
            # `date` is parsed, so the panels can put a real time axis on x.
            self.assertEqual(str(df["date"].iloc[0].date()), "2026-01-02")


class LegSplitTests(unittest.TestCase):
    def _legs(self, *, with_swap: bool = True, with_explain: bool = True):
        with tempfile.TemporaryDirectory() as raw:
            track = pathlib.Path(raw) / "track.tsv"
            write_track(track, with_swap=with_swap, with_explain=with_explain)
            _meta, df = renderer.read_track(track)
            return renderer.split_legs(df)

    def test_strangle_leg_is_pnl_total_net_of_the_swap_column(self) -> None:
        legs = self._legs()

        # pnl_total is the engine's whole step total (options + settlement +
        # hedge shares + financing - cost + swap), so the OPTIONS-side leg is
        # what is left once the swap's flow column is taken back out.
        self.assertEqual(list(legs.strangle_step), [0.0, 700.0, -500.0, 350.0, -150.0])
        self.assertEqual(list(legs.strangle_cum), [0.0, 700.0, 200.0, 550.0, 400.0])

    def test_swap_leg_is_the_cumulative_swap_pnl_column(self) -> None:
        legs = self._legs()

        self.assertEqual(list(legs.swap_step), SWAP_PNL)
        self.assertEqual(list(legs.swap_cum), [0.0, 500.0, 200.0, 300.0, 300.0])

    def test_two_legs_sum_back_to_the_runs_nav(self) -> None:
        legs = self._legs()

        for got_strangle, got_swap, nav in zip(legs.strangle_cum, legs.swap_cum, NAV):
            self.assertAlmostEqual(got_strangle + got_swap, nav, places=9)

    def test_swap_liveness_keys_off_vega_not_theta(self) -> None:
        legs = self._legs()

        # Row 1 has a live swap with a NaN theta; rows 3-4 are the one-legged
        # tail. Keying liveness off swap_theta would drop row 1 as well.
        self.assertEqual(list(legs.swap_live), [True, True, True, False, False])
        self.assertEqual(legs.n_swap_live_rows, 3)

    def test_cumulative_skip_counters_are_differenced_into_per_step_events(self) -> None:
        legs = self._legs()

        self.assertEqual(list(legs.restrike_events), [0.0, 0.0, 1.0, 0.0, 0.0])
        self.assertEqual(list(legs.swap_skip_events), [0.0, 0.0, 0.0, 1.0, 0.0])
        self.assertEqual(legs.total_restrike_skips, 1.0)
        self.assertEqual(legs.total_swap_skips, 1.0)

    def test_nan_swap_rows_do_not_poison_the_cumulative_leg(self) -> None:
        legs = self._legs()

        for v in legs.swap_cum:
            self.assertFalse(math.isnan(v), "a NaN swap greek must not reach the P&L leg")

    def test_a_track_with_no_swap_columns_still_splits(self) -> None:
        legs = self._legs(with_swap=False)

        # An options-only run (enable_swap_leg off) is legal input: the swap leg
        # is flat-zero and no row reports a live swap.
        self.assertEqual(list(legs.strangle_step), PNL_TOTAL)
        self.assertEqual(list(legs.swap_cum), [0.0] * 5)
        self.assertEqual(legs.n_swap_live_rows, 0)

    def test_the_explain_components_close_back_onto_the_swap_leg(self) -> None:
        legs = self._legs()

        self.assertEqual(list(legs.explain), EXPLAIN_FLOW_COLUMNS)
        for i, swap_step in enumerate(SWAP_PNL):
            summed = math.fsum(float(c.iloc[i]) for c in legs.explain.values())
            self.assertAlmostEqual(summed, swap_step, places=9)
        self.assertAlmostEqual(legs.max_explain_gap, 0.0, places=9)
        self.assertEqual(legs.total_unattributed, sum(UNATTRIBUTED))

    def test_an_explainless_track_reports_no_attribution_rather_than_a_flat_one(self) -> None:
        legs = self._legs(with_explain=False)

        # `RunConfig::swap_pnl_explain` off leaves the engine's vectors EMPTY, so
        # the columns never reach the TSV. NOT COMPUTED IS NOT COMPUTED AS ZERO:
        # a zero here would report a swap whose whole P&L was attributed and
        # every component of it flat, which is a different claim entirely.
        self.assertFalse(legs.has_explain)
        self.assertEqual(legs.explain, {})
        self.assertTrue(math.isnan(legs.max_explain_gap))
        self.assertTrue(math.isnan(legs.total_unattributed))
        # The swap leg itself is untouched by the explain being off.
        self.assertEqual(list(legs.swap_step), SWAP_PNL)

    def test_a_component_that_did_not_compute_stays_nan(self) -> None:
        # Within a PRESENT explain column, a cell the engine could not fill is
        # NaN, and NaN is not zero either: the memoized-skew Critical of this
        # sprint was exactly a 0.0 standing in for an uncomputed sensitivity.
        df = pandas.DataFrame(
            {
                "swap_pnl": [1.0, 2.0],
                EXPLAIN_FLOW_COLUMNS[0]: [1.0, float("nan")],
                EXPLAIN_COUNTER: [0.0, 1.0],
            }
        )

        components = renderer.explain_components(df)

        self.assertEqual(list(components), [EXPLAIN_FLOW_COLUMNS[0]])
        self.assertEqual(float(components[EXPLAIN_FLOW_COLUMNS[0]].iloc[0]), 1.0)
        self.assertTrue(math.isnan(float(components[EXPLAIN_FLOW_COLUMNS[0]].iloc[1])))


class RenderTests(unittest.TestCase):
    def test_renders_a_png_over_a_track_with_nan_swap_rows(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "report.png"
            write_track(track)

            summary = renderer.render(track, out)

            self.assertTrue(out.exists())
            png = out.read_bytes()
            self.assertEqual(png[:8], PNG_MAGIC)
            self.assertGreater(len(png), 20_000, "a real 150-dpi figure is > 20 KB")
            self.assertEqual(summary["n_rows"], 5)
            self.assertEqual(summary["n_swap_live_rows"], 3)
            self.assertAlmostEqual(summary["strangle_total"], 400.0, places=9)
            self.assertAlmostEqual(summary["swap_total"], 300.0, places=9)
            self.assertEqual(summary["skipped_restrikes"], 1.0)
            self.assertEqual(summary["skipped_swaps"], 1.0)
            self.assertEqual(summary["n_explain_components"], len(EXPLAIN_FLOW_COLUMNS))
            self.assertEqual(summary["explain_unattributed"], sum(UNATTRIBUTED))
            self.assertAlmostEqual(summary["max_explain_gap"], 0.0, places=9)

    def test_every_comparison_series_reaches_a_panel(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            write_track(track)
            _meta, df = renderer.read_track(track)

            fig, panels = renderer.build_figure(META, df)
            try:
                # One entry per rendered comparison panel, keyed by the columns it
                # draws — this is the renderer's contract with the signal names the
                # strategy and the example froze.
                self.assertEqual(
                    set(panels),
                    {"pnl", "explain", "vega", "delta", "gamma", "theta"},
                )
                self.assertEqual(panels["vega"], ("gross_vega", "swap_vega"))
                self.assertEqual(panels["delta"], ("gross_delta", "swap_delta"))
                self.assertEqual(panels["gamma"], ("gross_gamma", "swap_gamma"))
                self.assertEqual(panels["theta"], ("gross_theta", "swap_theta"))
                self.assertEqual(panels["pnl"], ("strangle_cum", "swap_cum"))
                self.assertEqual(panels["explain"], tuple(EXPLAIN_FLOW_COLUMNS))
            finally:
                renderer.close_figure(fig)

    def test_a_track_without_the_explain_draws_no_attribution_panel(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            write_track(track, with_explain=False)
            _meta, df = renderer.read_track(track)

            fig, panels = renderer.build_figure(META, df)
            try:
                # An opt-in the run did not take draws NOTHING. A panel of flat
                # zeros would be a claim the run never made.
                self.assertNotIn("explain", panels)
                self.assertEqual(set(panels), {"pnl", "vega", "delta", "gamma", "theta"})
            finally:
                renderer.close_figure(fig)

    def test_renders_a_self_contained_html_page(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "report.html"
            write_track(track)

            renderer.render(track, out)

            html = out.read_text(encoding="utf-8")
            self.assertIn("<html", html.lower())
            # Self-contained: the figure is inlined, never a sidecar reference.
            self.assertIn("data:image/png;base64,", html)
            self.assertIn("XOM", html)

    def test_renders_an_options_only_track(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "report.png"
            write_track(track, with_swap=False)

            summary = renderer.render(track, out)

            self.assertEqual(out.read_bytes()[:8], PNG_MAGIC)
            self.assertEqual(summary["n_swap_live_rows"], 0)
            self.assertEqual(summary["swap_total"], 0.0)
            # No swap lane at all, so no attribution to report — and NaN, not
            # 0.0, is what "nothing was measured" reads as in the summary.
            self.assertEqual(summary["n_explain_components"], 0.0)
            self.assertTrue(math.isnan(summary["explain_unattributed"]))
            self.assertTrue(math.isnan(summary["max_explain_gap"]))


class CommandLineTests(unittest.TestCase):
    def test_cli_writes_the_report_it_is_given(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "cli.png"
            write_track(track)

            proc = subprocess.run(
                [sys.executable, str(_TOOL), str(track), str(out)],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(out.read_bytes()[:8], PNG_MAGIC)

    def test_cli_reports_usage_without_arguments(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(_TOOL)], capture_output=True, text=True, check=False
        )

        self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
