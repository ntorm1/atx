"""Pending-merger target research field (platform v8 lane YDATA, draft): the risk-arbitrage return source.

Item 4 of lane YDATA: the top in-house data that only lacks a reader. The atx-db ``sec_filings`` stage (schema
``atx.alpha-panel.sec-filings/v1``, documented in atx-db/docs/ALPHA_PANEL_SEC.md on ``origin/main``: ``filings.parquet``
"one row per (cik, accession) ... A filing appears once per CIK it is filed under") already reaches the engine through
the SEC field module's pins (``--sec-stages``, ``--sec-filings-sha256``, ``--sec-identity-bridge``); no v8 field reads
its merger forms. No new option, no atx-db change.

Field ``deal_pending``: 1 while the line's issuer is the target of a pending merger or tender offer, else 0.
* Deal filings (target side): an original ``PREM14A`` / ``DEFM14A`` (merger proxy), ``PREM14C`` / ``DEFM14C`` (merger
  information statement), ``SC 14D9`` (the subject company's tender-offer recommendation) or ``SC14D9C`` (its
  pre-commencement communication) under the CIK. A merger proxy filed by a CIK that also filed an original ``S-4`` /
  ``S-4EF`` within the 365 days before it (visible by then) is the share-issuing acquirer's, not a target's: dropped.
* Each deal filing opens a window from its usable session u (``SEC_CLOCK``: available_at < 22:00 UTC of session u-1)
  to the earlier of the usable session of the CIK's first later resolution filing (``25``, ``25-NSE``, ``15-12B``,
  ``15-12G``, ``15-15D``, ``15F-12B``, ``15F-12G``, ``15F-15D``: delisting or deregistration, i.e. the deal closed) and
  u + 252 sessions (a deal unresolved after a year of sessions is no longer treated as pending). The value is 1 while
  any window of the CIK is open.
* NaN unless the line's primary link is a domestic periodic filer at t (research_fields_v9 ``NT_PRESENT``: an original
  10-K / 10-Q family filing visible within 400 days), as ``nt_first_126``.

Return source (not one of the 12 v8 themes): the merger-arbitrage spread. Long pending targets earns the deal-completion
risk premium (Mitchell and Pulvino 2001, JF, "Characteristics of risk and return in risk arbitrage"; Baker and
Savasoglu 2002, JFE, "Limited arbitrage in mergers and acquisitions"), as recalled; root verifies before registration.

Seal (reader side): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``; rows available on or
after ``SEAL_NS`` are dropped and counted (the stage file is one file, read in batches as the v9 NT field reads it).

``--reuse`` (v8 C-3): the stage manifest and SEC bridge pins, and ``imported_code`` (the AST closure of the names
imported from research_fields_sec.py and research_fields_v9.py, which no producer fingerprint follows).
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc

import code_fingerprint  # same directory: the AST closure fingerprints --reuse keys on
import research_fields_sec  # same directory; neither imports this module
import research_fields_v9
import research_window as rw
from research_fields_sec import _Host, Calendar, LINK_NOTE, Latest, SEC_CLOCK, STAGES, Windowed, cik_positions
from research_fields_v9 import NT_PRESENCE_DAYS, NT_PRESENT, PERIODIC_FORMS, form_codes, stage_batches

GROUP = "deals"
OPTIONS = ("sec_stages", "sec_identity_bridge", "sec_identity_bridge_sha256", "sec_filings_sha256")
HOST_HANDLES = ("h",)
BUILDER = "prepare_research_fields.py"
BUILDER_ORCHESTRATION = frozenset({"run", "main"})
STAGE = "sec_filings"
DEAL_FORMS = ("PREM14A", "DEFM14A", "PREM14C", "DEFM14C", "SC 14D9", "SC14D9C")
PROXY_FORMS = ("PREM14A", "DEFM14A", "PREM14C", "DEFM14C")       # can be an acquirer's (share issuance vote)
ISSUER_FORMS = ("S-4", "S-4EF")                                   # registration of shares issued in the combination
RESOLUTION_FORMS = ("25", "25-NSE", "15-12B", "15-12G", "15-15D", "15F-12B", "15F-12G", "15F-15D")
ALL_FORMS = DEAL_FORMS + ISSUER_FORMS + RESOLUTION_FORMS + PERIODIC_FORMS
DEAL_MAX_SESSIONS = 252          # one year of sessions: past a merger agreement's usual outside date
ACQUIRER_LOOKBACK_DAYS = 365     # an S-4 of the same CIK within a year before its merger proxy: the acquirer's proxy
FILING_COLS = ("cik", "form", "is_amendment", "available_at")
LAG_SESSIONS = 1

# The names the producer imports from other field modules: their closure is an input pin (imported_code).
IMPORTS = ((research_fields_sec, ("Calendar", "Latest", "Windowed", "cik_positions")),
           (research_fields_v9, ("stage_batches", "form_codes")))

DEAL_RULE = (
    "sec-deal-target-pending-v1 on the sec_filings stage's filings.parquet: forms upper-cased and trimmed, originals "
    "only (is_amendment false and no '/A'); deal filings = " + ", ".join(DEAL_FORMS) + " under the CIK, except a "
    f"{', '.join(PROXY_FORMS)} whose CIK filed an original {' or '.join(ISSUER_FORMS)} with available_at in "
    f"[its available_at - {ACQUIRER_LOOKBACK_DAYS} days, its available_at] (the acquirer's proxy); a deal filing "
    "usable from session u (SEC_CLOCK) is open on [u, min(u_r, u + "
    f"{DEAL_MAX_SESSIONS})), u_r = the usable session of the CIK's first resolution filing (" +
    ", ".join(RESOLUTION_FORMS) + ") with a later available_at; value at t = 1 if any deal filing of the line's CIK is "
    "open at t, else 0")
DEAL_CAVEATS = [
    "a deal filing is a filing under the CIK: a merger proxy of a CIK that is both target and acquirer, or an "
    "acquirer's proxy without its own S-4 in the prior year, is read as a target's",
    "withdrawn or terminated deals are not detected: their window lasts to the first resolution filing or "
    f"{DEAL_MAX_SESSIONS} sessions",
    "a Form 25 / 15 for another class of the issuer's securities (preferred, notes) ends the window early",
    "the clock is the stage's available_at: the EDGAR acceptance (acceptance-per-file-clock-v1), else filing_date + 1 "
    "day 00:00 America/New_York (available_basis filing_date_eod_et)",
    LINK_NOTE]

FIELDS: dict = {
    "deal_pending": {
        "group": "y_deal", "point_in_time": True, "lagged": False, "stages": [STAGE],
        "units": "indicator: 1 while the issuer is the target of a pending merger or tender offer",
        "clock": SEC_CLOCK, "staleness": NT_PRESENT, "definition": DEAL_RULE,
        "source_columns": [f"filings.parquet:{c}" for c in FILING_COLS], "caveats": DEAL_CAVEATS,
        "formula_id": "sec-deal-target-pending-v1", "domain": (0.0, 1.0),
        "min_history": f"stage from 2009-01-01: the {ACQUIRER_LOOKBACK_DAYS}-day acquirer lookback and the "
                       f"{NT_PRESENCE_DAYS}-day presence lookback are complete from 2010-02-05",
    }
}

PRODUCERS = {"y_deal": ("deal_rows",)}


def imported_code(group: str, sources: dict | None = None, builder: bytes | None = None) -> dict:
    """{"modules": [{"module", "names", "sha256"}]}: SHA-256 (code_fingerprint) of the AST closure of the names imported
    from each field module, with the builder definitions they read through ``h``. ``sources`` / ``builder`` replace the
    files read (tests)."""
    if group not in PRODUCERS:
        raise ValueError(f"research_fields_deals: unknown producer group {group!r}")
    host = (Path(__file__).resolve().parent / BUILDER).read_bytes() if builder is None else builder
    out = []
    for module, names in IMPORTS:
        path = Path(module.__file__)
        src = path.read_bytes() if sources is None or path.name not in sources else sources[path.name]
        fp = code_fingerprint.fingerprints(src.replace(b"\r\n", b"\n"), {group: names},
                                           host=code_fingerprint.Host(host.replace(b"\r\n", b"\n"), HOST_HANDLES,
                                                                      BUILDER_ORCHESTRATION))[group]
        if fp is None:
            raise ValueError(f"research_fields_deals: {path.name} lacks one of {names}")
        out.append({"module": path.name, "names": list(names), "sha256": fp})
    return {"modules": out}


# ---------------------------------------------------------------------------------------------------------------
# Pure helpers (unit-tested directly)
# ---------------------------------------------------------------------------------------------------------------

def acquirer_proxies(cidx: np.ndarray, avail: np.ndarray, kind: np.ndarray, lookback_ns: int) -> np.ndarray:
    """Per row: True for a PROXY_FORMS row whose CIK has an ISSUER_FORMS row with available_at in
    [avail - lookback_ns, avail]. ``kind`` indexes ALL_FORMS."""
    proxy = np.isin(kind, [ALL_FORMS.index(f) for f in PROXY_FORMS])
    issuer = np.isin(kind, [ALL_FORMS.index(f) for f in ISSUER_FORMS])
    out = np.zeros(len(cidx), dtype=bool)
    if not proxy.any() or not issuer.any():
        return out
    ic, ia = cidx[issuer], avail[issuer]
    o = np.lexsort((ia, ic))
    ic, ia = ic[o], ia[o]
    for i in np.flatnonzero(proxy):
        lo = np.searchsorted(ic, cidx[i], side="left")
        hi = np.searchsorted(ic, cidx[i], side="right")
        if hi > lo:
            a = ia[lo:hi]
            out[i] = bool(np.any((a >= avail[i] - lookback_ns) & (a <= avail[i])))
    return out


def deal_windows(cidx: np.ndarray, avail: np.ndarray, deal: np.ndarray, resolution: np.ndarray, cal) -> tuple:
    """(enter, leave, cik index) of every deal row's window (``DEAL_RULE``) on the calendar's extended axis."""
    rc, ra = cidx[resolution], avail[resolution]
    o = np.lexsort((ra, rc))
    rc, ra = rc[o], ra[o]
    dc, da = cidx[deal], avail[deal]
    enter = cal.usable_from(da)
    leave = enter + DEAL_MAX_SESSIONS
    for i in range(len(dc)):
        lo = np.searchsorted(rc, dc[i], side="left")
        hi = np.searchsorted(rc, dc[i], side="right")
        later = ra[lo:hi][ra[lo:hi] > da[i]]
        if len(later):
            leave[i] = min(int(leave[i]), int(cal.usable_from(later[:1])[0]))
    return enter, leave, dc


# ---------------------------------------------------------------------------------------------------------------
# The producer
# ---------------------------------------------------------------------------------------------------------------

def deal_filings(h, directory: Path, manifest: dict, ciks: np.ndarray, end_ns: int, budget) -> tuple:
    """The rows of ALL_FORMS (originals) of linked CIKs available before the role's last mark and the seal: (cik index,
    available_at, form kind), their source and checks."""
    st: dict = {"rows_total": 0, "rows_other_form": 0, "rows_amendment": 0, "rows_sealed": 0, "rows_after_role": 0,
                "rows_unlinked_cik": 0}
    parts: dict = {"c": [], "a": [], "k": []}
    src: dict = {}
    for batch in stage_batches(h, directory, manifest, "filings.parquet", FILING_COLS, budget, src):
        tb = pa.Table.from_batches([batch])
        st["rows_total"] += tb.num_rows
        kind, suffix = form_codes(h, tb, ALL_FORMS)
        rows = np.flatnonzero(kind >= 0)
        st["rows_other_form"] += tb.num_rows - len(rows)
        if not len(rows):
            continue
        tb, kind, suffix = tb.take(pa.array(rows)), kind[rows], suffix[rows]
        amend = pc.fill_null(h.column_of(tb, "is_amendment"), True).to_numpy(zero_copy_only=False) | suffix
        avail = h.as_instants_ns(h.column_of(tb, "available_at"), "filings.parquet available_at")
        pos, linked = cik_positions(ciks, h.as_ids(h.column_of(tb, "cik"), "filings.parquet cik"))
        sealed = avail >= h.SEAL_NS
        late = ~sealed & (avail >= end_ns)
        st["rows_amendment"] += int(np.count_nonzero(amend))
        st["rows_sealed"] += int(np.count_nonzero(~amend & sealed))
        st["rows_after_role"] += int(np.count_nonzero(~amend & late))
        st["rows_unlinked_cik"] += int(np.count_nonzero(~amend & ~sealed & ~late & ~linked))
        keep = np.flatnonzero(~amend & ~sealed & ~late & linked)
        parts["c"].append(pos[keep])
        parts["a"].append(avail[keep])
        parts["k"].append(kind[keep])
        budget.check("deals-filings-batch")
    cat = lambda xs: np.concatenate(xs).astype(np.int64) if xs else np.empty(0, dtype=np.int64)
    c, a, k = cat(parts["c"]), cat(parts["a"]), cat(parts["k"])
    o = np.lexsort((k, a))
    c, a, k = c[o], a[o], k[o]
    st["rows_used"] = {f: int(np.count_nonzero(k == i)) for i, f in enumerate(ALL_FORMS) if np.any(k == i)}
    return (c, a, k), src, st


def deal_rows(h, role, output: Path, budget, options: dict) -> tuple:
    """``deal_pending`` (its field definition): ({name: (writer, sources, extras)}, source checks)."""
    nd, n = role.n_dates, role.n
    budget.admit(nd * n * 5 + (128 << 20), "deals-link-matrix")
    links, bridge_src, bridge_st = h.load_bridge(Path(options["sec_identity_bridge"]),
                                                 options["sec_identity_bridge_sha256"], role, budget)
    marks = role.days * h.DAY_NS + h.MARK_NS
    ciks, link, primary, link_st = h.resolve_links(links, role, marks, budget)
    del links
    cal = Calendar(role, h.DAY_NS, h.MARK_NS)
    end_ns = int(marks[-1])
    directory = Path(options["sec_stages"]) / STAGES[STAGE][0]
    manifest, man_src = h.pinned_manifest(directory, options["sec_filings_sha256"], STAGE.replace("_", "-"),
                                          STAGES[STAGE][1])
    (c, a, k), f_src, st = deal_filings(h, directory, manifest, ciks, end_ns, budget)
    is_deal = np.isin(k, [ALL_FORMS.index(f) for f in DEAL_FORMS])
    acquirer = acquirer_proxies(c, a, k, ACQUIRER_LOOKBACK_DAYS * h.DAY_NS)
    target = is_deal & ~acquirer
    resolution = np.isin(k, [ALL_FORMS.index(f) for f in RESOLUTION_FORMS])
    enter, leave, dc = deal_windows(c, a, target, resolution, cal)
    window = Windowed(enter, leave, dc, np.ones(len(dc), dtype=np.int64), len(ciks), np.int64)
    periodic = np.isin(k, [ALL_FORMS.index(f) for f in PERIODIC_FORMS])
    pc_, pa_ = c[periodic], a[periodic]
    latest = Latest(cal.usable_from(pa_), pc_, len(ciks))
    st.update(deal_filings=int(np.count_nonzero(is_deal)), acquirer_proxies_dropped=int(np.count_nonzero(acquirer)),
              target_windows=int(len(dc)), windows_closed_by_resolution=int(np.count_nonzero(
                  leave < enter + DEAL_MAX_SESSIONS)))
    budget.report("deals-loaded", target_windows=int(len(dc)))
    reasons = {"not_primary_link": 0, "absent_or_stale": 0}
    flagged = 0
    w = h.FieldWriter(output, "deal_pending", role)
    try:
        for t in range(nd):
            u = t + cal.prefix
            lk = link[t]
            safe = np.maximum(lk, 0)
            pline = (lk >= 0) & primary[t]
            kk = np.where(pline, latest.at(u)[safe] if len(ciks) else -1, -1)
            present = np.zeros(n, dtype=bool)
            if len(pa_):
                cutoff = int(cal.marks[u - 1]) - NT_PRESENCE_DAYS * h.DAY_NS
                present = (kk >= 0) & (pa_[np.maximum(kk, 0)] >= cutoff)
            row = np.where(present, (window.at(u)[safe] > 0).astype(np.float64), np.nan)
            w.write(row)
            member = role.member[t] != 0
            reasons["not_primary_link"] += int(np.count_nonzero(member & ~pline))
            reasons["absent_or_stale"] += int(np.count_nonzero(member & pline & ~present))
            flagged += int(np.count_nonzero(member & (row == 1.0)))
            if t % 256 == 0:
                budget.check("deals-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    extras = {"lag_sessions": LAG_SESSIONS, "nan_rule": NT_PRESENT,
              "stage_manifests": {STAGE: {"path": man_src["path"], "sha256": man_src["sha256"]}},
              "identity_bridge_manifest_sha256": bridge_src[0]["sha256"],
              "nan_reasons_member_cells": reasons, "flagged_member_cells": flagged}
    sources = list(bridge_src) + [man_src, f_src]
    checks = {"clock": SEC_CLOCK, "calendar": cal.stats, "identity_bridge": {**bridge_st, **link_st}, "filings": st}
    return {"deal_pending": (w, sources, extras)}, checks


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "DealsFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_ydata.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_deals: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return DealsFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins: the stage manifest, the SEC bridge and the imported code."""
    lower = lambda k: str(options.get(k) or "").lower()
    return {STAGE: lower(f"{STAGE}_sha256"), "sec_identity_bridge": lower("sec_identity_bridge_sha256"),
            "imported_code": imported_code(producer_group(name))}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    stages = entry.get("stage_manifests") if isinstance(entry.get("stage_manifests"), dict) else {}
    return {STAGE: (stages.get(STAGE) or {}).get("sha256"),
            "sec_identity_bridge": entry.get("identity_bridge_manifest_sha256"),
            "imported_code": entry.get("imported_code")}


class DealsFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: the field reads the SEC module's --sec-stages, --sec-identity-bridge(-sha256) and
        --sec-filings-sha256 (OPTIONS)."""

    @staticmethod
    def check(selected, options: dict):
        """Before any output: the SEC stage root, its sec_filings pin and the SEC bridge are given."""
        names = [x for x in FIELDS if x in selected]
        if not names:
            return
        missing = ["--" + k.replace("_", "-") for k in OPTIONS if options.get(k) in (None, "")]
        if missing:
            raise ValueError(f"--fields: {', '.join(names)} need {', '.join(missing)} (run() callers pass them in "
                             "module_options)")

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_deals: the builder's seal {h.SEAL.isoformat()} is not "
                               f"research_window's {rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        results, checks = deal_rows(h, role, output, budget, options)
        source_checks[GROUP] = checks
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                               "min_history": spec["min_history"], "domain": list(spec["domain"]), **extra,
                               "imported_code": imported_code(spec["group"])}
            outcome[x] = (w, sources, w.coverage())
        budget.report("deals-complete", fields=len(names))
