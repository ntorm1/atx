"""Session open, high and low on the role close's adjusted basis (platform v8 lane XWQ, draft; Ruling PM7-33).

The formulaic alphas of Kakushadze (2016), "101 Formulaic Alphas" (arXiv:1601.00991), read the day's open, high and
low beside its close. The research projection carries only the close (``prepare_recent_research.COLUMNS``); these three
fields add the vendor bar of the same session, on the same price basis as the DSL's ``close``:

* ``open_adj`` / ``high_adj`` / ``low_adj``: row t = the vendor open / high / low of session t times the role's own
  adjustment of that session, close.f64[t] / raw_close.f64[t] (the role's close is the vendor raw close times the
  vendor cumulReturnFactor of the same row). So x_adj / close = x / raw close of the same vendor row, and every
  cross-session ratio of the four prices carries the same split and dividend adjustment as ``close``.

Point in time (``LAG_SESSIONS`` = 0, same session as the role's close): the vendor end-of-day row of session t carries
its open, high, low and close together and is known at session t's 22:00 UTC close mark (``TH_CLOCK``, the clock of the
role's close), before the 23:00 UTC decision of row t (the builder's ``visibility_mark``). Row t reads the vendor rows
dated session t and the role rows of row t only; nothing dated after session t is read. This matches the paper's
delay-1 convention: an alpha computed from day t's bar is traded from day t+1 (the house fills a decision at row t at
session t+1).

Cell rule: NaN unless the role marks the line present at t with a finite positive close and raw close, the vendor row
of (session t, line) is unique, and its open, high and low are finite and positive with low <= min(open, raw close)
and high >= max(open, raw close) (a bar that contradicts the role's close is withheld whole and counted).

Seal (reader side, research_window): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``;
vendor rows dated on or after the seal are skipped and counted; the role ends before the seal (asserted by the builder).

Draft and off by default: ``prepare_research_fields.py`` does not register this module (no v8 field list, formula or
producer fingerprint moves); ``prepare_research_fields_ohlc.py`` registers it. ``--reuse`` (v8 C-3 contract):
``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs``, ``entry_inputs``. The inputs are
the role (bound by the prior's role binding) and the vendor file, which must hash to the role's ``source_sha256``, so no
further input pin is needed; the producers import nothing from another field module.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import research_window as rw  # same directory: the seal
from research_fields_sec import _Host  # the builder-namespace view every field module uses (same directory)

GROUP = "ohlc"
OPTIONS = ("price_source",)          # research_fields_price.py's --price-source: no new CLI option
HOST_HANDLES = ("h",)                # v8 C-3: the producers read the builder through ``h``
LAG_SESSIONS = 0                     # row t reads the vendor bar of session t - LAG_SESSIONS (same session as close)
BAR_COLUMNS = ("open", "high", "low")
BAR_TYPES = (pa.float32(), pa.float64())
NAMES = {"open_adj": "open", "high_adj": "high", "low_adj": "low"}

OHLC_CLOCK = (
    "ohlc-same-session-v1: row t reads the vendor TickerHistory3 row of session t (its open, high and low, delivered "
    "in the end-of-day row that also carries the role's close: known at session t's 22:00 UTC close mark, TH_CLOCK) "
    "and the role's close.f64, raw_close.f64 and present.u8 of row t; nothing dated after session t; rows on or after "
    "the seal are never read")
BAR_RULE = (
    "ohlc-bar-v1: x_adj[t] = x[t] * close.f64[t] / raw_close.f64[t], x = the vendor {col} of (session t, line) "
    "widened from the source float; NaN unless present.u8[t] = 1 with finite positive close and raw close, exactly one "
    "vendor row has that (tradingDate, securityID) key (duplicates quarantined), open, high and low are finite and "
    "positive, and low <= min(open, raw close) and high >= max(open, raw close) (a violating bar is withheld whole)")
CAVEATS = ["the vendor's open, high and low (TickerHistory3), not exchange auction or consolidated prints",
           "same modeled clock as the role's close (vendor end-of-day row at the 22:00 UTC mark); the vendor's real "
           "delivery time and history vintage are the open caveats of every same-date vendor value, close included",
           "the role's adjustment (vendor cumulReturnFactor, unrepaired, as close.f64): any vendor factor artefact in "
           "close (the 2021-01-04 re-anchoring) is carried identically, so cross-session ratios equal close's",
           "one price line's values (the vendor securityID), not an issuer total"]

FIELDS: dict = {}


def _spec(name: str, col: str, formula: str):
    FIELDS[name] = {
        "group": "ohlc_bar", "point_in_time": True, "lagged": False,
        "units": f"price on the role close's adjusted basis: the session's vendor {col} x close / raw close",
        "clock": OHLC_CLOCK, "staleness": "same-session vendor bar only; no fill: an absent, duplicated, invalid or "
                                          "order-violating bar, or an absent role close -> NaN",
        "caveats": CAVEATS, "definition": BAR_RULE.format(col=col),
        "source_columns": ["tradingDate", "securityID", "open", "high", "low", "close.f64", "raw_close.f64",
                           "present.u8"],
        "formula_id": formula, "min_history": "0 sessions (row t reads session t only)", "needs": "vendor_ohlc"}


_spec("open_adj", "open", "ohlc-open-adj-v1")
_spec("high_adj", "high", "ohlc-high-adj-v1")
_spec("low_adj", "low", "ohlc-low-adj-v1")

# The producing code of the field group (contract of task C-3: {group: (entry functions,)}); compute() orchestrates.
PRODUCERS = {"ohlc_bar": ("verify_bar_source", "bar_panel", "bar_rows")}


# ---------------------------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------------------------

def require_bar_columns(path: Path) -> None:
    """Refuse (a footer read, before any output) a price source without float open, high and low columns."""
    schema = pq.ParquetFile(Path(path), memory_map=False).schema_arrow
    for c in BAR_COLUMNS:
        i = schema.get_field_index(c)
        if i < 0 or schema.field(i).type not in BAR_TYPES:
            raise ValueError(f"open_adj/high_adj/low_adj: --price-source column {c} is missing or not a float "
                             f"({Path(path)})")


def verify_bar_source(h, path: Path, role, budget):
    """Hash the price source once: it must be the vendor file the role was projected from (the role manifest's
    source_sha256). Returns its file identity, re-checked after every read."""
    path = Path(path)
    captured = h.identity(path)
    budget.report("ohlc-source-hash-start", bytes=captured[2])
    digest = h.sha_file(path, budget)
    if role.source_sha256 is None or digest != role.source_sha256:
        raise ValueError("--price-source SHA-256 differs from the role's source_sha256 (not the file the role was "
                         "projected from)")
    if h.identity(path) != captured:
        raise ValueError("--price-source changed during hashing")
    return captured


def bar_panel(h, path: Path, captured, role, budget) -> dict:
    """The vendor open, high and low of every (role session, role line) key, f32 as stored (duplicates NaN), and the
    read statistics. Rows off the role's session axis, off its lines, or dated on or after the seal are not used."""
    path = Path(path)
    pf = pq.ParquetFile(path, memory_map=False)
    require_bar_columns(path)
    seal_day = h.day_of(h.SEAL)
    days = role.days.astype(np.int64)
    first, last = int(days[0]), min(int(days[-1]), seal_day - 1)
    nd, n = role.n_dates, role.n
    budget.admit(nd * n * (3 * 4 + 2) + (32 << 20), "ohlc-bar-matrices")
    bars = {c: np.full((nd, n), np.nan, dtype=np.float32) for c in BAR_COLUMNS}
    counts = np.zeros((nd, n), dtype=np.uint16)
    st = {"rows_scanned": 0, "rows_on_or_after_seal_skipped": 0, "rows_off_calendar": 0, "rows_selected": 0}
    columns = ["tradingDate", "securityID", *BAR_COLUMNS]
    for batch in pf.iter_batches(batch_size=65536, columns=columns, use_threads=False):
        budget.check("ohlc-source-batch")
        st["rows_scanned"] += batch.num_rows
        d = pc.fill_null(batch.column("tradingDate").cast(pa.int32()), -1).to_numpy().astype(np.int64)
        st["rows_on_or_after_seal_skipped"] += int(np.count_nonzero(d >= seal_day))
        idx = np.flatnonzero((d >= first) & (d <= last))
        if not len(idx):
            continue
        sid = pc.fill_null(batch.column("securityID"), 0).to_numpy()[idx]
        pos, on = role.columns_of(sid)
        idx, j, dd = idx[on], pos[on], d[idx][on]
        t = np.minimum(np.searchsorted(days, dd), nd - 1)
        cal = days[t] == dd
        st["rows_off_calendar"] += int(np.count_nonzero(~cal))
        idx, j, t = idx[cal], j[cal], t[cal]
        if not len(idx):
            continue
        st["rows_selected"] += len(idx)
        np.add.at(counts, (t, j), 1)
        sub = batch.take(pa.array(idx, type=pa.int64()))
        for c in BAR_COLUMNS:
            bars[c][t, j] = sub.column(c).to_numpy(zero_copy_only=False).astype(np.float32)
    if h.identity(path) != captured:
        raise ValueError("--price-source changed while the bars were read")
    dup = counts > 1
    for c in BAR_COLUMNS:
        bars[c][dup] = np.nan
    st["duplicate_keys_quarantined"] = int(np.count_nonzero(dup))
    st["rule"] = BAR_RULE.format(col="open / high / low")
    return {"bars": bars, "stats": st}


def bar_rows(h, panel: dict, role, output: Path, budget, names) -> dict:
    """``open_adj`` / ``high_adj`` / ``low_adj`` (their field definitions), streamed through the role's verified close,
    raw close and presence rows."""
    bars = panel["bars"]
    rows = h.RoleRows(role, ["close.f64", "raw_close.f64", "present.u8"])
    writers = {}
    st = {"order_violation_member_cells": 0, "missing_bar_member_cells": 0}
    try:
        try:
            for x in names:
                writers[x] = h.FieldWriter(output, x, role)
            for t in range(role.n_dates):
                c, rc = rows.row("close.f64"), rows.row("raw_close.f64")
                p = rows.row("present.u8") != 0
                s = t - LAG_SESSIONS
                inside = 0 <= s < role.n_dates
                o, hi, lo = ((bars[col][s].astype(np.float64) if inside else np.full(role.n, np.nan))
                             for col in BAR_COLUMNS)
                with np.errstate(invalid="ignore", divide="ignore"):
                    ok = p & np.isfinite(c) & (c > 0) & np.isfinite(rc) & (rc > 0)
                    bar = ok & np.isfinite(o) & np.isfinite(hi) & np.isfinite(lo) & (o > 0) & (hi > 0) & (lo > 0)
                    order = (lo <= np.minimum(o, rc)) & (hi >= np.maximum(o, rc))
                    good = bar & order
                    k = c / rc
                member = role.member[t] != 0
                st["order_violation_member_cells"] += int(np.count_nonzero(bar & ~order & member))
                st["missing_bar_member_cells"] += int(np.count_nonzero(ok & ~bar & member))
                values = {"open_adj": o, "high_adj": hi, "low_adj": lo}
                for x, w in writers.items():
                    w.write(np.where(good, values[x] * k, np.nan))
                if t % 256 == 0:
                    budget.check("ohlc-write")
            sources = rows.verify()
        finally:
            rows.close()
    except BaseException:
        for w in writers.values():
            w.f.close()
        raise
    for w in writers.values():
        w.close()
    return {x: (w, sources, dict(st)) for x, w in writers.items()}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "OhlcFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_ohlc.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_ohlc: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return OhlcFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """No input pin beyond the role: the vendor file must hash to the role's source_sha256 and the prior is bound to
    the same role; the producers import no other field module's code."""
    return {}


def entry_inputs(entry: dict) -> dict:
    return {}


class OhlcFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: the fields read research_fields_price.py's --price-source (OPTIONS)."""

    @staticmethod
    def check(selected, options: dict):
        """Before any output: the price source and its open, high and low columns (a footer read)."""
        names = [f for f in selected if f in FIELDS]
        if not names:
            return
        if options.get("price_source") is None:
            raise ValueError(f"--fields: {', '.join(names)} need --price-source (the role's vendor source file)")
        require_bar_columns(Path(options["price_source"]))

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_ohlc: the builder's seal {h.SEAL.isoformat()} is not research_window's "
                               f"{rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        source = Path(options["price_source"])
        captured = verify_bar_source(h, source, role, budget)
        panel = bar_panel(h, source, captured, role, budget)
        budget.report("ohlc-source-loaded", rows_selected=panel["stats"]["rows_selected"])
        results = bar_rows(h, panel, role, output, budget, names)
        source_checks[GROUP] = {"lag_sessions": LAG_SESSIONS, "clock": OHLC_CLOCK, "seal": rw.SEAL_DATE,
                                "window": rw.WINDOW_ID, "source": panel["stats"]}
        del panel
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        source_pin = {"path": str(source.resolve()), "bytes": captured[2], "sha256": role.source_sha256}
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                               "lag_sessions": LAG_SESSIONS, "min_history": spec["min_history"], **extra}
            outcome[x] = (w, [source_pin] + sources, w.coverage())
        budget.report("ohlc-complete", fields=len(names))
