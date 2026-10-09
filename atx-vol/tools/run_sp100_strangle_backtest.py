"""Run the SP100 projection-strangle backtest from a SurfaceDb and report it.

WHAT THIS COMPOSES. The pipeline is entirely the library's; this file is the
operator's end of it and holds no economics of its own:

    SurfaceDb.open
      -> Clock.from_surface_db(db).between(--from, --to)
      -> StrategySpec.session_ts   (the window's snapshot timestamps, ascending)
      -> make_dispersion_strangle_spec(DispersionStrangleConfig)
      -> DeclarativeStrategy
      -> run_backtest(clock, strategy, RunConfig)
      -> tearsheet / write_backtest_pnl_tsv
      -> atxvol.report.dispersion.build_report

Long constituent strangles against a short index strangle, sized vega-flat at
entry, delta-hedged daily at the close, held to a synthetic expiry that is
SNAPPED onto the run's own session grid. Every number is replayed from the
db's cached American fitted surfaces; nothing is re-fit inside the run.

THREE CHOICES THAT ARE NOT ARBITRARY.

1. ``RunConfig.unpriced = EXCLUDE_AND_REPORT`` is required, not a preference.
   A real corpus has one-session provider gaps, and under the engine's
   fail-closed default (``ERROR``) the first such gap aborts the whole run. The
   excluded lots are not swept under the rug: their four counting lanes
   (held-PnL, hedge shares, hedge-skip, settlement-deferral) land in
   ``n_unpriced_lots``, which this driver prints and puts in the track header.

2. ``snap_expiry_to_sessions`` needs ``session_ts``, and the builder
   deliberately never invents a calendar — so this file fills it, from the SAME
   ``Clock.between`` window the run walks. A grid taken from the whole clock
   would contain instants the run never visits, and the snap would anchor
   expiries onto sessions that are not in the run.

   COST NOTE. ``SnapshotRef`` carries only ``date`` and ``archive_path`` and
   ``DbPartitionInfo.created_ts_ns`` is when the partition was WRITTEN, not the
   market instant it observes. ``SurfaceDb.session_ts`` reads that instant from
   the first directory record's fixed-size header, without reconstructing a
   surface. ``MarketSnapshot`` later rejects a partition whose surfaces do not
   all agree on that timestamp, so the fast pre-pass and engine share one value.

3. The sources are PINNED to this worktree before ``atxvol`` is imported. The
   development box carries a scikit-build-core editable install whose
   ``sys.meta_path`` finder outranks ``sys.path``, so an unpinned import
   silently runs a DIFFERENT checkout's Python layer and reports its numbers as
   this tree's. The preamble below is ``python/tests/_ctest_pytest_driver.py``'s,
   minus the pytest-specific parts: strip the finder, put this tree's
   ``python/src`` first, then VERIFY. Nothing is repaired silently.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import os
import sys
import time
from typing import Iterable, Sequence

# ── Source pinning. Must run before `import atxvol`. ────────────────────────

_HERE = os.path.dirname(os.path.abspath(__file__))
_PY_SRC = os.path.join(os.path.dirname(_HERE), "python", "src")

sys.meta_path[:] = [f for f in sys.meta_path if "ScikitBuild" not in type(f).__name__]
if _PY_SRC not in sys.path:
    sys.path.insert(0, _PY_SRC)

import atxvol as av  # noqa: E402
from atxvol.report import dispersion as report_dispersion  # noqa: E402


def _is_inside(path: str | None, root: str) -> bool:
    """Path CONTAINMENT, not a string prefix: ``<root>-other`` is not ``<root>``."""
    if not path:
        return False
    try:
        root = os.path.abspath(root)
        return os.path.commonpath([os.path.abspath(path), root]) == root
    except ValueError:  # different drives on Windows
        return False


def _resolution_complaint() -> str:
    """Empty when both halves of ``atxvol`` resolve inside this worktree."""
    for name, path in (("atxvol", av.__file__), ("atxvol._core", av._core.__file__)):
        if not _is_inside(path, _PY_SRC):
            return (
                f"{name} resolved to {path}, which is OUTSIDE this tree's {_PY_SRC}. "
                "An editable install is shadowing the sources under test; its "
                "meta-path finder outranks sys.path. Refusing to run: numbers from "
                "another checkout reported as this one's are worse than no numbers."
            )
    return ""


_CONTAMINATION = _resolution_complaint()

# The zero-friction key of `atxvol.report.dispersion.REGIMES`, checked against
# the renderer at import. The check is not ceremony: a track with NO regime is
# refused outright, but an UNRECOGNISED one is rendered on the neutral "unknown"
# tone with the raw string as its badge — so a typo here, or a rename on the
# renderer's side, would ship a grey banner rather than fail. This makes it an
# import-time error instead.
FRICTIONLESS = "frictionless"
if FRICTIONLESS not in report_dispersion.REGIMES:  # pragma: no cover - tripwire
    raise RuntimeError(
        f"atxvol.report.dispersion.REGIMES no longer carries {FRICTIONLESS!r} "
        f"(it has {sorted(report_dispersion.REGIMES)}). This run IS frictionless; "
        "pick the renamed zero-friction key rather than letting the report fall "
        "back to its unrecognised-regime tone."
    )
FRICTION_DETAIL = (
    "mid fills — no spread, no commission, no impact, no hedge slippage "
    "(engine-default FrictionModel; every field is 0)"
)

# The universe file's TAB-delimited header (data/universe/sp100_2026-07.csv).
UNIVERSE_COLUMNS = ("effective_date", "symbol", "raw_weight", "source", "as_of")

# `dispersion_strangle.cpp`'s `kCalendarDaysPerYear`. Used ONLY to look the
# corpus's rate up at the run's tenor for the report; the engine does its own
# tenor_days -> T conversion and never reads this.
DAYS_PER_YEAR = 365.25

TRACK_NAME = "track.tsv"
TEARSHEET_NAME = "tearsheet.tsv"
REPORT_NAME = "report.html"

EXIT_OK, EXIT_ENGINE, EXIT_USAGE = 0, 1, 2

# ── The report's masthead ───────────────────────────────────────────────────
#
# `atxvol.report.dispersion` was written for the SPY listed-options proxy and
# still DEFAULTS to that masthead, which for this run would be false in three of
# its four clauses (straddles, a listed monthly expiry, and a roll — against this
# run's strangles, synthetic snapped expiry and hold-to-expiry), and would
# contradict the byline two lines below it. That renderer's entire design rule is
# that a reader must never meet a figure without the assumptions that produced
# it; a `<title>` is the one line a circulated HTML file is guaranteed to be read
# by. So the driver supplies its own, and `build_report` escapes all three.
REPORT_TITLE = "SP100 Projection Strangles vs SPY"
REPORT_EYEBROW = "atx-vol · surface replay · projection-priced dispersion strangles"


class UsageError(Exception):
    """A caller mistake — bad flags, an unreadable or malformed universe."""


def _check_iso_date(flag: str, value: str) -> None:
    """`--from 2026-1-6` is a typo, not an engine refusal.

    Without this the value falls through to ``Clock::between``, whose comparison
    is LEXICOGRAPHIC — so a malformed date is reported as
    ``date_lo 'not-a-date' > date_hi '2026-01-14'``, an exit-1 engine error whose
    message describes an ordering problem rather than the actual mistake.
    """
    # The EXTENDED form specifically, not everything `fromisoformat` accepts: on
    # 3.11+ that also parses the basic `20260106` and ISO week dates, which are
    # real dates but do not compare against a hyphenated partition key.
    extended = len(value) == 10 and value[4] == "-" and value[7] == "-"
    try:
        if not extended:
            raise ValueError(value)
        datetime.date.fromisoformat(value)
    except ValueError:
        raise UsageError(
            f"{flag} {value!r}: not an ISO date. Expected YYYY-MM-DD, zero-padded "
            "(the partition keys this is matched against are hyphenated ISO dates, "
            "and the match is a STRING comparison — so an unpadded or basic-format "
            "date silently orders wrong rather than failing to parse)."
        ) from None


def report_standfirst(cfg: av.DispersionStrangleConfig, n_names: int) -> str:
    """One paragraph describing THIS run, BUILT FROM the config that ran it.

    Derived rather than written out so it cannot become a second prose constant
    that drifts from the run the way the renderer's default did — change a knob
    and the sentence changes with it.
    """
    hedge = ("delta-hedged daily at the close"
             if cfg.hedge.kind == av.HedgeSpec.Kind.DELTA_TO_ZERO
             and cfg.hedge.cadence == av.HedgeSpec.Cadence.DAILY
             else f"hedged {cfg.hedge.kind.name.lower()} / {cfg.hedge.cadence.name.lower()}")
    holding = ("held to expiry, never rolled" if cfg.hold_to_expiry
               else f"closed {_g(cfg.close_dte_days)} days from expiry")
    expiry = f"a synthetic {_g(cfg.tenor_days)}-day tenor"
    if cfg.snap_expiry_to_sessions:
        expiry += " snapped onto the run's own session grid"
    return (
        f"Long {n_names} single-name {_g(cfg.target_abs_delta * 100.0)}-delta strangles "
        f"against a short {cfg.index_symbol} strangle, sized vega-flat at entry on served "
        f"American vegas to a ${_g(cfg.theta_per_name_daily)}/day theta budget per name. A "
        f"fresh clip is opened every session, {hedge}, and {holding}; each cohort expires on "
        f"{expiry}. Prices and Greeks are projected from cached atx-vol American fitted "
        "surfaces — no listed quote is read and no re-fit occurs inside the backtest."
    )


# ── Universe ────────────────────────────────────────────────────────────────

def _split_symbols(values: Iterable[str] | None) -> list[str]:
    """`--exclude A,B --exclude C` and `--exclude A,B,C` are the same request."""
    out: list[str] = []
    for value in values or ():
        out += [part for part in (chunk.strip() for chunk in value.split(",")) if part]
    return out


def read_universe(path: str, index_symbol: str, excluded: Sequence[str]) -> list[str]:
    """The constituent names, in file order, with the index and `excluded` removed.

    Symbols are compared through the ENGINE's own ``canonical_symbol`` so this
    file and the strike resolver cannot disagree about what "the same name"
    means — a lower-cased or space-padded row in a hand-edited universe must not
    become a second, silently-unresolvable member.

    A duplicate symbol is REJECTED rather than de-duplicated: it doubles that
    name's weight in a vega-flat basket, so silently collapsing it would change
    the economics of the run without changing anything the operator can see.
    """
    if not os.path.isfile(path):
        raise UsageError(f"--universe {path}: no such file")
    with open(path, newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    if not rows:
        raise UsageError(f"--universe {path}: no rows")
    missing = [column for column in UNIVERSE_COLUMNS if column not in rows[0]]
    if missing:
        raise UsageError(
            f"--universe {path}: not the TAB-delimited universe format — missing "
            f"column(s) {', '.join(missing)}. Expected a header of "
            + "\t".join(UNIVERSE_COLUMNS)
        )

    index = av.canonical_symbol(index_symbol)
    drop = {av.canonical_symbol(symbol) for symbol in excluded}

    seen: set[str] = set()
    names: list[str] = []
    for row in rows:
        symbol = av.canonical_symbol(row["symbol"] or "")
        if not symbol:
            raise UsageError(f"--universe {path}: a row has an empty symbol")
        if symbol in seen:
            raise UsageError(f"--universe {path}: duplicate symbol {symbol}")
        seen.add(symbol)
        if symbol == index or symbol in drop:
            continue
        names.append(symbol)
    if not names:
        raise UsageError(
            f"--universe {path}: every symbol was removed by --index/--exclude"
        )
    return names


def universe_digest(names: Sequence[str]) -> str:
    """Content identity of the RESOLVED name list (order included).

    Hashing the resolved list rather than the file bytes is deliberate: two runs
    off the same file with different `--exclude` sets are different universes and
    must not share an identity, and the same names reached through a re-exported
    file must not look like a different one.
    """
    return hashlib.sha256("\n".join(names).encode("utf-8")).hexdigest()


# ── Session grid ────────────────────────────────────────────────────────────

def _probe_surface(db, date: str, probe_symbols: Sequence[str]):
    """Map the first of `probe_symbols` that this partition actually carries.

    The reporting rate may come from any present universe surface. Mapping keeps
    that one-off metadata read zero-copy instead of reconstructing an owned
    surface that the backtest has not started consuming yet.
    """
    for symbol in probe_symbols:
        try:
            return db.map_surface(date, symbol)
        except av.AtxError:
            continue
    raise UsageError(
        f"partition {date}: none of the universe's symbols is in it, so its "
        "reporting rate cannot be read. The db and the universe do not "
        "describe the same corpus."
    )


def session_timestamps(db, clock) -> list[int]:
    """The window's snapshot instants, ascending — see the module note (2).

    One entry per ref of the clock HANDED IN, so the grid is the run's own
    window; `sorted(set(...))` because `resolve_spec` hard-rejects an unsorted
    grid and the snap's `upper_bound` would otherwise return a wrong anchor as a
    success.
    """
    stamps = [int(db.session_ts(ref.date)) for ref in clock.refs]
    ordered = sorted(set(stamps))
    if ordered != stamps:
        # Not fatal — resolution only requires ascending — but a corpus whose
        # partition-key order disagrees with its market instants is an anomaly
        # the operator should hear about rather than discover in the expiries.
        print(
            "warning: the corpus's session timestamps are not in partition-key "
            "order (or repeat); the snap grid is the sorted, de-duplicated set.",
            file=sys.stderr,
        )
    return ordered


def corpus_rate(db, clock, probe_symbols: Sequence[str], at_T: float) -> float:
    """The corpus's own discount rate at the run's tenor — a REPORTING value.

    The engine reads rates off the surfaces itself and never consults this. It
    exists because the renderer's colophon falls back to the SPY run's
    hard-coded 0.043 when the metadata omits the key, and printing another run's
    rate as this one's is the class of thing the regime banner exists to prevent.
    Kept out of `session_timestamps` (whose name promises one thing). The
    timestamp scan intentionally does not warm the partition cache; this maps a
    first-session surface zero-copy, avoiding the old owned reconstruction that
    cost about 20 ms only to read one reporting value.
    """
    return float(_probe_surface(db, clock.refs[0].date, probe_symbols).rate_at(at_T))


# ── The run ─────────────────────────────────────────────────────────────────

def strangle_config(names: Sequence[str], args) -> av.DispersionStrangleConfig:
    cfg = av.DispersionStrangleConfig()
    cfg.names = list(names)
    cfg.index_symbol = av.canonical_symbol(args.index)
    cfg.target_abs_delta = args.delta
    cfg.tenor_days = args.tenor_days
    cfg.entry_every_n_days = 1
    cfg.theta_per_name_daily = args.theta_per_name
    cfg.hold_to_expiry = True
    cfg.snap_expiry_to_sessions = True
    # Defensive under DROP_RENORMALIZE (an index-missing entry day is already a
    # no-trade step there), and load-bearing the moment the policy is tightened.
    cfg.skip_entry_on_missing_index = True
    cfg.hedge = av.HedgeSpec(av.HedgeSpec.Kind.DELTA_TO_ZERO,
                             av.HedgeSpec.Cadence.DAILY, args.hedge_band)
    # The floor is the ENGINE's default, read off a fresh config rather than
    # re-declared here: a Python-side constant would be a second source of truth
    # that drifts the first time `dispersion_strangle.hpp` moves it.
    cfg.missing = av.MissingNameSpec(av.MissingNamePolicy.DROP_RENORMALIZE,
                                     av.DispersionStrangleConfig().missing.min_names)
    return cfg


# Pinned at 1, not the library default 2 — a recorded track-header value two
# tests assert (test_run_sp100_strangle_backtest.py:276,510); post-v1 alignment candidate.
def run_config(prefetch_depth: int = 1) -> av.RunConfig:
    cfg = av.RunConfig()
    # See the module note (1): required, not a preference.
    cfg.unpriced = av.UnpricedLotPolicy.EXCLUDE_AND_REPORT
    cfg.record_every_n = 1
    cfg.prefetch_depth = prefetch_depth
    # `snapshot_cache` is left null on purpose: the engine then builds a PRIVATE
    # bounded cache sized off the prefetch depth. An explicit `SnapshotCache()`
    # is unbounded and would retain every session's whole board.
    return cfg


FRICTION_FIELDS = ("half_spread_bps", "vol_tick", "impact_fraction",
                   "per_contract_cost", "hedge_slippage_bps")


def regime_of(frictions: av.FrictionModel) -> tuple[str, str]:
    """`(friction_regime, friction_detail)` DERIVED from the model that priced.

    This must not be a constant. The regime drives the report's colour-coded
    banner, the caption on every headline tile and the P&L chart title — it is
    the repo's designated defence against publishing a number without the
    execution assumptions that produced it. A hand-written "frictionless" is
    only as good as the literal: it cannot disagree with the code even when the
    code changes, so the defence would silently stop defending the day someone
    configured a spread.

    A NON-zero model is refused rather than guessed at. This driver has no flag
    for frictions and therefore no vocabulary for describing them; mapping an
    arbitrary model onto `frictioned` / `frictioned+impact` would be inventing
    the very label the banner exists to make trustworthy.
    """
    nonzero = {name: getattr(frictions, name) for name in FRICTION_FIELDS
               if float(getattr(frictions, name)) != 0.0}
    if nonzero or frictions.spread_kind != av.FrictionModel.SpreadKind.NONE:
        raise UsageError(
            "the run's FrictionModel is not zero "
            f"(spread_kind={frictions.spread_kind.name}, {nonzero}), but this driver "
            f"can only label a {FRICTIONLESS} run. Extend it with the flags and the "
            "regime mapping for the frictions you want before reporting them."
        )
    return FRICTIONLESS, FRICTION_DETAIL


# ── Artifacts ───────────────────────────────────────────────────────────────

def _g(value: float) -> str:
    return f"{float(value):.10g}"


def _g17(value: float) -> str:
    """%.17g — the engine's own TSV precision, so a value round-trips exactly."""
    return f"{float(value):.17g}"


def _mean_abs_net_vega_at_entry(result) -> float:
    """Mean |net book vega| over the steps that actually carry a book.

    ``gross_vega`` is the SIGNED net across both groups (``gross_vega_abs`` is
    the two-sided gross), so this is the residual the vega-flat constraint left
    behind. Steps with no open lot are excluded rather than averaged in as
    zeros: with `entry_every_n_days=1` every step is an entry attempt, and a
    no-trade step (a missing index, a fully-dropped basket) is an absence of a
    measurement, not a measurement of zero.
    """
    lots = result.n_open_lots
    vega = result.gross_vega
    live = [abs(float(v)) for v, n in zip(vega, lots) if float(n) > 0.0]
    return sum(live) / len(live) if live else 0.0


def track_meta(args, names, dates, cfg, run_cfg, sheet, result,
               rate: float) -> dict[str, str]:
    """The `# key=value` head of `track.tsv`.

    Everything needed to reproduce the run is in it — the regime FIRST (the
    report refuses to render without it), then the universe identity, the
    resolved window, and every knob. Nothing wall-clock-derived appears here:
    two identical invocations must produce a byte-identical track, and a
    generation timestamp would break that for no benefit the run archive does
    not already provide.

    EVERY VALUE IS AN ECHO OF AN OBJECT THAT RAN — `cfg` for the strategy knobs,
    `run_cfg` for the run-level ones, `result`/`sheet` for the outcomes. None of
    them is a literal restating what the code above is supposed to have done. A
    hand-written constant cannot disagree with the code even when the code
    changes, so the header — whose whole stated purpose is that the run can be
    reproduced from it — would keep asserting the old truth and any test reading
    it would keep passing. That is not hypothetical: `unpriced_policy`,
    `record_every_n` and `friction_regime` WERE such literals, and mutating the
    run to `UnpricedLotPolicy.ERROR` left the header still saying
    `EXCLUDE_AND_REPORT` with the whole suite green.

    The keys `date_lo`/`date_hi`/`delta_band`/`min_names`/`target_dte_days`/
    `roll_dte_days`/`gross_index_vega`/`opra_root`/`flat_rate` are ALSO read back
    by `atxvol.report.dispersion._render`, which otherwise falls back to the SPY
    run's constants. They are supplied so the report's byline describes THIS run
    rather than that one.
    """
    hedge = cfg.hedge
    regime, regime_detail = regime_of(run_cfg.frictions)
    return {
        "friction_regime": regime,
        "friction_detail": regime_detail,
        # `build_report` takes the run's label from `strategy`.
        "strategy": args.label,
        "label": args.label,
        "index_symbol": cfg.index_symbol,
        "n_names": str(len(names)),
        # Canonical, so the header records what was actually dropped rather than
        # however the operator happened to spell it.
        "excluded": ",".join(av.canonical_symbol(symbol)
                             for symbol in _split_symbols(args.exclude)),
        "universe": os.path.basename(args.universe),
        "universe_sha256": universe_digest(names),
        "opra_root": os.path.basename(os.path.normpath(args.db)),
        "requested_from": args.date_from,
        "requested_to": args.date_to,
        "date_lo": dates[0] if dates else "",
        "date_hi": dates[-1] if dates else "",
        "n_sessions": str(len(dates)),
        "target_abs_delta": _g(cfg.target_abs_delta),
        "tenor_days": _g(cfg.tenor_days),
        "close_dte_days": _g(cfg.close_dte_days),
        "entry_every_n_days": str(cfg.entry_every_n_days),
        "theta_per_name_daily": _g(cfg.theta_per_name_daily),
        "gross_index_vega": _g(cfg.index_base_vega),
        "hedge_kind": hedge.kind.name,
        "hedge_cadence": hedge.cadence.name,
        "hedge_band": _g(hedge.band),
        "delta_band": _g(hedge.band),
        "hold_to_expiry": str(cfg.hold_to_expiry),
        "snap_expiry_to_sessions": str(cfg.snap_expiry_to_sessions),
        "skip_entry_on_missing_index": str(cfg.skip_entry_on_missing_index),
        "missing_policy": cfg.missing.policy.name,
        "min_names": str(cfg.missing.min_names),
        # The report's masthead reads these; a synthetic snapped tenor has no
        # listed DTE band and is never rolled, so the band is the tenor itself.
        "target_dte_days": _g(cfg.tenor_days),
        "min_dte_days": _g(cfg.tenor_days),
        "max_dte_days": _g(cfg.tenor_days),
        "roll_dte_days": "0",
        # The corpus's own rate at the run's tenor, so the colophon does not fall
        # back to the SPY run's constant. `min_weight_coverage` is deliberately
        # ABSENT: sizing here is a per-name daily theta budget, not a
        # weight-coverage rule, and the renderer now omits the clause rather than
        # printing an unfilled "≥ ?".
        "flat_rate": _g(rate),
        "unpriced_policy": run_cfg.unpriced.name,
        "record_every_n": str(run_cfg.record_every_n),
        "prefetch_depth": str(run_cfg.prefetch_depth),
        "n_unpriced_lots_max": _g(max(result.n_unpriced_lots, default=0.0)),
        "n_unpriced_greeks_max": _g(max(result.n_unpriced_greeks, default=0.0)),
        "final_nav": _g17(result.nav[-1]) if len(result) else "0",
        "total_return": _g17(sheet.total_return),
    }


def write_tearsheet_tsv(path: str, sheet, result, names, dates, timing,
                        ledger_delta) -> int:
    """`key<TAB>value`, the TearSheet fold first, then the run-level facts.

    ``final_nav`` is NOT a TearSheet field — the fold reports `total_return`,
    which is the cumulative flow sum and equals the last NAV by construction —
    but it is the number an operator looks for first, so it is written
    explicitly from the series rather than left to be inferred.
    """
    rows = [(key, _g17(value)) for key, value in sheet.to_dict().items()]
    rows += [
        ("final_nav", _g17(result.nav[-1]) if len(result) else "0"),
        ("n_sessions", str(len(dates))),
        ("n_names", str(len(names))),
        ("date_lo", dates[0] if dates else ""),
        ("date_hi", dates[-1] if dates else ""),
        ("mean_abs_net_vega_at_entry", _g17(_mean_abs_net_vega_at_entry(result))),
        ("n_unpriced_lots_max", _g17(max(result.n_unpriced_lots, default=0.0))),
        ("n_unpriced_greeks_max", _g17(max(result.n_unpriced_greeks, default=0.0))),
        # Timing lives HERE and in the stdout summary, never in track.tsv: the
        # track is gated byte-identical across identical invocations, and a
        # wall-clock stamp would break that contract on every run.
        ("backtest_wall_s", _g17(timing["wall_s"])),
        ("backtest_cpu_s", _g17(timing["cpu_s"])),
    ]
    rows += [(key, str(int(value))) for key, value in ledger_delta.items()]
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        for key, value in rows:
            handle.write(f"{key}\t{value}\n")
    return len(rows)


# ── CLI ─────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="run_sp100_strangle_backtest.py",
        description="Run the SP100 projection-strangle backtest from a SurfaceDb "
                    "and write track.tsv, tearsheet.tsv and report.html.",
    )
    parser.add_argument("--db", required=True, help="SurfaceDb root")
    parser.add_argument("--universe", required=True,
                        help="TAB-delimited universe csv (see data/universe/)")
    # `from` is a keyword, so the destination cannot be derived from the flag.
    parser.add_argument("--from", dest="date_from", required=True, metavar="YYYY-MM-DD")
    parser.add_argument("--to", dest="date_to", required=True, metavar="YYYY-MM-DD")
    parser.add_argument("--out", required=True, help="output directory (created if absent)")
    parser.add_argument("--exclude", action="append", metavar="SYM[,SYM...]",
                        help="symbols to drop from the universe (case-insensitive; "
                             "repeatable and/or comma-separated)")
    parser.add_argument("--index", default="SPY", help="dispersion index symbol")
    parser.add_argument("--delta", type=float, default=0.40,
                        help="target |delta| per strangle wing")
    parser.add_argument("--tenor-days", type=float, default=90.0,
                        help="synthetic tenor, snapped onto the run's sessions")
    parser.add_argument("--theta-per-name", type=float, default=10.0,
                        help="daily theta budget per constituent name")
    parser.add_argument("--hedge-band", type=float, default=0.0,
                        help="delta band below which the daily hedge does nothing")
    # Pinned at 1, not the library default 2 — a recorded track-header value two
    # tests assert (test_run_sp100_strangle_backtest.py:276,510); post-v1 alignment candidate.
    parser.add_argument("--prefetch-depth", type=int, default=1,
                        help="number of future snapshots allowed in flight (default: 1)")
    parser.add_argument("--label", default="sp100-projection-strangle",
                        help="run label, carried into the track header and the report")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    try:
        args = parser.parse_args(list(argv) if argv is not None else None)
    except SystemExit as exit_request:  # argparse already printed the diagnosis
        return int(exit_request.code or EXIT_OK)

    if _CONTAMINATION:
        # EXIT_USAGE, not EXIT_ENGINE: a wrong-checkout resolution is an
        # ENVIRONMENT fault, and it is exactly the failure an operator most needs
        # to tell apart from "the engine said no". Nothing was run, so reporting
        # it in the engine's bucket would invite reading it as a verdict.
        print(f"error: {_CONTAMINATION}", file=sys.stderr)
        return EXIT_USAGE

    try:
        for flag, value in (("--from", args.date_from), ("--to", args.date_to)):
            _check_iso_date(flag, value)
        if args.prefetch_depth < 1:
            raise UsageError("--prefetch-depth must be at least 1")
        names = read_universe(args.universe, args.index, _split_symbols(args.exclude))
    except UsageError as bad:
        print(f"error: {bad}", file=sys.stderr)
        return EXIT_USAGE

    index = av.canonical_symbol(args.index)
    print(f"universe: {len(names)} name(s) against {index}")

    try:
        db = av.SurfaceDb.open(args.db)
        clock = av.Clock.from_surface_db(db).between(args.date_from, args.date_to)
        sessions = session_timestamps(db, clock)
        rate = corpus_rate(db, clock, [index, *names], args.tenor_days / DAYS_PER_YEAR)

        cfg = strangle_config(names, args)
        spec = av.make_dispersion_strangle_spec(cfg)
        spec.session_ts = sessions
        strategy = av.DeclarativeStrategy(spec)

        run_cfg = run_config(args.prefetch_depth)
        # perf_counter is the wall clock; process_time is user+system CPU of the
        # WHOLE process across all threads — run_backtest releases the GIL and
        # fans out over the pricing executor in-process, so the delta captures
        # the engine's parallel work (cpu/wall ~ effective core count).
        av.reset_solve_ledger()
        wall_0 = time.perf_counter()
        cpu_0 = time.process_time()
        result = av.run_backtest(clock, strategy, run_cfg)
        wall_1 = time.perf_counter()
        cpu_1 = time.process_time()
        ledger_delta = av.solve_ledger()
        timing = {
            "wall_s": wall_1 - wall_0,
            "cpu_s": cpu_1 - cpu_0,
        }
        sheet = av.tearsheet(result)
        dates = list(result.date)
        meta = track_meta(args, names, dates, cfg, run_cfg, sheet, result, rate)

        # Created only once the run has succeeded, so a failed invocation does not
        # leave an empty output directory behind to be mistaken for a stale run.
        os.makedirs(args.out, exist_ok=True)
        track_path = os.path.join(args.out, TRACK_NAME)
        av.write_backtest_pnl_tsv(result, meta, track_path)

        tearsheet_path = os.path.join(args.out, TEARSHEET_NAME)
        n_metrics = write_tearsheet_tsv(tearsheet_path, sheet, result, names, dates,
                                        timing, ledger_delta)

        report_path = os.path.join(args.out, REPORT_NAME)
        report_dispersion.build_report(
            result, sheet, meta, report_path,
            title=REPORT_TITLE, eyebrow=REPORT_EYEBROW,
            standfirst=report_standfirst(cfg, len(names)),
        )
    except UsageError as bad:
        print(f"error: {bad}", file=sys.stderr)
        return EXIT_USAGE
    except (av.AtxError, OSError, ValueError) as failure:
        print(f"error: {failure}", file=sys.stderr)
        return EXIT_ENGINE

    print(f"wrote {TRACK_NAME}      {track_path}  ({len(result)} session rows, "
          f"{len(meta)} meta keys)")
    print(f"wrote {TEARSHEET_NAME}  {tearsheet_path}  ({n_metrics} metrics)")
    print(f"wrote {REPORT_NAME}    {report_path}  "
          f"({os.path.getsize(report_path)} bytes, regime {meta['friction_regime']})")

    final_nav = float(result.nav[-1]) if len(result) else 0.0
    print(f"  sessions              {len(dates)}  "
          f"[{meta['date_lo']} .. {meta['date_hi']}]")
    print(f"  names                 {len(names)}  (sha256 {meta['universe_sha256'][:16]})")
    print(f"  final NAV             {final_nav:,.2f}")
    print(f"  total PnL             {sheet.total_return:,.2f}")
    print(f"  max drawdown          {sheet.max_drawdown:,.2f}")
    print(f"  mean |net vega| at entry  {_mean_abs_net_vega_at_entry(result):,.2f}")
    print(f"  unpriced lots (max/step)  {max(result.n_unpriced_lots, default=0.0):.0f}"
          f"   unpriced greeks (max/step) "
          f"{max(result.n_unpriced_greeks, default=0.0):.0f}")
    print(f"  backtest time         {timing['wall_s']:,.2f} s wall / "
          f"{timing['cpu_s']:,.2f} s cpu")
    print("  solve ledger")
    for key, value in ledger_delta.items():
        print(f"    {key:<30} {value:,}")
    return EXIT_OK


if __name__ == "__main__":  # pragma: no cover - CLI entry point
    raise SystemExit(main())
