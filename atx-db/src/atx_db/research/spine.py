"""Price wave inputs (tier-1 v2 node 1.12): cleaned session bars, line types, the EW market, the monthly spine.

Everything here is read straight from the retained vendor file ``TickerHistory3.parquet``
(ruling R-5: research workers never open the warehouse) and written as Parquet under a work
directory (``data/research/work/price_wave/<run>/``); the monthly spine is then exported to
the research lake as ``spine_monthly`` (JKP layout). Each stage is one bounded job (DuckDB
256MB / 1 thread, capped by ``research.workers.run_jobs``) and writes a JSON receipt with its
code digest and the sha256 of every file it wrote; a stage whose receipt matches is skipped.

Bars (``stage_bars``, one job per line bucket ``securityID % K``)
    * Vendor lines with a positive ``securityID`` only (``line_id`` =
      ``TBLTICKERHISTORY-<securityID>``, the loader's id for them); rows with a NULL, zero or
      negative id are unidentified lines and are counted, never keyed by symbol
      (:func:`unidentified_rows_stats` labels each one's exclusion reason, ruling C-57).
    * Sessions: only bars dated on an XNYS rule session (``calendar.xnys_sessions``); a bar on
      a closure or a weekend is a stray bar, counted and dropped. ``sno`` numbers the rule
      sessions from :data:`SESSION_ORIGIN`.
    * Row validity: a non-empty symbol, a positive finite close, a non-negative volume, and
      ``adjusted_close = close x cumulReturnFactor`` when finite and positive
      (``ticker_history_bulk._RAW_CTE``). Deviation from the bulk publisher, which also drops a
      row whose OHLC range is inconsistent: such a row is kept with ``ohlc_ok = false`` and NULL
      ``open/high/low`` (only the spread natives read the range; they skip the pair); counted in
      the receipt. In the retained file (272,374 rows; 1.12 review m1) 71 % of them have only
      the open outside [low, high], 17.5 % a missing or zero open/high/low and 11.4 % the close
      outside the range (median excess 0.2 %). About 96k sit on 21 sessions (mostly the day
      before an exchange holiday, 2016-01 .. 2018-02, and 2025-05-20, where the vendor's open is
      the prior close on up to half of the lines); the rest are spread over 3,621 sessions.
      Dropping them would manufacture missing sessions (a broken return chain, an empty
      formation or label session). The bulk publisher drops them, so final labels (3.8) would
      miss sessions these provisional labels keep: the alignment is routed to 3.2/3.8.
    * Duplicate vendor keys (1.9 C2): one whole row per ``(line, session)`` by the
      publisher's total order (``_vendor_artifact.bar_pick_order_sql(with_shares=True)``,
      then volume: node 0.13's I4 pick, the same terms as ``market_daily``), then high, low,
      open, ``dn`` and the current symbol (these only split rows that tie on every
      publisher term). Keys with more than one row are counted (and those whose rows differ).
    * Vendor shares (A9 unit rule, ``migrations.bodies_0327``): the parquet format means
      thousands (``ticker_history_bulk.SHARES_UNIT_BY_FORMAT``) and every year's cross-section
      median must agree (``_median_verdict``), else the stage refuses to run; shares are then
      stored in units (x1000). A stored count above ``THOUSANDS_ROW_CEILING`` would exceed
      any real share count after x1000 (a row already in units): it is withheld (NULL) and
      counted, never guessed.
    * VA1 v2 (``_vendor_artifact.repaired_bars_sql`` with the same-bar share veto) over the
      line's whole history: ``adj = adjusted_close x va_multiplier``. Rows without a valid
      adjusted close are dropped (counted).
    * Vendor artifact flag (ruling C-79; VA1 v2 repairs factor *decreases* only). ``va_suspect``
      labels a bar ``factor_step_artifact_session`` when, on :data:`VA_ARTIFACT_SESSION`, its
      repaired factor ``adj / close`` is at least :data:`VA_FACTOR_STEP_MIN` times the line's
      previous bar's (the ruling's rule, whatever the close did); ``factor_step_close_flat``
      when, on any session, the factor is at least :data:`VA_FLAT_FACTOR_STEP_MIN` times the
      previous bar's (at most :data:`VA_FLAT_MAX_GAP_SESSIONS` sessions back) while the raw close
      stays within +-:data:`VA_FLAT_CLOSE_BAND` of the previous close (unlike the inverse raw-close
      move expected from a split alone; the adjusted return is then >= +50 % while the raw move
      is bounded by 25 %). This is an input-quality exclusion, not a verified correction: an
      offsetting genuine price move around a corporate action can also satisfy it. It reads
      only the current and preceding bars, never a later return. ``sentinel_close`` applies when
      its close is at least
      :data:`VA_SENTINEL_CLOSE_MIN` and more than :data:`VA_SENTINEL_RATIO` times the line's
      nearest *prior* close below that floor (a run retains the same prior anchor). No subsequent
      close is consulted. High closes without such a prior anchor are explicitly labeled
      ``va_sentinel_unanchored`` and counted as ambiguous, not inferred to be corrupt: this
      includes genuinely high-priced lines whose whole retained history is above the floor.
      The close-flat rule extends
      the ruling's x50 rule (sized in the fix round: on 2021-01-04, 73 more bars with a factor
      step of x2 .. x50 and a flat close, adjusted returns +200 % .. +3,800 %; 27 bars on other
      sessions, two of them x100). ``va_brk_in`` counts the
      breaks a bar introduces (1 at a flagged bar, +1 at the bar after a sentinel) and ``va_brk``
      is their running sum per line: a price ratio between two bars is clean only when both
      carry the same ``va_brk``. Downstream (never filled, always counted): ``r``/``lr`` are NULL
      at a bar with ``va_brk_in > 0``; a label whose entry and exit bars differ in ``va_brk`` is
      ``invalid``; a native whose window holds a break is NULL with status
      ``vendor_artifact_suspect``; spine columns that would span one are NULL with a
      ``va_suspect_*`` flag. The warehouse-level VA1 extension to increase-side artifacts on any
      session is CARRY 3.2 (``_vendor_artifact`` is node 0.13's and is not edited here).
    * ``r`` / ``lr``: the simple / log return between two *consecutive* rule sessions both
      observed by the line (a return across a gap is not daily and is NULL).

Lines (``stage_lines``): first/last session, current and historical symbols, the first bar with
vendor earnings evidence, and the line's security type (see :func:`classify_lines`).

Spine (``stage_spine``, one job per year; ``spine_monthly``): one row per (calendar month end
``eom``, line) for every line of an eligible type (:data:`ELIGIBLE_SECURITY_TYPES`) with at
least one observed bar in the month and vendor earnings evidence dated at or before the
formation session (ruling C-78: one point-in-time rule for listed and delisted lines; see
:func:`classify_lines`). The formation session is the month's last rule session
(``calendar.expected_month_end_session``), known at its 22:00 UTC. Columns: ``price`` (close
at the formation session; NULL when the line has no bar on it), ``shares_lagged`` (A8
domestic modeled lag, ``me_basis='vendor_shares_lag90'``: the vendor count of the line's last
bar dated at or before the formation session minus :data:`SHARES_LAG_DAYS` calendar days and
at most :data:`SHARES_MAX_AGE_DAYS` older, restated to the formation session's share basis by
the ratio of the repaired vendor factors ``adj/close``, which carries splits and, by a few
tenths of a percent, dividends), ``me_line`` = price x shares_lagged, ``ret_1m`` (the
formation month's return, repaired adjusted close), ``dollar_volume_21d`` and
``n_sessions_21d`` (observed sessions among the last 21), ``size_grp`` (JKP size groups on
the Fama-French NYSE ME breakpoints of the month: mega >= p80, large >= p50, small >= p20,
micro below; the JKP nano split needs the NYSE p1, which French does not publish, so micro
includes nano: ``size_basis``). ``me_line`` and ``size_grp`` are NULL at the formations
2012-04 .. 2012-06: their share lag date falls before the file's first bar (1.12 review m7).
``va_suspect_price`` / ``_shares`` / ``_ret_1m`` / ``_liq`` flag the rows whose price (a
sentinel formation bar), lagged share count (restated across a break), ``ret_1m`` or 21-session
dollar volume (a sentinel in the window) were withheld (NULL) under ruling C-79.

EW market (``stage_market``): the P3 sealed convention (``research.events``): per session, the
equal-weighted mean of the daily bar returns (VA1 v2 repaired adjusted close) of the lines in
the spine of the latest formation strictly before the session (a name leaves after its last
bar), published when at least :data:`MARKET_MIN_NAMES` lines have a return. No terminal
returns. Ruling C-55: each bar return is winsorized at +-:data:`MARKET_CLIP` before the mean
(``m``, the market every native reads); the unclipped mean (``m_unclipped``) is kept and
reported beside it.

Line months (``stage_line_month``): per (line, month) the last observed session and its
repaired adjusted close, observed sessions and daily-return counts, and the sums of the
overlapping 3-session log returns of the line and the EW market (Frazzini-Pedersen
correlation input), for the month-based natives of :mod:`atx_db.research.price_natives`.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import inspect
import json
import os
import re
import time
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

from .. import _vendor_artifact
from .. import calendar as _xnys
from .research_lake import canonical_json, connect_bounded, sha256_file, sql_text

SPINE_VERSION = "price-wave-spine-v1"
DEFAULT_TH3 = Path("C:/atx/atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet")
DEFAULT_ROOT = Path("C:/atx/atx-db/data/research")
#: Sessions are numbered from this date on (a stable ``sno`` across runs).
SESSION_ORIGIN = dt.date(2011, 1, 3)
SESSION_HORIZON = dt.date(2027, 12, 31)
#: Published spine formations (calendar month ends); the month before the first one is
#: built too (the EW market's universe for the first formation month).
FIRST_EOM = dt.date(2012, 4, 30)
LAST_EOM = dt.date(2026, 8, 31)
BASE_FIRST_EOM = dt.date(2012, 3, 31)
LINE_PREFIX = "TBLTICKERHISTORY-"
VENDOR_SOURCE = "TickerHistory3.parquet"
#: Ruling C-78: a line enters at formation F only on vendor earnings evidence dated at or before
#: F's session, listed and delisted alike; the directory only excludes listed non-commons.
UNIVERSE_BASIS = "pit_vendor_earnings_evidence_name_pattern_exclusion"
ME_BASIS = "vendor_shares_lag90"
#: A8 domestic modeled lag: a vendor share run is read 90 calendar days after its bar.
SHARES_LAG_DAYS = 90
#: A lagged count older than this (from the lag date) is stale: no ``shares_lagged``.
SHARES_MAX_AGE_DAYS = 400
SIZE_BASIS = "french_nyse_me_breakpoints_p20_p50_p80_micro_includes_nano"
#: The reconstructed research universe's eligible types (``fundamental_signal_research``
#: ``RECONSTRUCTED_ELIGIBLE_TYPES``): common stock with or without positive name evidence.
ELIGIBLE_SECURITY_TYPES = ("common", "common_unverified")
MARKET_MIN_NAMES = 100
#: Ruling C-55: each line's daily bar return is winsorized at +-50% before the equal-weight mean
#: (one bad vendor bar moved the unclipped warehouse EW mean to +433% on 2021-01-04); the
#: unclipped mean is kept beside it as a diagnostic and reported.
MARKET_CLIP = 0.5
MARKET_BASIS = "equal_weight_prior_formation_spine_lines_bar_returns_winsorized_50pct"
#: A session whose unclipped mean differs from the winsorized one by more than this is listed
#: in the market receipt (a vendor-bar data finding; never a real market move).
MARKET_GAP_REPORT = 0.01
#: Ruling C-79 (research-layer vendor artifact flag): the artifact session, the factor step that
#: flags a bar on it, and the sentinel-close rule (a close at least the floor and more than the
#: ratio times the line's nearest prior close below the floor; never a future close).
VA_ARTIFACT_SESSION = dt.date(2021, 1, 4)
VA_FACTOR_STEP_MIN = 50.0
#: The close-flat factor step (any session): a factor at least this many times the previous bar's
#: (at most VA_FLAT_MAX_GAP_SESSIONS back) with the raw close within +-VA_FLAT_CLOSE_BAND of it.
VA_FLAT_FACTOR_STEP_MIN = 2.0
VA_FLAT_CLOSE_BAND = 0.25
VA_FLAT_MAX_GAP_SESSIONS = 5
VA_SENTINEL_CLOSE_MIN = 1000.0
VA_SENTINEL_RATIO = 100.0
VA_FACTOR_STEP = "factor_step_artifact_session"
VA_FACTOR_FLAT = "factor_step_close_flat"
VA_SENTINEL = "sentinel_close"
DEFAULT_BUCKETS = 16

_SAFE = re.compile(r"^[A-Za-z0-9_.:/\\ -]+$")


# ---------------------------------------------------------------------------
# Work directory, receipts and digests
# ---------------------------------------------------------------------------

def work_dir(root: Path, run: str) -> Path:
    if not re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,63}", run):
        raise ValueError(f"run name {run!r} must be lower-case letters, digits, '_', '.', '-'")
    return Path(root) / "work" / "price_wave" / run


def source_digest(paths: Iterable[Path | str], *functions: Any) -> str:
    """sha256 over source files (LF-normalized) and function sources: a stage's code digest."""
    digest = hashlib.sha256()
    for item in paths:
        path = Path(item)
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    for function in functions:
        digest.update(inspect.getsource(function).replace("\r\n", "\n").encode("utf-8"))
    return digest.hexdigest()


def spine_code_digest() -> str:
    """Code digest of the whole input build: this module, the VA1 repair and the XNYS calendar."""
    return source_digest([Path(__file__), Path(_vendor_artifact.__file__), Path(_xnys.__file__)])


def _constants_digest() -> str:
    """The module's upper-case constants (types, bases, dates, limits): part of every stage digest."""
    names = sorted(name for name, value in globals().items()
                   if name.isupper() and isinstance(value, (str, int, float, tuple, dt.date, Path)))
    return hashlib.sha256(canonical_json({name: repr(globals()[name]) for name in names}).encode()).hexdigest()


def stage_digest(*functions: Any, files: Sequence[Path] = ()) -> str:
    """One stage's code digest: its functions, the shared session/formation helpers, the constants,
    the VA1 repair and the XNYS calendar (so an edit elsewhere in this module does not re-run it)."""
    helpers = (source_digest, connect, sessions, session_numbers, month_end, add_months, formation_months,
               formation_rows, stage_sessions_table, bars_files, base_files, parquet_list, year_formations,
               stage_forms_table)
    code = source_digest([Path(_vendor_artifact.__file__), Path(_xnys.__file__), *files], *helpers, *functions)
    return hashlib.sha256(f"{SPINE_VERSION}|{code}|{_constants_digest()}".encode()).hexdigest()


def write_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp")
    tmp.write_text(json.dumps(payload, indent=1, sort_keys=True, default=str) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def receipt_ok(path: Path, code: str, inputs: Mapping[str, Any] | None = None) -> bool:
    """A stage is done when its receipt exists with the same code digest and inputs, and its files hash."""
    if not path.is_file():
        return False
    receipt = read_json(path)
    if receipt.get("code_digest") != code or receipt.get("inputs") != json.loads(canonical_json(dict(inputs or {}))):
        return False
    for name, sha in receipt.get("files", {}).items():
        target = path.parent / name
        if not target.is_file() or sha256_file(target) != sha:
            return False
    return True


def finish_receipt(path: Path, code: str, inputs: Mapping[str, Any], files: Sequence[Path],
                   stats: Mapping[str, Any], started: float) -> dict[str, Any]:
    payload = {"version": SPINE_VERSION, "code_digest": code, "inputs": json.loads(canonical_json(dict(inputs))),
               "files": {f.relative_to(path.parent).as_posix(): sha256_file(f) for f in files},
               "stats": dict(stats), "seconds": round(time.perf_counter() - started, 2),
               "finished_at": dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0).isoformat()}
    write_json(path, payload)
    return payload


def files_digest(work: Path, pattern: str) -> str:
    """Digest of a set of work files (relative path + sha256), e.g. every ``bars/*.parquet``."""
    items = sorted((p.relative_to(work).as_posix(), sha256_file(p)) for p in work.glob(pattern))
    if not items:
        raise FileNotFoundError(f"no {pattern} under {work}")
    return hashlib.sha256(canonical_json(items).encode("utf-8")).hexdigest()


def th3_sha256(th3: Path) -> str:
    """The retained file's sha256 from its sidecar (hashing 3.45 GB per run is avoided)."""
    sidecar = th3.with_name(th3.name + ".sha256")
    if not sidecar.is_file():
        raise FileNotFoundError(f"{sidecar} (the retained file's sha256 sidecar) is missing")
    return sidecar.read_text(encoding="utf-8").split()[0].lower()


def connect(work: Path, name: str | None = None, *, memory_limit: str | None = None) -> Any:
    """A bounded DuckDB connection (worker limits from the environment); ``name`` = a scratch DB file.

    The spill directory is private to the connection (``research_lake.connect_bounded``,
    ruling C-56), so concurrent stage workers never share spill files.
    """
    limit = memory_limit or os.environ.get("ATX_RESEARCH_DUCKDB_MEMORY_LIMIT", "256MB")
    threads = int(os.environ.get("ATX_RESEARCH_DUCKDB_THREADS", "1"))
    if name is None:
        return connect_bounded(None, root=DEFAULT_ROOT, memory_limit=limit, threads=threads)
    scratch = work / "tmp" / f"{name}.duckdb"
    scratch.parent.mkdir(parents=True, exist_ok=True)
    for leftover in (scratch, scratch.with_name(scratch.name + ".wal")):
        if leftover.exists():
            leftover.unlink()
    return connect_bounded(scratch, root=DEFAULT_ROOT, memory_limit=limit, threads=threads, read_only=False)


def drop_scratch(work: Path, name: str) -> None:
    scratch = work / "tmp" / f"{name}.duckdb"
    for leftover in (scratch, scratch.with_name(scratch.name + ".wal")):
        if leftover.exists():
            leftover.unlink()


# ---------------------------------------------------------------------------
# Sessions and formations
# ---------------------------------------------------------------------------

def sessions() -> list[dt.date]:
    return _xnys.xnys_sessions(SESSION_ORIGIN, SESSION_HORIZON)


def session_numbers() -> dict[dt.date, int]:
    return {day: index for index, day in enumerate(sessions())}


def month_end(day: dt.date) -> dt.date:
    return dt.date(day.year + day.month // 12, day.month % 12 + 1, 1) - dt.timedelta(days=1)


def add_months(eom: dt.date, months: int) -> dt.date:
    index = eom.year * 12 + eom.month - 1 + months
    return month_end(dt.date(index // 12, index % 12 + 1, 1))


def formation_months(first: dt.date = FIRST_EOM, last: dt.date = LAST_EOM) -> list[dt.date]:
    months, eom = [], month_end(first)
    while eom <= last:
        months.append(eom)
        eom = add_months(eom, 1)
    return months


def formation_rows(first: dt.date, last: dt.date) -> list[tuple[dt.date, dt.date, int, int]]:
    """``(eom, formation session, its sno, sno of the month's first session)`` per month end."""
    numbers = session_numbers()
    rows = []
    for eom in formation_months(first, last):
        formation = _xnys.expected_month_end_session(eom.year, eom.month)
        first_session = next(day for day in sessions() if day >= eom.replace(day=1))
        rows.append((eom, formation, numbers[formation], numbers[first_session]))
    return rows


def stage_sessions_table(con: Any) -> None:
    """``_px_sessions(session, sno, eom, prev_eom)``: the XNYS rule sessions, their calendar month end
    and the previous month end (the latest formation strictly before any session of the month)."""
    days = sessions()
    con.execute("CREATE OR REPLACE TEMP TABLE _px_sessions (session DATE, sno INTEGER, eom DATE, prev_eom DATE, "
                "week DATE)")
    con.executemany("INSERT INTO _px_sessions VALUES (?, ?, ?, ?, ?)",
                    [(day, index, month_end(day), day.replace(day=1) - dt.timedelta(days=1),
                      day - dt.timedelta(days=day.weekday())) for index, day in enumerate(days)])


def bars_files(work: Path, buckets: Iterable[int] | None = None) -> list[str]:
    files = sorted((work / "bars").glob("bucket=*.parquet"))
    if buckets is not None:
        wanted = {int(b) for b in buckets}
        files = [f for f in files if int(f.stem.split("=", 1)[1]) in wanted]
    if not files:
        raise FileNotFoundError(f"no bars files under {work / 'bars'}")
    return [f.as_posix() for f in files]


def parquet_list(paths: Sequence[str]) -> str:
    return "[" + ", ".join(sql_text(p) for p in paths) + "]"


# ---------------------------------------------------------------------------
# A9 vendor share unit rule
# ---------------------------------------------------------------------------

def stage_units(th3: Path, work: Path) -> dict[str, Any]:
    """Decide the unit of the file's ``shares`` column by the A9 rule (format + per-year median).

    The parquet format means thousands; every year's cross-section median of positive
    counts must be consistent with thousands (``bodies_0327._median_verdict``), else raise:
    nothing is scaled on a guess. Rows above ``THOUSANDS_ROW_CEILING`` are counted (they are
    withheld by :func:`stage_bars`).
    """
    from ..migrations.bodies_0327 import THOUSANDS_ROW_CEILING, _median_verdict
    from ..ticker_history_bulk import SHARES_UNIT_BY_FORMAT, SHARES_UNIT_SCALE

    started = time.perf_counter()
    receipt = work / "units.json"
    code = stage_digest(stage_units)
    inputs = {"th3_sha256": th3_sha256(th3)}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    con = connect(work, "units")
    try:
        rows = con.execute(f"""
            SELECT year(tradingDate) AS year, approx_quantile(shares, 0.5) FILTER (WHERE shares > 0) AS median,
                   count(DISTINCT securityID) FILTER (WHERE shares > 0) AS lines,
                   count(*) FILTER (WHERE shares > {int(THOUSANDS_ROW_CEILING)}) AS above_ceiling,
                   count(*) FILTER (WHERE shares > 0) AS positive_rows
            FROM read_parquet({sql_text(th3.as_posix())}) WHERE securityID > 0 GROUP BY 1 ORDER BY 1
        """).fetchall()
    finally:
        con.close()
        drop_scratch(work, "units")
    format_unit = SHARES_UNIT_BY_FORMAT["parquet"]
    years = []
    for year, median, lines, above, positive in rows:
        verdict = _median_verdict(None if median is None else float(median), int(lines))
        years.append({"year": int(year), "median_positive_shares": None if median is None else float(median),
                      "lines": int(lines), "positive_rows": int(positive), "rows_above_ceiling": int(above),
                      "median_verdict": verdict})
        if median is not None and verdict not in (format_unit, "either"):
            raise RuntimeError(f"A9 unit rule: year {year} median {median:,.0f} means {verdict}, the parquet "
                               f"format means {format_unit}; refusing to scale on a guess")
    stats = {"format": "parquet", "format_unit": format_unit, "scale": SHARES_UNIT_SCALE[format_unit],
             "thousands_row_ceiling": int(THOUSANDS_ROW_CEILING), "years": years,
             "rows_above_ceiling": sum(y["rows_above_ceiling"] for y in years),
             "rule": "A9: format evidence (parquet = thousands) and every year's cross-section median agree; "
                     "rows above the ceiling are withheld"}
    return finish_receipt(receipt, code, inputs, [], stats, started)


# ---------------------------------------------------------------------------
# Bars
# ---------------------------------------------------------------------------

def _pick_order() -> str:
    """I4 publisher order (0.13) plus row-content tie-breaks for rows equal on every publisher term."""
    return (f"{_vendor_artifact.bar_pick_order_sql(with_shares=True)}, volume DESC NULLS LAST, "
            "high DESC NULLS LAST, low DESC NULLS LAST, open DESC NULLS LAST, dn DESC NULLS LAST, "
            "current_symbol ASC NULLS LAST")


def stage_bars(th3: Path, work: Path, bucket: int, buckets: int) -> dict[str, Any]:
    """Cleaned, deduplicated, VA1-repaired session bars of one line bucket (``securityID % buckets``)."""
    started = time.perf_counter()
    units = read_json(work / "units.json")["stats"]
    scale, ceiling = int(units["scale"]), int(units["thousands_row_ceiling"])
    out = work / "bars" / f"bucket={bucket:02d}.parquet"
    receipt = out.with_suffix(".json")
    code = stage_digest(stage_bars, _pick_order)
    inputs = {"th3_sha256": th3_sha256(th3), "bucket": bucket, "buckets": buckets, "scale": scale,
              "ceiling": ceiling, "repair": _vendor_artifact.REPAIR_VERSION}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    name = f"bars-{bucket:02d}"
    con = connect(work, name)
    try:
        stage_sessions_table(con)
        source = sql_text(th3.as_posix())
        con.execute(f"""
            CREATE TABLE raw AS
            SELECT s.sno, CAST(t.tradingDate AS DATE) AS trade_date,
                   '{LINE_PREFIX}' || CAST(t.securityID AS VARCHAR) AS line_id,
                   CAST(t.securityID AS VARCHAR) AS vendor_security_id,
                   upper(trim(coalesce(nullif(t.ticker_tk, ''), nullif(t.todayTicker, '')))) AS symbol,
                   upper(trim(coalesce(nullif(t.todayTicker, ''), nullif(t.ticker_tk, '')))) AS current_symbol,
                   CAST(t.dn AS BIGINT) AS dn, CAST(t.open AS DOUBLE) AS open, CAST(t.high AS DOUBLE) AS high,
                   CAST(t.low AS DOUBLE) AS low, CAST(t.close AS DOUBLE) AS close,
                   CAST(t.volume AS DOUBLE) AS volume,
                   CASE WHEN t.shares > 0 AND t.shares <= {ceiling} THEN CAST(t.shares AS DOUBLE) * {scale}
                        END AS shares_outstanding,
                   coalesce(t.shares > {ceiling}, false) AS shares_over_ceiling,
                   CASE WHEN isfinite(t.close) AND t.close > 0 AND isfinite(t.cumulReturnFactor)
                             AND t.cumulReturnFactor > 0 AND isfinite(t.close * t.cumulReturnFactor)
                             AND t.close * t.cumulReturnFactor > 0
                        THEN CAST(t.close * t.cumulReturnFactor AS DOUBLE) END AS adjusted_close,
                   CAST(t.nEarnCnt_504d AS DOUBLE) AS earn_504d,
                   CAST(NULL AS TIMESTAMP) AS available_at, '{VENDOR_SOURCE}' AS source,
                   CAST(NULL AS TIMESTAMP) AS source_loaded_at,
                   s.sno IS NULL AS stray,
                   (t.tradingDate IS NULL OR coalesce(nullif(trim(t.ticker_tk), ''), nullif(trim(t.todayTicker), ''))
                        IS NULL) AS bad_key,
                   (coalesce(t.volume < 0, false) OR t.close IS NULL OR coalesce(t.close <= 0, false)
                    OR coalesce(NOT isfinite(t.close), false)) AS bad_ohlcv,
                   coalesce(t.open > 0 AND t.high > 0 AND t.low > 0 AND isfinite(t.open) AND isfinite(t.high)
                            AND isfinite(t.low) AND t.high >= greatest(t.open, t.low, t.close)
                            AND t.low <= least(t.open, t.high, t.close), false) AS ohlc_ok
            FROM read_parquet({source}) t
            LEFT JOIN _px_sessions s ON s.session = CAST(t.tradingDate AS DATE)
            WHERE t.securityID > 0 AND t.securityID % {int(buckets)} = {int(bucket)}
        """)
        counts = con.execute("""
            SELECT count(*), count(*) FILTER (WHERE stray), count(*) FILTER (WHERE bad_key),
                   count(*) FILTER (WHERE bad_ohlcv AND NOT stray AND NOT bad_key),
                   count(*) FILTER (WHERE shares_over_ceiling), count(DISTINCT line_id),
                   count(*) FILTER (WHERE NOT ohlc_ok AND NOT (stray OR bad_key OR bad_ohlcv))
            FROM raw
        """).fetchone()
        stray_days = con.execute("SELECT trade_date, count(*) FROM raw WHERE stray GROUP BY 1 ORDER BY 1").fetchall()
        con.execute(f"""
            CREATE TABLE picked AS
            SELECT * EXCLUDE (bar_pick) FROM (
              SELECT *, row_number() OVER (PARTITION BY line_id, trade_date ORDER BY {_pick_order()}) AS bar_pick,
                     count(*) OVER (PARTITION BY line_id, trade_date) AS key_rows
              FROM raw WHERE NOT (stray OR bad_key OR bad_ohlcv))
            WHERE bar_pick = 1
        """)
        con.execute("DROP TABLE raw")
        dup = con.execute("""
            SELECT count(*) FILTER (WHERE key_rows > 1), coalesce(sum(key_rows) FILTER (WHERE key_rows > 1), 0),
                   count(*), count(*) FILTER (WHERE adjusted_close IS NULL)
            FROM picked
        """).fetchone()
        repair = _vendor_artifact.repaired_bars_sql("picked", close="close", adjusted="adjusted_close",
                                                    shares="shares_outstanding", key="line_id", order="trade_date")
        con.execute(f"CREATE TABLE rep AS {repair}")
        con.execute("DROP TABLE picked")
        steps = con.execute("""
            SELECT count(*) FILTER (WHERE va_ln_k <> 0), count(DISTINCT line_id) FILTER (WHERE va_ln_k <> 0),
                   count(*) FILTER (WHERE va_ln_k <> 0 AND trade_date = DATE '2021-01-04')
            FROM rep
        """).fetchone()
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name(f".{out.name}.{os.getpid()}.tmp")
        floor, ratio, step = float(VA_SENTINEL_CLOSE_MIN), float(VA_SENTINEL_RATIO), float(VA_FACTOR_STEP_MIN)
        flat_step, band, gap = float(VA_FLAT_FACTOR_STEP_MIN), float(VA_FLAT_CLOSE_BAND), int(VA_FLAT_MAX_GAP_SESSIONS)
        line_order = "PARTITION BY line_id ORDER BY sno"
        con.execute(f"""
            COPY (
              SELECT line_id, trade_date, sno, symbol, current_symbol, close, adj, open, high, low, volume, shares,
                     earn_504d, va_step, key_rows, ohlc_ok, va_suspect, va_brk_in,
                     close >= {floor} AND p_ok IS NULL AS va_sentinel_unanchored,
                     CAST(sum(va_brk_in) OVER ({line_order} ROWS UNBOUNDED PRECEDING) AS INTEGER) AS va_brk,
                     CASE WHEN prev_sno = sno - 1 AND va_brk_in = 0 THEN adj / prev_adj - 1.0 END AS r,
                     CASE WHEN prev_sno = sno - 1 AND va_brk_in = 0 THEN ln(adj / prev_adj) END AS lr
              FROM (
                SELECT *, CAST(CAST(va_suspect IS NOT NULL AS INTEGER)
                               + CAST(coalesce(lag(va_suspect) OVER ({line_order}) = '{VA_SENTINEL}', false) AS INTEGER)
                               AS TINYINT) AS va_brk_in
                FROM (
                  SELECT *,
                         CASE WHEN trade_date = DATE '{VA_ARTIFACT_SESSION}' AND prev_fac > 0
                                   AND adj / close >= {step} * prev_fac THEN '{VA_FACTOR_STEP}'
                              WHEN prev_fac > 0 AND prev_close > 0 AND sno - prev_sno <= {gap}
                                   AND adj / close >= {flat_step} * prev_fac
                                   AND close BETWEEN {1.0 - band} * prev_close AND {1.0 + band} * prev_close
                                   THEN '{VA_FACTOR_FLAT}'
                              WHEN close >= {floor} AND p_ok IS NOT NULL
                                   AND close > {ratio} * p_ok THEN '{VA_SENTINEL}'
                         END AS va_suspect
                  FROM (
                    SELECT *, lag(sno) OVER ({line_order}) AS prev_sno, lag(adj) OVER ({line_order}) AS prev_adj,
                           lag(adj / close) OVER ({line_order}) AS prev_fac,
                           lag(close) OVER ({line_order}) AS prev_close,
                           last_value(CASE WHEN close < {floor} THEN close END IGNORE NULLS)
                             OVER ({line_order} ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS p_ok
                    FROM (SELECT line_id, trade_date, sno, symbol, current_symbol, close,
                                 adjusted_close * va_multiplier AS adj, CASE WHEN ohlc_ok THEN open END AS open,
                                 CASE WHEN ohlc_ok THEN high END AS high, CASE WHEN ohlc_ok THEN low END AS low,
                                 volume, shares_outstanding AS shares, earn_504d, va_ln_k <> 0 AS va_step, key_rows,
                                 ohlc_ok
                          FROM rep WHERE adjusted_close > 0 AND isfinite(adjusted_close * va_multiplier)
                                         AND adjusted_close * va_multiplier > 0))))
              ORDER BY trade_date, line_id
            ) TO {sql_text(tmp.as_posix())} (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 100000)
        """)
        written = con.execute(f"SELECT count(*), count(r), min(trade_date), max(trade_date), "
                              f"count(*) FILTER (WHERE va_suspect = '{VA_FACTOR_STEP}'), "
                              f"count(*) FILTER (WHERE va_suspect = '{VA_SENTINEL}'), "
                              f"count(*) FILTER (WHERE va_brk_in > 0), "
                              f"count(DISTINCT line_id) FILTER (WHERE va_suspect IS NOT NULL), "
                              f"count(*) FILTER (WHERE va_suspect = '{VA_FACTOR_FLAT}'), "
                              f"count(*) FILTER (WHERE va_sentinel_unanchored), "
                              f"count(DISTINCT line_id) FILTER (WHERE va_sentinel_unanchored) "
                              f"FROM read_parquet({sql_text(tmp.as_posix())})").fetchone()
        suspects = con.execute(f"""
            SELECT line_id, trade_date, va_suspect, close, adj / close / prev_fac, prev_close
            FROM (SELECT *, lag(adj / close) OVER ({line_order}) AS prev_fac, lag(close) OVER ({line_order}) AS prev_close
                  FROM read_parquet({sql_text(tmp.as_posix())}))
            WHERE va_suspect IS NOT NULL ORDER BY line_id, trade_date
        """).fetchall()
    finally:
        con.close()
        drop_scratch(work, name)
    os.replace(tmp, out)
    stats = {"source_rows_positive_id": int(counts[0]), "stray_rows": int(counts[1]),
             "stray_days": [[str(d), int(n)] for d, n in stray_days], "bad_key_rows": int(counts[2]),
             "invalid_price_rows": int(counts[3]), "shares_over_ceiling_rows": int(counts[4]),
             "ohlc_inconsistent_rows_kept": int(counts[6]),
             "lines": int(counts[5]), "duplicate_keys": int(dup[0]), "duplicate_key_rows": int(dup[1]),
             "picked_rows": int(dup[2]), "invalid_adjusted_rows": int(dup[3]),
             "va_steps": int(steps[0]), "va_lines": int(steps[1]), "va_steps_2021_01_04": int(steps[2]),
             "rows": int(written[0]), "daily_returns": int(written[1]), "first": str(written[2]),
             "last": str(written[3]),
             "vendor_artifact_suspect": {
                 "rule": {"artifact_session": str(VA_ARTIFACT_SESSION), "factor_step_min": VA_FACTOR_STEP_MIN,
                          "flat_factor_step_min": VA_FLAT_FACTOR_STEP_MIN, "flat_close_band": VA_FLAT_CLOSE_BAND,
                          "flat_max_gap_sessions": VA_FLAT_MAX_GAP_SESSIONS,
                          "sentinel_close_min": VA_SENTINEL_CLOSE_MIN, "sentinel_ratio": VA_SENTINEL_RATIO,
                          "sentinel_anchor": "nearest_prior_close_below_floor; no future close",
                          "unanchored_high_close": "ambiguous, counted; no corruption inferred"},
                 "factor_step_bars": int(written[4]), "factor_step_close_flat_bars": int(written[8]),
                 "sentinel_bars": int(written[5]),
                 "break_bars_returns_nulled": int(written[6]), "lines": int(written[7]),
                 "unanchored_high_close_bars": int(written[9]), "unanchored_high_close_lines": int(written[10]),
                 "bars": [{"line_id": lid, "trade_date": str(day), "kind": kind, "close": close,
                           "factor_step": None if k is None else round(float(k), 3), "prev_close": pc}
                          for lid, day, kind, close, k, pc in suspects]}}
    if written[0] + int(dup[3]) != int(dup[2]):
        raise RuntimeError(f"bucket {bucket}: {written[0]} written + {dup[3]} invalid-adjusted != {dup[2]} picked")
    return finish_receipt(receipt, code, inputs, [out], stats, started)


def duplicate_key_stats(th3: Path, work: Path) -> dict[str, Any]:
    """Duplicate vendor keys over the whole file: keys, rows, and keys whose rows differ (report QA)."""
    started = time.perf_counter()
    receipt = work / "qa_duplicates.json"
    code = stage_digest(duplicate_key_stats)
    inputs = {"th3_sha256": th3_sha256(th3)}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    con = connect(work, "dups")
    parts = 16
    try:
        source = sql_text(th3.as_posix())
        ids = con.execute(f"""
            SELECT count(*), count(*) FILTER (WHERE securityID IS NULL), count(*) FILTER (WHERE securityID <= 0)
            FROM read_parquet({source})
        """).fetchone()
        formations = [row[1] for row in formation_rows(BASE_FIRST_EOM, LAST_EOM)]
        totals = [0, 0, 0, 0]
        by_year: dict[str, int] = {}
        # One pass per id bucket (a whole-file key aggregate does not fit a 256MB worker); rows of a
        # key differ when their content hashes differ (no DISTINCT aggregate: it cannot spill).
        for part in range(parts):
            con.execute(f"""
                CREATE OR REPLACE TABLE dk AS
                SELECT securityID, CAST(tradingDate AS DATE) AS d, count(*) AS n,
                       min(hash(struct_pack(close, cumulReturnFactor, volume, shares, high, low, open, ticker_tk)))
                         <> max(hash(struct_pack(close, cumulReturnFactor, volume, shares, high, low, open, ticker_tk)))
                         AS differ
                FROM read_parquet({source}) WHERE securityID > 0 AND securityID % {parts} = {part}
                GROUP BY 1, 2 HAVING count(*) > 1
            """)
            row = con.execute("""
                SELECT count(*), coalesce(sum(n), 0), count(*) FILTER (WHERE differ),
                       count(*) FILTER (WHERE last_day(d) = d OR d IN (SELECT unnest(?::DATE[])))
                FROM dk
            """, [formations]).fetchone()
            totals = [a + int(b) for a, b in zip(totals, row, strict=True)]
            for year, n in con.execute("SELECT year(d), count(*) FROM dk GROUP BY 1").fetchall():
                by_year[str(year)] = by_year.get(str(year), 0) + int(n)
    finally:
        con.close()
        drop_scratch(work, "dups")
    stats = {"rows": int(ids[0]), "null_id_rows": int(ids[1]), "nonpositive_id_rows": int(ids[2]),
             "duplicate_keys": totals[0], "duplicate_rows": totals[1], "keys_with_differing_rows": totals[2],
             "keys_at_month_end_or_formation_sessions": totals[3],
             "keys_by_year": dict(sorted(by_year.items()))}
    return finish_receipt(receipt, code, inputs, [], stats, started)


# ---------------------------------------------------------------------------
# Unidentified vendor rows (ruling C-57: characterized and excluded with a labeled reason)
# ---------------------------------------------------------------------------

#: The labeled exclusion reasons of the rows without a positive vendor ``securityID`` (every such
#: row of the retained file has ``securityID = 0``: the vendor's unmapped bucket, not a line). First
#: match wins, in this order.
UNIDENTIFIED_REASONS = (
    ("placeholder_blank_symbol", "sym IS NULL"),
    ("vendor_test_symbol", r"regexp_full_match(sym, '(Z[A-Z]ZZT|ZXIET|ZEXIT|ZIEXT|ZVV|[A-Z]?TEST)(\.[A-Z]+)?')"),
    ("index_or_index_proxy", "sym LIKE '%!%' OR sym IN ('SPX', 'SPXW', 'XSP', 'RUT', 'MRUT', 'DJX', 'NDX', 'XND', "
                             "'OEX', 'XEO', 'VIX', 'MXEA', 'MXEF', 'NANOS')"),
    ("non_common_instrument_symbol",
     r"regexp_matches(sym, '\.PR|\.W[ST]?$|\.U$|\.RT?$|[.\-/ ]WS') OR (length(sym) = 5 AND sym[5] IN ('W', 'U', 'R'))"),
    ("unmapped_symbol_with_earnings", "earn_symbol"),
    ("unmapped_symbol", "true"),
)
UNIDENTIFIED_BASIS = ("securityID = 0 rows carry no vendor line identity; a symbol is not a line key (symbols are "
                      "reused), so no row is keyed or mapped by symbol here: mapping them is the identity layer's "
                      "job (nodes 3.3/3.6)")


def unidentified_rows_stats(th3: Path, work: Path) -> dict[str, Any]:
    """``qa_unidentified.json``: the rows without a positive ``securityID``, by labeled exclusion reason.

    Per reason: rows, symbols, dates and rows on formation sessions; per year; and the rows of
    the ``unmapped_symbol*`` reasons on each formation session against the positive-id lines
    with a bar there (how much of the month-end cross-section the exclusion could touch).
    """
    started = time.perf_counter()
    receipt = work / "qa_unidentified.json"
    code = stage_digest(unidentified_rows_stats)
    inputs = {"th3_sha256": th3_sha256(th3)}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    arms = " ".join(f"WHEN {test} THEN '{name}'" for name, test in UNIDENTIFIED_REASONS)
    formations = [row[1] for row in formation_rows(BASE_FIRST_EOM, LAST_EOM)]
    con = connect(work, "unidentified")
    try:
        source = sql_text(th3.as_posix())
        con.execute(f"""
            CREATE TABLE z AS
            SELECT securityID AS id, CAST(tradingDate AS DATE) AS d,
                   upper(trim(coalesce(nullif(trim(ticker_tk), ''), nullif(trim(todayTicker), '')))) AS sym,
                   nEarnCnt_504d AS earn, cumulReturnFactor AS factor
            FROM read_parquet({source}) WHERE securityID IS NULL OR securityID <= 0
        """)
        con.execute(f"""
            CREATE TABLE zr AS
            SELECT z.*, CASE {arms} END AS reason
            FROM (SELECT *, bool_or(coalesce(earn, 0) > 0) OVER (PARTITION BY sym) AS earn_symbol FROM z) z
        """)
        ids = con.execute("SELECT count(*), count(*) FILTER (WHERE id IS NULL), count(*) FILTER (WHERE id = 0), "
                          "count(*) FILTER (WHERE id < 0), count(DISTINCT sym), min(d), max(d), "
                          "count(*) FILTER (WHERE factor IS DISTINCT FROM 1.0) FROM zr").fetchone()
        reasons = con.execute("""
            SELECT reason, count(*), count(DISTINCT sym), count(DISTINCT d),
                   count(*) FILTER (WHERE d IN (SELECT unnest(?::DATE[])))
            FROM zr GROUP BY 1 ORDER BY 2 DESC
        """, [formations]).fetchall()
        by_year = con.execute("SELECT year(d), reason, count(*) FROM zr GROUP BY 1, 2 ORDER BY 1, 2").fetchall()
        cross = con.execute(f"""
            WITH f AS (SELECT unnest(?::DATE[]) AS d),
            p AS (SELECT CAST(tradingDate AS DATE) AS d, count(DISTINCT securityID) AS lines
                  FROM read_parquet({source}) WHERE securityID > 0 AND CAST(tradingDate AS DATE) IN (SELECT d FROM f)
                  GROUP BY 1),
            u AS (SELECT d, count(*) FILTER (WHERE reason = 'unmapped_symbol_with_earnings') AS earn_rows,
                         count(*) FILTER (WHERE reason = 'unmapped_symbol') AS other_rows
                  FROM zr WHERE d IN (SELECT d FROM f) GROUP BY 1)
            SELECT year(p.d), count(*), avg(p.lines), avg(coalesce(u.earn_rows, 0)), max(coalesce(u.earn_rows, 0)),
                   avg(coalesce(u.other_rows, 0)), max(coalesce(u.other_rows, 0))
            FROM p LEFT JOIN u USING (d) GROUP BY 1 ORDER BY 1
        """, [formations]).fetchall()
    finally:
        con.close()
        drop_scratch(work, "unidentified")
    years: dict[str, dict[str, int]] = {}
    for year, reason, n in by_year:
        years.setdefault(str(year), {})[reason] = int(n)
    stats = {"rows": int(ids[0]), "null_id_rows": int(ids[1]), "zero_id_rows": int(ids[2]),
             "negative_id_rows": int(ids[3]), "symbols": int(ids[4]), "first": str(ids[5]), "last": str(ids[6]),
             "rows_factor_not_one": int(ids[7]), "decision": "excluded with the labeled reason of each row",
             "basis": UNIDENTIFIED_BASIS,
             "reasons": {r: {"rows": int(n), "symbols": int(s), "dates": int(d), "formation_session_rows": int(f)}
                         for r, n, s, d, f in reasons},
             "rules": {name: test for name, test in UNIDENTIFIED_REASONS}, "by_year": years,
             "formation_sessions_by_year": [
                 {"year": int(y), "sessions": int(n), "mean_positive_lines": round(float(pl), 1),
                  "mean_unmapped_earnings_rows": round(float(er), 2), "max_unmapped_earnings_rows": int(em),
                  "mean_unmapped_other_rows": round(float(orr), 2), "max_unmapped_other_rows": int(om)}
                 for y, n, pl, er, em, orr, om in cross]}
    return finish_receipt(receipt, code, inputs, [], stats, started)


# ---------------------------------------------------------------------------
# Lines and security types
# ---------------------------------------------------------------------------

#: Nasdaq Trader symbol directories retained with the SEC cache (current snapshot).
DIRECTORY_FILES = (Path("C:/atx/atx-db/data/cache/nasdaqlisted.txt"), Path("C:/atx/atx-db/data/cache/otherlisted.txt"))


def read_directory(files: Sequence[Path] = DIRECTORY_FILES) -> dict[str, dict[str, Any]]:
    """Symbol -> ``{name, etf, test_issue, file}`` from the Nasdaq Trader directory snapshot.

    ``nasdaqlisted.txt`` keys by ``Symbol``; ``otherlisted.txt`` by its ``CQS Symbol``, ``ACT
    Symbol`` and ``NASDAQ Symbol`` (all upper-case). The footer (``File Creation Time``) is
    recorded as the snapshot's date.
    """
    entries: dict[str, dict[str, Any]] = {}
    for path in files:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        header = lines[0].split("|")
        for raw in lines[1:]:
            if raw.startswith("File Creation Time"):
                continue
            cells = dict(zip(header, raw.split("|"), strict=False))
            record = {"name": cells.get("Security Name", ""), "etf": cells.get("ETF", ""),
                      "test_issue": cells.get("Test Issue", ""), "file": path.name}
            keys = [cells.get(k, "") for k in ("Symbol", "CQS Symbol", "ACT Symbol", "NASDAQ Symbol")]
            for key in keys:
                key = (key or "").strip().upper()
                if key and key not in entries:
                    entries[key] = record
    return entries


def directory_snapshot(files: Sequence[Path] = DIRECTORY_FILES) -> dict[str, Any]:
    stamps = []
    for path in files:
        tail = path.read_text(encoding="utf-8", errors="replace").splitlines()[-1]
        stamps.append(tail.split("|")[0].replace("File Creation Time:", "").strip())
    return {"files": [{"path": p.as_posix(), "sha256": sha256_file(p), "created": s} for p, s in
                      zip(files, stamps, strict=True)]}


def symbol_variants(symbol: str) -> list[str]:
    """The vendor symbol and its directory spellings (``BRK.B`` / ``BRK-B`` / ``BRK/B`` / ``BRK B``)."""
    base = (symbol or "").strip().upper()
    if not base:
        return []
    variants = [base]
    for sep in (".", "-", "/", " "):
        for other in (".", "-", "/", " "):
            if sep != other and sep in base:
                candidate = base.replace(sep, other)
                if candidate not in variants:
                    variants.append(candidate)
    return variants


#: A line is still listed at the directory snapshot when its last bar is at most this many days
#: before the file's last session (the directory then names the line itself, not a later holder
#: of a reused symbol).
LISTED_AT_SNAPSHOT_DAYS = 14


def classify_lines(lines: Sequence[Mapping[str, Any]], directory: Mapping[str, Mapping[str, Any]],
                   file_last: dt.date) -> list[dict[str, Any]]:
    """The security type of each line (``universe_basis`` :data:`UNIVERSE_BASIS`).

    The vendor file carries no security names, so the A2 classifier
    (``universe_us_listed.classify_security_type``) reads the only retained name source: the
    Nasdaq Trader directory snapshot (``nasdaqlisted``/``otherlisted``, 2026-09-18).

    * ``directory_name``: a line still trading at the snapshot (last bar within
      :data:`LISTED_AT_SNAPSHOT_DAYS` of the file's last session) whose current vendor symbol
      the directory lists: A2 on the directory's security name with its ETF and test-issue
      flags. The directory only *excludes* here (a listed ETF, fund, preferred, unit ... is
      never eligible); a listed common is typed ``common`` but still needs evidence at F.
    * ``vendor_earnings_events``: any other line (delisted, renamed away, or not in the
      directory). A symbol's current directory entry may name a later holder of a reused
      symbol, so it is not read. The line is ``common_unverified`` when the vendor ever
      attached an earnings event to it (``nEarnCnt_504d > 0``: an operating company; funds,
      ETFs, ETNs and trusts never report earnings), else ``unknown`` (never eligible).

    Ruling C-78 (1.12 review I1): eligibility at a formation F is point in time and the same for
    listed and delisted lines: an eligible type *and* ``first_earn_date`` (the line's first bar
    with ``nEarnCnt_504d > 0``) on or before F's session (:func:`stage_spine`). No line enters on
    its survival to the snapshot or on evidence first seen after F. Asymmetry left, stated: a
    delisted ADR, REIT or LP with earnings evidence is admitted (no name source says what it
    was), while its listed twin is excluded by its directory name.
    """
    from ..universe_us_listed import classify_security_type

    out = []
    for line in lines:
        symbol = str(line.get("current_symbol") or "")
        last = line.get("last_date")
        listed = last is not None and (file_last - last).days <= LISTED_AT_SNAPSHOT_DAYS
        entry = next((directory[v] for v in symbol_variants(symbol) if v in directory), None) if listed else None
        if entry is not None:
            kind = classify_security_type(entry["name"], etf=entry["etf"] == "Y", test_issue=entry["test_issue"] == "Y")
            out.append({**line, "security_type": kind, "type_basis": "directory_name",
                        "directory_name": entry["name"], "directory_file": entry["file"]})
            continue
        earnings = line.get("first_earn_date") is not None
        out.append({**line, "security_type": "common_unverified" if earnings else "unknown",
                    "type_basis": "vendor_earnings_events", "directory_name": None, "directory_file": None})
    return out


def stage_lines(work: Path) -> dict[str, Any]:
    """``lines.parquet``: per line first/last session, symbols, earnings evidence and security type."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    started = time.perf_counter()
    out = work / "lines.parquet"
    receipt = work / "lines.json"
    from .. import universe_us_listed

    code = stage_digest(stage_lines, classify_lines, read_directory, directory_snapshot, symbol_variants,
                        files=[Path(universe_us_listed.__file__)])
    inputs = {"bars": files_digest(work, "bars/bucket=*.parquet"), "directory": directory_snapshot()}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    con = connect(work, "lines")
    try:
        source = parquet_list(bars_files(work))
        rows = con.execute(f"""
            SELECT line_id, min(trade_date) AS first_date, max(trade_date) AS last_date, count(*) AS bars,
                   arg_max(current_symbol, trade_date) AS current_symbol, arg_max(symbol, trade_date) AS last_symbol,
                   count(DISTINCT symbol) AS symbols, max(earn_504d) AS max_earn_504d,
                   min(trade_date) FILTER (WHERE earn_504d > 0) AS first_earn_date
            FROM read_parquet({source}) GROUP BY line_id ORDER BY line_id
        """).fetchall()
        names = ("line_id", "first_date", "last_date", "bars", "current_symbol", "last_symbol", "symbols",
                 "max_earn_504d", "first_earn_date")
        lines = [dict(zip(names, row, strict=True)) for row in rows]
    finally:
        con.close()
        drop_scratch(work, "lines")
    typed = classify_lines(lines, read_directory(), max(line["last_date"] for line in lines))
    for line in typed:
        # Eligible at some formation: an eligible type with evidence; stage_spine applies the date (C-78).
        line["eligible"] = line["security_type"] in ELIGIBLE_SECURITY_TYPES and line["first_earn_date"] is not None
    columns = {name: [line[name] for line in typed] for name in (*names, "security_type", "type_basis",
                                                                 "directory_name", "directory_file", "eligible")}
    table = pa.table(columns)
    tmp = out.with_name(f".{out.name}.{os.getpid()}.tmp")
    pq.write_table(table, tmp, compression="zstd")
    os.replace(tmp, out)
    counts: dict[str, dict[str, int]] = {}
    for line in typed:
        bucket = counts.setdefault(line["type_basis"], {})
        bucket[line["security_type"]] = bucket.get(line["security_type"], 0) + 1
    no_evidence: dict[str, int] = {}
    for line in typed:
        if line["security_type"] in ELIGIBLE_SECURITY_TYPES and line["first_earn_date"] is None:
            no_evidence[line["type_basis"]] = no_evidence.get(line["type_basis"], 0) + 1
    stats = {"lines": len(typed), "eligible": sum(1 for line in typed if line["eligible"]),
             "types_by_basis": counts, "universe_basis": UNIVERSE_BASIS,
             "eligible_types": list(ELIGIBLE_SECURITY_TYPES),
             "eligible_type_without_evidence_by_basis": no_evidence,
             "rule": "eligible at formation F: an eligible type and first_earn_date <= F's session (ruling C-78)"}
    return finish_receipt(receipt, code, inputs, [out], stats, started)


# ---------------------------------------------------------------------------
# Monthly spine (JKP layout)
# ---------------------------------------------------------------------------

def year_formations(year: int, first: dt.date = BASE_FIRST_EOM, last: dt.date = LAST_EOM
                    ) -> list[tuple[dt.date, dt.date, int, int]]:
    low, high = max(first, dt.date(year, 1, 31)), min(last, dt.date(year, 12, 31))
    return formation_rows(low, high) if low <= high else []


def stage_forms_table(con: Any, forms: Sequence[tuple[dt.date, dt.date, int, int]]) -> None:
    """``_px_forms``: eom, formation session and its sno, the month's first sno, the shares lag date
    and the previous month's first and formation snos."""
    numbers = session_numbers()
    rows = []
    for eom, formation, fsno, first_sno in forms:
        prev = add_months(eom, -1)
        prev_formation = _xnys.expected_month_end_session(prev.year, prev.month)
        prev_first = next(day for day in sessions() if day >= prev.replace(day=1))
        rows.append((eom, formation, fsno, first_sno, formation - dt.timedelta(days=SHARES_LAG_DAYS),
                     numbers[prev_formation], numbers[prev_first]))
    con.execute("CREATE OR REPLACE TEMP TABLE _px_forms (eom DATE, formation_date DATE, fsno INTEGER, "
                "first_sno INTEGER, lag_date DATE, prev_fsno INTEGER, prev_first_sno INTEGER)")
    con.executemany("INSERT INTO _px_forms VALUES (?, ?, ?, ?, ?, ?, ?)", rows)


def stage_spine(work: Path, year: int, breakpoints: Sequence[str]) -> dict[str, Any]:
    """``base/year=Y.parquet``: the spine rows of the year's month ends (plus internal columns)."""
    started = time.perf_counter()
    out = work / "base" / f"year={year}.parquet"
    receipt = out.with_suffix(".json")
    forms = year_formations(year)
    code = stage_digest(stage_spine)
    inputs = {"bars": read_json(work / "lines.json")["inputs"]["bars"], "lines": sha256_file(work / "lines.parquet"),
              "breakpoints": [sha256_file(Path(p)) for p in breakpoints], "year": year}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    if not forms:
        raise ValueError(f"no spine formations in {year}")
    name = f"spine-{year}"
    out.parent.mkdir(parents=True, exist_ok=True)
    con = connect(work, name)
    try:
        stage_forms_table(con, forms)
        low = min(f[1] for f in forms).replace(day=1) - dt.timedelta(days=SHARES_LAG_DAYS + SHARES_MAX_AGE_DAYS + 40)
        high = max(f[1] for f in forms)
        con.execute(f"""
            CREATE TABLE b AS SELECT line_id, trade_date, sno, close, adj, volume, shares, va_suspect, va_brk
            FROM read_parquet({parquet_list(bars_files(work))})
            WHERE trade_date BETWEEN ? AND ?
        """, [low, high])
        con.execute(f"CREATE TABLE lines AS SELECT line_id, security_type, first_earn_date FROM read_parquet("
                    f"{sql_text((work / 'lines.parquet').as_posix())}) WHERE eligible")
        # Alive in the month: an observed bar between the month's first session and its formation session;
        # eligible at F only on vendor earnings evidence dated at or before F's session (ruling C-78).
        con.execute("""
            CREATE TABLE u AS
            SELECT f.eom, f.formation_date, f.fsno, f.lag_date, b.line_id, l.security_type,
                   max(b.trade_date) AS last_obs_date, count(*) AS n_obs_month
            FROM b JOIN _px_forms f ON b.sno BETWEEN f.first_sno AND f.fsno
            JOIN lines l ON l.line_id = b.line_id AND l.first_earn_date <= f.formation_date
            GROUP BY ALL
        """)
        # A sentinel formation bar (ruling C-79) has a bar but no usable price.
        con.execute(f"""
            CREATE TABLE s AS
            SELECT u.*, fb.line_id IS NOT NULL AS has_bar, coalesce(fb.va_suspect = '{VA_SENTINEL}', false) AS va_price,
                   CASE WHEN NOT coalesce(fb.va_suspect = '{VA_SENTINEL}', false) THEN fb.close END AS price,
                   fb.adj, fb.adj / fb.close AS fac_f, fb.va_brk AS brk_f
            FROM u LEFT JOIN b fb ON fb.line_id = u.line_id AND fb.sno = u.fsno
        """)
        con.execute("""
            CREATE TABLE prev AS
            SELECT f.eom, b.line_id, arg_max(b.adj, b.sno) AS prev_adj, arg_max(b.va_brk, b.sno) AS prev_brk
            FROM b JOIN _px_forms f ON b.sno BETWEEN f.prev_first_sno AND f.prev_fsno
            SEMI JOIN lines l ON l.line_id = b.line_id
            GROUP BY ALL
        """)
        con.execute(f"""
            CREATE TABLE liq AS
            SELECT f.eom, b.line_id, count(*) AS n_sessions_21d, avg(b.close * b.volume) AS dollar_volume_21d,
                   count(*) FILTER (WHERE b.va_suspect = '{VA_SENTINEL}') AS liq_sentinels
            FROM b JOIN _px_forms f ON b.sno BETWEEN f.fsno - 20 AND f.fsno
            SEMI JOIN lines l ON l.line_id = b.line_id
            GROUP BY ALL
        """)
        # A8 lag: the line's last share count dated at or before the lag date, restated to the
        # formation session's basis by the ratio of the repaired vendor factors (never across a
        # vendor artifact break: C-79).
        con.execute("""
            CREATE TABLE sh AS
            SELECT s.eom, s.line_id, v.shares AS shares_lag_raw, v.trade_date AS shares_lag_date,
                   v.adj / v.close AS fac_lag, v.va_brk AS brk_lag
            FROM (SELECT eom, line_id, lag_date FROM s WHERE has_bar) s
            ASOF JOIN (SELECT line_id, trade_date, shares, adj, close, va_brk FROM b WHERE shares > 0) v
              ON v.line_id = s.line_id AND v.trade_date <= s.lag_date
        """)
        bp = parquet_list(breakpoints)
        fresh = (f"shares_lag_date >= lag_date - INTERVAL {SHARES_MAX_AGE_DAYS} DAY AND fac_lag > 0 AND fac_f > 0")
        rows = con.execute(f"""
            COPY (
              SELECT s.eom, s.line_id, s.formation_date, s.fsno, s.security_type,
                     '{UNIVERSE_BASIS}' AS universe_basis, s.has_bar AS has_session_bar,
                     s.price, s.adj, s.fac_f, s.last_obs_date, s.n_obs_month,
                     sh.shares_lag_raw, sh.shares_lag_date, sh.fac_lag, x.shares_lagged,
                     s.price * x.shares_lagged AS me_line, '{ME_BASIS}' AS me_basis,
                     CASE WHEN s.adj > 0 AND p.prev_adj > 0 AND s.brk_f = p.prev_brk
                          THEN s.adj / p.prev_adj - 1.0 END AS ret_1m,
                     CASE WHEN coalesce(liq.liq_sentinels, 0) = 0 THEN liq.dollar_volume_21d END AS dollar_volume_21d,
                     coalesce(liq.n_sessions_21d, 0) AS n_sessions_21d,
                     CASE WHEN s.price * x.shares_lagged IS NULL OR k.me_p20_musd IS NULL THEN NULL
                          WHEN s.price * x.shares_lagged / 1e6 >= k.me_p80_musd THEN 'mega'
                          WHEN s.price * x.shares_lagged / 1e6 >= k.me_p50_musd THEN 'large'
                          WHEN s.price * x.shares_lagged / 1e6 >= k.me_p20_musd THEN 'small'
                          ELSE 'micro' END AS size_grp,
                     '{SIZE_BASIS}' AS size_basis, s.brk_f,
                     s.va_price AS va_suspect_price, coalesce(x.va_shares, false) AS va_suspect_shares,
                     coalesce(s.adj > 0 AND p.prev_adj > 0 AND s.brk_f <> p.prev_brk, false) AS va_suspect_ret_1m,
                     coalesce(liq.liq_sentinels, 0) > 0 AS va_suspect_liq,
                     year(s.eom) AS year
              FROM s
              LEFT JOIN sh ON sh.eom = s.eom AND sh.line_id = s.line_id
              LEFT JOIN (SELECT eom, line_id,
                                CASE WHEN {fresh} AND brk_lag = brk_f THEN shares_lag_raw * fac_f / fac_lag
                                     END AS shares_lagged,
                                coalesce({fresh} AND brk_lag <> brk_f, false) AS va_shares
                         FROM s JOIN sh USING (eom, line_id)) x ON x.eom = s.eom AND x.line_id = s.line_id
              LEFT JOIN prev p ON p.eom = s.eom AND p.line_id = s.line_id
              LEFT JOIN liq ON liq.eom = s.eom AND liq.line_id = s.line_id
              LEFT JOIN read_parquet({bp}) k ON k.month_end = s.eom
              ORDER BY s.eom, s.line_id
            ) TO {sql_text(out.with_name('.' + out.name + '.tmp').as_posix())} (FORMAT PARQUET, COMPRESSION ZSTD)
        """).fetchone()
        tmp = out.with_name("." + out.name + ".tmp")
        stats = con.execute(f"""
            SELECT eom, count(*), count(*) FILTER (WHERE has_session_bar), count(me_line), count(ret_1m),
                   count(size_grp), count(*) FILTER (WHERE size_grp IN ('mega', 'large')),
                   count(*) FILTER (WHERE va_suspect_price), count(*) FILTER (WHERE va_suspect_shares),
                   count(*) FILTER (WHERE va_suspect_ret_1m), count(*) FILTER (WHERE va_suspect_liq)
            FROM read_parquet({sql_text(tmp.as_posix())}) GROUP BY 1 ORDER BY 1
        """).fetchall()
    finally:
        con.close()
        drop_scratch(work, name)
    os.replace(tmp, out)
    months = [{"eom": str(e), "rows": int(n), "session_bar": int(sb), "me_line": int(me), "ret_1m": int(r),
               "size_grp": int(sg), "large_or_mega": int(lm), "va_suspect_price": int(vp),
               "va_suspect_shares": int(vs), "va_suspect_ret_1m": int(vr), "va_suspect_liq": int(vl)}
              for e, n, sb, me, r, sg, lm, vp, vs, vr, vl in stats]
    return finish_receipt(receipt, code, inputs, [out], {"rows": int(rows[0]), "months": months}, started)


def base_files(work: Path, years: Iterable[int] | None = None) -> list[str]:
    files = sorted((work / "base").glob("year=*.parquet"))
    if years is not None:
        wanted = {int(y) for y in years}
        files = [f for f in files if int(f.stem.split("=", 1)[1]) in wanted]
    if not files:
        raise FileNotFoundError(f"no spine base files under {work / 'base'}")
    return [f.as_posix() for f in files]


# ---------------------------------------------------------------------------
# EW market (P3 convention) and line months
# ---------------------------------------------------------------------------

def stage_market(work: Path) -> dict[str, Any]:
    """``market.parquet``: the daily EW market over the prior formation's spine lines (P3 convention).

    ``m`` (and ``lm = ln(1 + m)``) is the mean of the lines' repaired bar returns winsorized at
    +-:data:`MARKET_CLIP` (ruling C-55); ``m_unclipped`` is the plain mean, kept as a diagnostic.
    The receipt reports both: their moments, the sessions where they differ by more than
    :data:`MARKET_GAP_REPORT` and 2021-01-04 (the VA1 artifact session).
    """
    started = time.perf_counter()
    out = work / "market.parquet"
    receipt = work / "market.json"
    code = stage_digest(stage_market)
    inputs = {"bars": read_json(work / "lines.json")["inputs"]["bars"], "base": files_digest(work, "base/year=*.parquet")}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    clip = float(MARKET_CLIP)
    con = connect(work, "market")
    try:
        stage_sessions_table(con)
        con.execute("CREATE TABLE mkt (session DATE, sno INTEGER, prev_eom DATE, m_clip DOUBLE, m_raw DOUBLE, "
                    "names BIGINT, clipped BIGINT, extreme BIGINT, r_min DOUBLE, r_max DOUBLE)")
        years = sorted({int(Path(p).stem.split("=", 1)[1]) for p in base_files(work)})
        for year in range(years[0], years[-1] + 2):
            prior = [y for y in (year - 1, year) if y in years]
            if not prior:
                continue
            con.execute(f"""
                INSERT INTO mkt
                SELECT b.trade_date, b.sno, x.prev_eom, avg(greatest(least(b.r, {clip}), {-clip})), avg(b.r),
                       count(*), count(*) FILTER (WHERE abs(b.r) > {clip}), count(*) FILTER (WHERE abs(b.r) > 1.0),
                       min(b.r), max(b.r)
                FROM read_parquet({parquet_list(bars_files(work))}) b
                JOIN _px_sessions x ON x.sno = b.sno
                JOIN (SELECT eom, line_id FROM read_parquet({parquet_list(base_files(work, prior))})) u
                  ON u.line_id = b.line_id AND u.eom = x.prev_eom
                WHERE b.trade_date BETWEEN DATE '{year}-01-01' AND DATE '{year}-12-31' AND b.r IS NOT NULL
                GROUP BY b.trade_date, b.sno, x.prev_eom
            """)
        tmp = out.with_name("." + out.name + ".tmp")
        published = f"names >= {MARKET_MIN_NAMES}"
        con.execute(f"""
            COPY (SELECT session, sno, prev_eom, CASE WHEN {published} THEN m_clip END AS m,
                         CASE WHEN {published} THEN ln(1.0 + m_clip) END AS lm,
                         CASE WHEN {published} THEN m_raw END AS m_unclipped, names, clipped, extreme, r_min, r_max,
                         '{MARKET_BASIS}' AS market_basis, year(session) AS year
                  FROM mkt ORDER BY sno)
            TO {sql_text(tmp.as_posix())} (FORMAT PARQUET, COMPRESSION ZSTD)
        """)
        written = f"read_parquet({sql_text(tmp.as_posix())})"
        stats = con.execute(f"""
            SELECT count(*), count(m), min(names), avg(names), max(names), sum(extreme), sum(clipped),
                   min(session), max(session), avg(m), stddev_samp(m), avg(m_unclipped), stddev_samp(m_unclipped),
                   corr(m, m_unclipped), max(abs(m_unclipped - m)), min(m), max(m), min(m_unclipped), max(m_unclipped),
                   count(*) FILTER (WHERE abs(m_unclipped - m) > {MARKET_GAP_REPORT})
            FROM {written}
        """).fetchone()
        gaps = con.execute(f"""
            SELECT session, m, m_unclipped, names, clipped, extreme, r_min, r_max FROM {written}
            WHERE abs(m_unclipped - m) > {MARKET_GAP_REPORT} ORDER BY abs(m_unclipped - m) DESC LIMIT 25
        """).fetchall()
        artifact = con.execute(f"SELECT m, m_unclipped, names, clipped, extreme, r_min, r_max FROM {written} "
                               "WHERE session = DATE '2021-01-04'").fetchone()
    finally:
        con.close()
        drop_scratch(work, "market")
    os.replace(tmp, out)
    names = ("m", "m_unclipped", "names", "clipped", "extreme", "r_min", "r_max")
    payload = {"sessions": int(stats[0]), "published": int(stats[1]), "min_names": stats[2],
               "mean_names": None if stats[3] is None else round(float(stats[3]), 1), "max_names": stats[4],
               "extreme_returns": stats[5], "clipped_returns": stats[6], "first": str(stats[7]), "last": str(stats[8]),
               "min_names_rule": MARKET_MIN_NAMES, "basis": MARKET_BASIS, "clip": clip,
               "winsorized": {"mean_daily": stats[9], "sd_daily": stats[10], "min": stats[15], "max": stats[16]},
               "unclipped": {"mean_daily": stats[11], "sd_daily": stats[12], "min": stats[17], "max": stats[18]},
               "corr_winsorized_unclipped": stats[13], "max_abs_gap": stats[14],
               "sessions_gap_above": {"threshold": MARKET_GAP_REPORT, "count": int(stats[19]),
                                      "largest": [{"session": str(row[0]), **dict(zip(names, row[1:], strict=True))}
                                                  for row in gaps]},
               "session_2021_01_04": None if artifact is None else dict(zip(names, artifact, strict=True))}
    return finish_receipt(receipt, code, inputs, [out], payload, started)


def stage_line_month(work: Path, year: int) -> dict[str, Any]:
    """``line_month/year=Y.parquet``: per (line, month) the last observed adjusted close, observed
    sessions and daily-return counts, the overlapping 3-session log-return sums (line x EW market),
    and ``brk_n``, the vendor artifact breaks introduced in the month (ruling C-79: a ratio between
    the last bars of two months is clean when no month after the earlier one up to the later one
    has a break; a 3-session log return never spans one)."""
    started = time.perf_counter()
    out = work / "line_month" / f"year={year}.parquet"
    receipt = out.with_suffix(".json")
    code = stage_digest(stage_line_month)
    inputs = {"bars": read_json(work / "lines.json")["inputs"]["bars"],
              "market": sha256_file(work / "market.parquet"), "year": year}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    con = connect(work, f"line-month-{year}")
    try:
        con.execute(f"""
            CREATE TABLE mk AS
            SELECT sno, CASE WHEN lag(sno, 2) OVER w = sno - 2 THEN lm + lag(lm) OVER w + lag(lm, 2) OVER w END AS m3
            FROM read_parquet({sql_text((work / 'market.parquet').as_posix())}) WHERE lm IS NOT NULL
            WINDOW w AS (ORDER BY sno)
        """)
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name("." + out.name + ".tmp")
        con.execute(f"""
            COPY (
              WITH b AS (
                SELECT line_id, trade_date, sno, adj, r, va_brk_in,
                       CASE WHEN lag(sno, 3) OVER w = sno - 3 AND lag(va_brk, 3) OVER w = va_brk
                            THEN ln(adj / lag(adj, 3) OVER w) END AS x3
                FROM read_parquet({parquet_list(bars_files(work))})
                WHERE trade_date BETWEEN DATE '{year}-01-01' - INTERVAL 14 DAY AND DATE '{year}-12-31'
                WINDOW w AS (PARTITION BY line_id ORDER BY sno))
              SELECT b.line_id, last_day(b.trade_date) AS eom, max(b.sno) AS last_sno,
                     arg_max(b.adj, b.sno) AS adj_last, CAST(sum(b.va_brk_in) AS INTEGER) AS brk_n,
                     count(*) AS n_obs, count(b.r) AS n_ret,
                     count(*) FILTER (WHERE b.r > 0) AS n_pos, count(*) FILTER (WHERE b.r < 0) AS n_neg,
                     count(*) FILTER (WHERE b.x3 IS NOT NULL AND k.m3 IS NOT NULL) AS n3,
                     sum(b.x3) FILTER (WHERE k.m3 IS NOT NULL) AS s3_x,
                     sum(k.m3) FILTER (WHERE b.x3 IS NOT NULL) AS s3_m,
                     sum(b.x3 * k.m3) AS s3_xm,
                     sum(b.x3 * b.x3) FILTER (WHERE k.m3 IS NOT NULL) AS s3_xx,
                     sum(k.m3 * k.m3) FILTER (WHERE b.x3 IS NOT NULL) AS s3_mm,
                     {year} AS year
              FROM b LEFT JOIN mk k ON k.sno = b.sno
              WHERE year(b.trade_date) = {year}
              GROUP BY b.line_id, last_day(b.trade_date)
              ORDER BY eom, line_id
            ) TO {sql_text(tmp.as_posix())} (FORMAT PARQUET, COMPRESSION ZSTD)
        """)
        rows = con.execute(f"SELECT count(*), count(DISTINCT line_id) FROM read_parquet({sql_text(tmp.as_posix())})"
                           ).fetchone()
    finally:
        con.close()
        drop_scratch(work, f"line-month-{year}")
    os.replace(tmp, out)
    return finish_receipt(receipt, code, inputs, [out], {"rows": int(rows[0]), "lines": int(rows[1])}, started)


# ---------------------------------------------------------------------------
# Provisional labels (built, never joined to features: ruling R-6)
# ---------------------------------------------------------------------------

LABEL_HORIZONS = (1, 3, 6, 12)
#: Policy v4 holdout (``split.holdout_start``; :func:`label_policy` checks both against the frozen
#: policy): formations from 2024-01 are sealed, and no bar dated on or after it is read, so a
#: selection-sample window whose exit session falls in the holdout is ``not_matured`` (the 1.10
#: power-study rule: selection = formations whose label window ends before the holdout).
HOLDOUT_START = dt.date(2024, 1, 1)
LABEL_LAST_EOM = dt.date(2023, 12, 31)
LABEL_BASIS = f"vendor_adjusted_close_{_vendor_artifact.REPAIR_VERSION}"


def label_policy() -> dict[str, Any]:
    """The frozen policy v4's holdout and horizons; refuses when this module's constants disagree."""
    from .qualification import load_policy_v4

    policy = load_policy_v4()
    horizons = tuple(int(h) for h in policy.evaluation_inputs["horizons_months"])
    if policy.split.holdout_start != HOLDOUT_START or horizons != LABEL_HORIZONS:
        raise ValueError(f"policy {policy.version}: holdout {policy.split.holdout_start} / horizons {horizons} "
                         f"differ from the price wave's {HOLDOUT_START} / {LABEL_HORIZONS}")
    return {"policy_id": policy.version, "policy_sha256": policy.sha256,
            "holdout_start": HOLDOUT_START.isoformat(), "horizons": list(horizons)}


def label_windows(first: dt.date = FIRST_EOM, last: dt.date = LABEL_LAST_EOM,
                  horizons: Sequence[int] = LABEL_HORIZONS) -> list[tuple[dt.date, int, dt.date, dt.date, dt.date]]:
    """``(eom, h, formation session, entry, exit)``: entry = the close of ``next_session(eom)``; exit =
    the close of ``next_session`` of the h-th month end after ``eom`` (calendar facts, no prices)."""
    rows = []
    for eom in formation_months(first, last):
        formation = _xnys.expected_month_end_session(eom.year, eom.month)
        entry = _xnys.next_session(eom)
        for h in horizons:
            rows.append((eom, int(h), formation, entry, _xnys.next_session(add_months(eom, int(h)))))
    return rows


def label_spec(work: Path, th3: Path, horizons: Sequence[int] = LABEL_HORIZONS) -> dict[str, Any]:
    policy = label_policy()
    return {
        "kind": "price_wave_provisional_labels", "provisional": True, "holdout_start": policy["holdout_start"],
        "holdout_basis": f"policy {policy['policy_id']} split.holdout_start (sha256 {policy['policy_sha256']})",
        "horizons": [int(h) for h in horizons], "formations": [FIRST_EOM.isoformat(), LABEL_LAST_EOM.isoformat()],
        "code_digest": spine_code_digest(),
        "input_digests": {"th3_sha256": th3_sha256(th3), "bars": read_json(work / "lines.json")["inputs"]["bars"],
                          "universe": files_digest(work, "base/year=*.parquet")},
        "source_file": th3.as_posix(), "basis": LABEL_BASIS, "calendar": _xnys.CALENDAR_BASIS,
        "entry_rule": "close of calendar.next_session(eom) (the first rule session after the calendar month end)",
        "exit_rule": "close of calendar.next_session(month end h months after eom)",
        "return_rule": "adj(exit) / adj(entry) - 1 on the VA1 v2 repaired adjusted close; ret_exc not computed (NULL)",
        "terminal_rule": "a line whose last bar before the holdout start is dated before the exit session stopped "
                         "trading inside the window: ret NULL, reason terminal_pending (counted, never imputed)",
        "invalid_rule": "ruling C-79: a window whose entry and exit bars differ in va_brk (a vendor_artifact_suspect "
                        f"bar lies between them: a factor step >= x{VA_FACTOR_STEP_MIN:g} on {VA_ARTIFACT_SESSION}, a "
                        f"factor step >= x{VA_FLAT_FACTOR_STEP_MIN:g} with the raw close within "
                        f"+-{VA_FLAT_CLOSE_BAND:.0%} on any session, or a sentinel close >= "
                        f"{VA_SENTINEL_CLOSE_MIN:g} and > x{VA_SENTINEL_RATIO:g} its nearest prior close below that "
                        "floor; high closes with no prior anchor are counted as ambiguous, not inferred corrupt) is "
                        "invalid: ret NULL, counted, never filled",
        "read_window_rule": f"no bar dated on or after {policy['holdout_start']} is read; a window whose exit "
                            "session is on or after it is not_matured with no return",
        "universe": "the spine rows (every eligible line alive in the formation month with vendor earnings evidence "
                    "dated at or before the formation session, ruling C-78)",
    }


def _label_rows_sql(h: int, year: int, sealed_from: dt.date) -> str:
    """One (h, year) of label rows from ``lu``/``lw``/``lp``/``ll`` (first match wins):

    * ``not_matured``: the exit session is on or after ``sealed_from`` (outside the read window);
    * ``valid``: entry and exit sessions both carry a positive price and the same vendor artifact
      break count ``va_brk``: ``adj(exit)/adj(entry) - 1``;
    * ``invalid``: both carry a price but a vendor_artifact_suspect break lies between them
      (ruling C-79): NULL, counted, never filled;
    * ``terminal_pending``: the line's last bar before ``sealed_from`` is dated before the exit
      session (it stopped trading inside the window, or before entry): NULL, counted, never imputed;
    * ``missing_entry_bar`` / ``missing_exit_bar``: the line trades on past the window but has no
      bar on that session.
    """
    from .label_matrix import LABEL_REASONS as reason

    sealed = f"DATE '{sealed_from.isoformat()}'"
    return f"""
        SELECT u.eom, u.line_id, u.owner_id, w.entry_date, w.exit_date,
               CASE WHEN w.exit_date < {sealed} AND pe.adj > 0 AND px.adj > 0 AND pe.va_brk = px.va_brk
                    THEN px.adj / pe.adj - 1.0 END AS ret,
               CAST(NULL AS DOUBLE) AS ret_exc, {sql_text(LABEL_BASIS)} AS basis,
               CAST(CASE WHEN w.exit_date >= {sealed} THEN {reason['not_matured']}
                         WHEN pe.adj > 0 AND px.adj > 0 AND pe.va_brk = px.va_brk THEN {reason['valid']}
                         WHEN pe.adj > 0 AND px.adj > 0 THEN {reason['invalid']}
                         WHEN l.last_date IS NULL OR l.last_date < w.exit_date THEN {reason['terminal_pending']}
                         WHEN pe.adj IS NULL OR NOT pe.adj > 0 THEN {reason['missing_entry_bar']}
                         ELSE {reason['missing_exit_bar']} END AS TINYINT) AS reason
        FROM lu u
        JOIN lw w ON w.eom = u.eom AND w.h = {int(h)}
        LEFT JOIN lp pe ON pe.line_id = u.line_id AND pe.trade_date = w.entry_date
        LEFT JOIN lp px ON px.line_id = u.line_id AND px.trade_date = w.exit_date
        LEFT JOIN ll l ON l.line_id = u.line_id
        WHERE year(u.eom) = {int(year)}
        ORDER BY u.eom, u.line_id
    """


def stage_labels(work: Path, year: int, label_sha: str, root: Path) -> dict[str, Any]:
    """Write the provisional labels of one formation year (every horizon) through the label matrix."""
    from .label_matrix import LabelMatrix

    started = time.perf_counter()
    receipt = work / "labels" / f"{label_sha[:16]}-year={year}.json"
    code = spine_code_digest()
    inputs = {"label_sha": label_sha, "year": year}
    if receipt_ok(receipt, code, inputs):
        return read_json(receipt)
    windows = [w for w in label_windows() if w[0].year == year]
    if not windows:
        raise ValueError(f"no label formations in {year}")
    name = f"labels-{year}"
    con = connect(work, name)
    matrix = LabelMatrix(root)
    try:
        con.execute("CREATE TEMP TABLE lw (eom DATE, h INTEGER, formation_date DATE, entry_date DATE, exit_date DATE)")
        con.executemany("INSERT INTO lw VALUES (?, ?, ?, ?, ?)", windows)
        con.execute(f"""
            CREATE TABLE lu AS SELECT eom, line_id, CAST(NULL AS VARCHAR) AS owner_id
            FROM read_parquet({parquet_list(base_files(work, [year]))}) WHERE year(eom) = {year}
                 AND eom >= DATE '{FIRST_EOM}'
        """)
        dates = sorted({d for w in windows for d in (w[3], w[4]) if d < HOLDOUT_START})
        con.execute(f"""
            CREATE TABLE lp AS SELECT line_id, trade_date, adj, va_brk
            FROM read_parquet({parquet_list(bars_files(work))})
            WHERE trade_date < DATE '{HOLDOUT_START}' AND trade_date IN (SELECT unnest(?::DATE[]))
              AND line_id IN (SELECT line_id FROM lu)
        """, [dates])
        con.execute(f"""
            CREATE TABLE ll AS SELECT line_id, max(trade_date) AS last_date
            FROM read_parquet({parquet_list(bars_files(work))})
            WHERE trade_date < DATE '{HOLDOUT_START}' AND line_id IN (SELECT line_id FROM lu)
            GROUP BY line_id
        """)
        stats: dict[str, Any] = {"year": year, "horizons": {}}
        for h in LABEL_HORIZONS:
            reader = con.sql(_label_rows_sql(h, year, HOLDOUT_START)).fetch_arrow_reader(100_000)
            path = matrix.write(label_sha, h, year, iter(reader),
                                meta={"stage": "price_wave_provisional_labels", "year": year, "h": h})
            sidecar = read_json(path.with_suffix(".json"))
            stats["horizons"][str(h)] = {"rows": sidecar["rows"], "reasons": sidecar["reasons"],
                                         "file_sha256": sidecar["file_sha256"]}
    finally:
        con.close()
        matrix.close()
        drop_scratch(work, name)
    return finish_receipt(receipt, code, inputs, [], stats, started)
