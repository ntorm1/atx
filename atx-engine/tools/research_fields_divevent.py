"""Dividend initiation and omission research field (platform v8 lane YDATA, draft): a payout-policy event family.

Michaely, Thaler and Womack (1995, JF 50(2), "Price reactions to dividend initiations and omissions: overreaction or
drift?"): after a dividend initiation prices drift up, after an omission they drift down, over the following year. The
events come from the ex-date ledger lane XDATA built from the vendor's chained factor (research_fields_xdata
``div_events``: an observed session whose factor step is a cash-distribution yield in [1 bp, 4%] and not a
factor-break-v1 jump cell), so no atx-db change and no new source: TickerHistory3, read through research_fields_price.py's
``--price-source`` (it must hash to the role manifest's ``source_sha256``).

Field ``div_init_omit`` at role row t (extended row e = prefix + t; ``LAG_SESSIONS`` = 1: only ledger rows <= e-1):
* initiation: an ex-date s in [e-252, e-1] with no ex-date of the line in [s-504, s-1] and the line first observed at
  or before s-504 (two years without a payment while trading);
* omission: L = the line's latest ex-date <= e-1; the line paid at least 3 ex-dates in [L-251, L] (a quarterly payer at
  its last payment); D = L + 126 (two quarterly payments missed) with e-252 <= D <= e-1 and the line observed at D;
* value = 1{initiation} - 1{omission} (both: 0); 0 for any other line with history; NaN when the line was first
  observed after e-1-504 (fewer than two years of ledger).

Stamping: an ex-date is stamped at its own session (the vendor applies the factor on the ex-date, after the dividend was
declared); an omission is decided at D from the absence of ex-dates through D. Row t reads ledger rows <= e-1 only.

Seal (reader side): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``; the panel skips and
counts every vendor row dated on or after it (research_fields_price.source_panel).

``--reuse`` (v8 C-3): the rule calendar of the extended axis (research_fields_price.session_calendar, review B-1) and
``imported_code`` (the AST closure of the names imported from research_fields_price.py and research_fields_xdata.py,
which no producer fingerprint follows) are the input pins.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np

import code_fingerprint  # same directory: the AST closure fingerprints --reuse keys on
import research_fields_price as price  # same directory; neither imports this module
import research_fields_xdata as xdata
import research_window as rw
from research_fields_sec import _Host

GROUP = "divevent"
OPTIONS = ("price_source",)
HOST_HANDLES = ("h",)
BUILDER = "prepare_research_fields.py"
BUILDER_ORCHESTRATION = frozenset({"run", "main"})
LAG_SESSIONS = 1
INIT_GAP_SESSIONS = 504      # two years without an ex-date before the initiating one (as recalled from MTW; verify)
EVENT_SESSIONS = 252         # MTW measure the drift over the year after the event
OMIT_GAP_SESSIONS = 126      # two quarterly payments (63 sessions apart) missed after the last ex-date
PAYER_WINDOW = 252           # the year ending at the last ex-date ...
PAYER_MIN = 3                # ... holding at least three ex-dates: a quarterly payer (XDATA DIV_MIN_PAID)
PRE_SESSIONS = EVENT_SESSIONS + INIT_GAP_SESSIONS + LAG_SESSIONS + 3

IMPORTS = ((price, ("source_panel", "verify_source", "extended_days")), (xdata, ("div_events",)))

DIVEVENT_CLOCK = ("divevent-lag1-v1: row t reads the ex-date ledger of extended sessions <= t-1 only (each ex-date row "
                  "known at its 22:00 UTC close mark); an omission is decided at the session it is detected")
DIVEVENT_RULE = (
    "mtw-div-init-omit-v1 on the research_fields_xdata ex-date ledger (hs-divseason ledger rule: an observed session "
    "whose factor step is a cash yield in [1 bp, 4%], no kept_gap step, not a factor-break-v1 jump cell). Row t, e = "
    f"its extended row: initiation = an ex-date s in [e-{EVENT_SESSIONS}, e-{LAG_SESSIONS}] with no ex-date in "
    f"[s-{INIT_GAP_SESSIONS}, s-1] and the line first observed at or before s-{INIT_GAP_SESSIONS}; omission = L, the "
    f"latest ex-date <= e-{LAG_SESSIONS}, has at least {PAYER_MIN} ex-dates in [L-{PAYER_WINDOW - 1}, L] and D = L + "
    f"{OMIT_GAP_SESSIONS} lies in [e-{EVENT_SESSIONS}, e-{LAG_SESSIONS}] with the line observed at D; value = "
    "1{initiation} - 1{omission}; NaN when the line is first observed after e-1-" + str(INIT_GAP_SESSIONS))

FIELDS: dict = {
    "div_init_omit": {
        "group": "y_divevent", "point_in_time": True, "lagged": False,
        "units": "event indicator: +1 in the year after a dividend initiation, -1 in the year after an omission is "
                 "detected, else 0",
        "clock": DIVEVENT_CLOCK,
        "staleness": f"event windows of {EVENT_SESSIONS} sessions; fewer than {INIT_GAP_SESSIONS} sessions of ledger "
                     "-> NaN",
        "caveats": ["the ledger is the vendor's cumulative factor, not a dividend file (research_fields_xdata caveats): "
                    "a cash distribution of 1 bp to 4% counts whatever its kind, and the ex-date, not the announcement, "
                    "dates an initiation",
                    "an omission is seen only when two quarterly payments are missed (126 sessions after the last "
                    "ex-date), months after the board's announcement MTW date their events by",
                    "MTW's initiation and omission definitions are recalled, not re-read: root verifies the two-year "
                    "gap and the one-year window before registration",
                    "one price line's values (the vendor securityID), not an issuer total"],
        "definition": DIVEVENT_RULE + ". " + xdata.SOURCE_NOTE,
        "source_columns": ["close", "cumulReturnFactor", "volume", "tradingDate", "securityID"],
        "formula_id": "mtw-div-init-omit-v1", "domain": (-1.0, 1.0),
        "min_history": f"{PRE_SESSIONS} extended sessions before the role start",
        "needs": "vendor_prices",
    }
}

PRODUCERS = {"y_divevent": ("divevent_rows",)}


def imported_code(group: str, sources: dict | None = None, builder: bytes | None = None) -> dict:
    """{"modules": [{"module", "names", "sha256"}]}: SHA-256 (code_fingerprint) of the AST closure of the names imported
    from each field module, with the builder definitions they read through ``h``."""
    if group not in PRODUCERS:
        raise ValueError(f"research_fields_divevent: unknown producer group {group!r}")
    host = (Path(__file__).resolve().parent / BUILDER).read_bytes() if builder is None else builder
    out = []
    for module, names in IMPORTS:
        path = Path(module.__file__)
        src = path.read_bytes() if sources is None or path.name not in sources else sources[path.name]
        fp = code_fingerprint.fingerprints(src.replace(b"\r\n", b"\n"), {group: names},
                                           host=code_fingerprint.Host(host.replace(b"\r\n", b"\n"),
                                                                      module.HOST_HANDLES, BUILDER_ORCHESTRATION))[group]
        if fp is None:
            raise ValueError(f"research_fields_divevent: {path.name} lacks one of {names}")
        out.append({"module": path.name, "names": list(names), "sha256": fp})
    return {"modules": out}


def event_matrices(ev: np.ndarray, first: np.ndarray, observed: np.ndarray) -> dict:
    """From the ledger (extended rows x lines): cumulative ex-date counts, the latest ex-date at or before each row,
    the initiating ex-dates (``DIVEVENT_RULE``) and their cumulative count, and the observation mask."""
    n_ext, n = ev.shape
    cum = np.cumsum(ev, axis=0, dtype=np.int32)
    rows = np.arange(n_ext, dtype=np.int32)[:, None]
    last = np.maximum.accumulate(np.where(ev, rows, -1), axis=0)
    init = np.zeros_like(ev)
    s = np.arange(INIT_GAP_SESSIONS + 1, n_ext)
    if len(s):
        gap = cum[s - 1] - cum[s - 1 - INIT_GAP_SESSIONS]          # ex-dates in [s-504, s-1]
        init[s] = ev[s] & (gap == 0) & (first[None, :] <= (s - INIT_GAP_SESSIONS)[:, None])
    return {"cum": cum, "last": last, "init_cum": np.cumsum(init, axis=0, dtype=np.int32), "observed": observed,
            "first": first}


def row_value(m: dict, e: int) -> np.ndarray:
    """``div_init_omit`` at extended row e (f64, NaN for short history)."""
    vis = e - LAG_SESSIONS
    cum, n = m["cum"], m["cum"].shape[1]
    lo = e - EVENT_SESSIONS
    init = (m["init_cum"][vis] - (m["init_cum"][lo - 1] if lo >= 1 else 0)) > 0
    L = m["last"][vis]
    has = L >= 0
    Ls = np.maximum(L, 0)
    before = np.where(Ls - PAYER_WINDOW >= 0, cum[np.maximum(Ls - PAYER_WINDOW, 0), np.arange(n)], 0)
    payer = has & ((cum[Ls, np.arange(n)] - before) >= PAYER_MIN)
    D = Ls + OMIT_GAP_SESSIONS
    inside = payer & (D >= lo) & (D <= vis)
    seen = m["observed"][np.minimum(D, len(cum) - 1), np.arange(n)]
    omit = inside & seen
    history = m["first"] <= vis - INIT_GAP_SESSIONS
    return np.where(history, init.astype(np.float64) - omit.astype(np.float64), np.nan)


def divevent_rows(h, panel: dict, role, output: Path, budget) -> dict:
    """``div_init_omit`` (its field definition)."""
    n, pre = role.n, panel["prefix"]
    n_ext = len(panel["days"])
    budget.admit(n_ext * role.n * 16 + (16 << 20), "divevent-ledger")
    ev, first, st = xdata.div_events(h, panel)
    m = event_matrices(ev, first, np.isfinite(panel["F"]) & np.isfinite(panel["C"]))
    del ev
    w = h.FieldWriter(output, "div_init_omit", role)
    counts = {"initiation_member_cells": 0, "omission_member_cells": 0, "short_history_member_cells": 0}
    try:
        for t in range(role.n_dates):
            row = row_value(m, pre + t)
            member = role.member[t] != 0
            counts["initiation_member_cells"] += int(np.count_nonzero(member & (row > 0)))
            counts["omission_member_cells"] += int(np.count_nonzero(member & (row < 0)))
            counts["short_history_member_cells"] += int(np.count_nonzero(member & np.isnan(row)))
            w.write(row)
            if t % 256 == 0:
                budget.check("divevent-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    return {"div_init_omit": (w, [panel["source"]], {"ledger": st, **counts})}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "DivEventFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_ydata.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_divevent: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return DivEventFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """The rule calendar of the extended axis (review B-1) and the imported code."""
    return {"session_calendar": price.session_calendar(), "imported_code": imported_code(producer_group(name))}


def entry_inputs(entry: dict) -> dict:
    return {"session_calendar": entry.get("session_calendar"), "imported_code": entry.get("imported_code")}


class DivEventFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: the field reads research_fields_price.py's --price-source (OPTIONS)."""

    @staticmethod
    def check(selected, options: dict):
        names = [f for f in selected if f in FIELDS]
        if names and options.get("price_source") is None:
            raise ValueError(f"--fields: {', '.join(names)} need --price-source (the role's vendor source file)")

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_divevent: the builder's seal {h.SEAL.isoformat()} is not "
                               f"research_window's {rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        source = Path(options["price_source"])
        verified = price.verify_source(h, source, role, budget)
        panel = price.source_panel(h, source, verified, role, budget, pre_sessions=PRE_SESSIONS, lookback_days=0,
                                   shares=False, open_=False)
        results = divevent_rows(h, panel, role, output, budget)
        source_checks[GROUP] = {"lag_sessions": LAG_SESSIONS, "seal": rw.SEAL_DATE, "window": rw.WINDOW_ID,
                                "source": panel["stats"]}
        del panel
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        calendar = price.session_calendar()
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                               "lag_sessions": LAG_SESSIONS, "min_history": spec["min_history"],
                               "domain": list(spec["domain"]), **extra, "session_calendar": dict(calendar),
                               "imported_code": imported_code(spec["group"])}
            outcome[x] = (w, sources, w.coverage())
        budget.report("divevent-complete", fields=len(names))
