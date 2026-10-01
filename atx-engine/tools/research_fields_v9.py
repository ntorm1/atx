"""Platform v9 draft research fields (lane FIELDS-V9, ruling PM5-13): ``nt_first_126``, the field the library v9
draft candidate C-3 ``nt_late`` waits on (spec F-L1 of docs/plans/2026-10-01-v9-library-draft.md, section 3).

Draft and off by default. ``prepare_research_fields.py`` does not register this module, so the plain builder, every
v8 field list (fields v9 to v12), every existing field's formula and every existing producer fingerprint are exactly as
before. ``prepare_research_fields_draft.py`` registers it into the builder (``bind`` appends ``FIELDS`` to the builder's
``ALL_FIELDS`` after every field registered before it; the draft entry appends the module object to
``FIELD_MODULES``) and runs the builder's own ``main``: fields v13 (draft) = fields v12 + its ``FIELDS_V13_DRAFT``.
Promoting the module later is the two-line hook ``import research_fields_v9 as _v9`` /
``FIELD_MODULES.append(_v9.bind(globals()))`` at the end of the builder, as research_fields_v8.py's (a module-level
``ALL_FIELDS.update(...)`` would join the SEC module's host closure and move its fingerprint).

Fields:
* ``nt_first_126`` (F-L1): 1 while a first late-filing notice (original NT 10-K or NT 10-Q, none of the CIK's NT forms
  in the prior 365 days) became usable within the last 126 sessions, else 0; NaN unless the CIK is a domestic periodic
  filer (an original 10-K / 10-Q family form visible within 400 days). Inputs: the atx-db ``sec_filings`` stage that the
  8-K fields already pin (``--sec-stages``, ``--sec-filings-sha256``): ``events.parquet`` (the notices) and
  ``filings.parquet`` (the periodic-filer presence), and the SEC identity bridge (``--sec-identity-bridge``). Clock and
  session assignment: research_fields_sec.py's ``SEC_CLOCK`` and its ``Calendar`` (a row is usable at session t iff its
  available_at < 22:00 UTC of session t-1: a notice accepted after the close of d is usable from d+2).

Seal: every source row with available_at on or after the builder's ``SEAL_NS`` (research_window.py) is dropped
before use and counted; the stage files are single files, so they are opened whole and filtered by the reader, as the
8-K fields read ``eight_k_items.parquet``.

``--reuse`` (v8 C-3 contract): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs`` and
``entry_inputs``; every computed entry records ``producer`` (this module's code identity). The producer calls code
imported from research_fields_sec.py, which no producer fingerprint covers (the fingerprint follows no import; review
B-1 of F-1): the AST closure of those imported names, with the builder definitions they read through ``h``, is an
input pin of each entry (``imported_code``), so an edit there recomputes the field.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import code_fingerprint  # same directory: the AST closure fingerprints --reuse keys on
import research_fields_sec  # same directory; neither imports this module
from research_fields_sec import (_Host, Calendar, LINK_NOTE, Latest, SEC_CLOCK, STAGES, Windowed, accession_keys,
                                 cik_positions)

GROUP = "v9"
OPTIONS = ("sec_stages", "sec_identity_bridge", "sec_identity_bridge_sha256", "sec_filings_sha256")
HOST_HANDLES = ("h",)
BUILDER = "prepare_research_fields.py"            # the builder these producers are bound into (same directory)
BUILDER_ORCHESTRATION = frozenset({"run", "main"})  # its PRODUCER_ORCHESTRATION (never followed by a fingerprint)

# ---- F-L1 nt_first_126 ----------------------------------------------------------------------------------------------
NT_STAGE = "sec_filings"
NT_FORMS = ("NT 10-K", "NT 10-Q")                 # the notices that flag
NT_ANY_FORMS = ("NT 10-K", "NT 10-Q", "NT 20-F")  # every NT form of the stage (atx-db FORM_EVENTS): 'first' lookback
NT_WINDOW = 126
NT_FIRST_DAYS = 365
NT_PRESENCE_DAYS = 400
NT_LAG_SESSIONS = 1
# atx-db sec_filings REGIME_FORMS of the 'domestic' regime: the original periodic forms of a domestic filer
PERIODIC_FORMS = ("10-K", "10-Q", "10-KT", "10-QT", "10-K405", "10-KSB", "10-QSB", "10-KSB40", "10-KT405")
NT_COLS = ("cik", "accession", "form", "is_amendment", "available_at")
FILING_COLS = ("cik", "form", "is_amendment", "available_at")
STAGE_BATCH_ROWS = 1 << 18
NT_LAG = ("notice usable from the session after the first session whose 22:00 UTC mark follows its acceptance (a "
          "notice accepted after 22:00 UTC on d is usable from d+2)")
NT_MIN_HISTORY = (f"stage from 2009-01-01: the {NT_FIRST_DAYS}-day first lookback is complete from 2010-01-01, the "
                  f"{NT_PRESENCE_DAYS}-day presence lookback from 2010-02-05")

NT_RULE = (
    "sec-nt-first365-126-v1 on the sec_filings stage: notices = events.parquet rows whose form, upper-cased and "
    "trimmed, is 'NT 10-K', 'NT 10-Q' or 'NT 20-F' with is_amendment false (a null is_amendment or a '/A' form is an "
    "amendment: dropped), one row per (cik, accession) with the later available_at if rows disagree; a notice is first "
    f"when no other notice of the same CIK has available_at in [available_at - {NT_FIRST_DAYS} days, available_at); a "
    "first NT 10-K or NT 10-Q is usable from session e = the session after the first session whose 22:00 UTC mark "
    f"follows its available_at (SEC_CLOCK); value at t = 1 if a first NT 10-K or NT 10-Q of the line's CIK has e <= t "
    f"< e + {NT_WINDOW}, else 0")
NT_PRESENT = (
    "NaN unless the CIK is a domestic periodic filer at t: the stage's filings.parquet holds an original (is_amendment "
    f"false) {', '.join(PERIODIC_FORMS)} of the CIK (atx-db filer regime 'domestic') with available_at < the 22:00 UTC "
    f"mark of session t-1 and >= that mark - {NT_PRESENCE_DAYS} days; so NT 20-F, 20-F and 40-F filers are NaN; a "
    "secondary (J) or unlinked line -> NaN")
NT_CAVEATS = [
    "Form 12b-25 (NT 10-K / NT 10-Q) announces a late periodic report: the flag does not say whether the report then "
    "arrived within the 5- or 15-day grace period",
    "the 365-day first lookback and the 400-day presence lookback read the stage from its first filing date "
    "(2009-01-01): a notice before 2010-01-01 can be called first for want of history (no 4-year role reaches it)",
    "the clock is the stage's available_at: the EDGAR acceptance (acceptance-per-file-clock-v1), else filing_date + 1 "
    "day 00:00 America/New_York (available_basis filing_date_eod_et)",
    "co-registrant notices count for each registrant CIK",
    LINK_NOTE]

FIELDS: dict = {
    "nt_first_126": {
        "group": "v9_nt", "point_in_time": True, "lagged": False, "stages": [NT_STAGE],
        "units": f"indicator: 1 when a first NT 10-K / NT 10-Q notice became usable in the last {NT_WINDOW} sessions",
        "clock": SEC_CLOCK,
        "staleness": NT_PRESENT,
        "definition": NT_RULE,
        "source_columns": [f"events.parquet:{c}" for c in NT_COLS] + [f"filings.parquet:{c}" for c in FILING_COLS],
        "caveats": NT_CAVEATS,
        "formula_id": "sec-nt-first365-126-v1",
        "lag": NT_LAG,
        "min_history": NT_MIN_HISTORY,
    },

}
# The producing code of each field group (contract of task C-3: {group: (entry names,)}); compute() orchestrates.
PRODUCERS = {"v9_nt": ("nt_rows",)}
# The names each producer group imports from another field module (their closure is an input pin, imported_code).
IMPORTS: dict = {
    "v9_nt": (research_fields_sec, ("Calendar", "Latest", "Windowed", "accession_keys", "cik_positions"))}


# ---------------------------------------------------------------------------------------------------------------
# Imported code: the --reuse input pin of what the producers import
# ---------------------------------------------------------------------------------------------------------------

def imported_code(group: str, source: bytes | None = None, builder: bytes | None = None) -> dict:
    """{"module", "names", "sha256"}: SHA-256 (code_fingerprint) of the AST closure of the names producer group
    ``group`` imports from another field module, plus the closure of every builder definition they read through
    ``h``. ``source`` / ``builder`` replace the files read (tests)."""
    module, names = IMPORTS[group]
    path = Path(module.__file__)
    src = path.read_bytes() if source is None else source
    host = (Path(__file__).resolve().parent / BUILDER).read_bytes() if builder is None else builder
    fp = code_fingerprint.fingerprints(src.replace(b"\r\n", b"\n"), {group: names},
                                       host=code_fingerprint.Host(host.replace(b"\r\n", b"\n"), HOST_HANDLES,
                                                                  BUILDER_ORCHESTRATION))[group]
    if fp is None:
        raise ValueError(f"research_fields_v9: {path.name} lacks one of {names}")
    return {"module": path.name, "names": list(names), "sha256": fp}


# ---------------------------------------------------------------------------------------------------------------
# F-L1 nt_first_126
# ---------------------------------------------------------------------------------------------------------------

def stage_batches(h, directory: Path, manifest: dict, rel: str, columns, budget, source: dict):
    """Record batches of ``columns`` of a manifest-listed stage file. The file is hashed as a stream and checked
    against its manifest entry (bytes, SHA-256) before it is parsed, and must keep its identity (device, inode, size,
    mtime) from before the hash until the last batch (the builder's TickerHistory rule): a large stage file is never
    held in memory. ``source`` receives the file's source record."""
    entry = (manifest.get("files") or {}).get(rel)
    if not isinstance(entry, dict):
        raise ValueError(f"{directory.name}: manifest does not list {rel}")
    path = directory / rel
    captured = h.identity(path)
    digest = h.sha_file(path, budget)
    if captured[2] != int(entry.get("bytes", -1)) or digest != entry.get("sha256"):
        raise ValueError(f"{directory.name}: {rel} does not match its manifest entry")
    with pq.ParquetFile(path, memory_map=False) as pf:
        missing = [c for c in columns if c not in pf.schema_arrow.names]
        if missing:
            raise ValueError(f"{directory.name}/{rel} lacks contract column(s) {missing}")
        source.update(path=str(path.resolve()), bytes=int(captured[2]), sha256=digest)
        yield from pf.iter_batches(batch_size=STAGE_BATCH_ROWS, columns=list(columns), use_threads=False)
        if h.identity(path) != captured:
            raise ValueError(f"{directory.name}: {rel} changed while it was read")


def form_codes(h, tb, forms) -> tuple:
    """(index into ``forms`` of the upper-cased, trimmed form without a '/A' suffix, or -1; whether it had the
    suffix)."""
    form = pc.utf8_upper(pc.utf8_trim_whitespace(h.as_text(h.column_of(tb, "form"))))
    base = pc.replace_substring_regex(form, pattern="/A$", replacement="")
    code = pc.fill_null(pc.index_in(base, value_set=pa.array(list(forms))), -1).to_numpy(zero_copy_only=False)
    return code.astype(np.int64), pc.not_equal(form, base).to_numpy(zero_copy_only=False)


def first_flags(cidx: np.ndarray, avail: np.ndarray, lookback_ns: int) -> np.ndarray:
    """Per row (any order): True when no row of the same CIK has avail in [avail - lookback_ns, avail)."""
    n = len(cidx)
    o = np.lexsort((avail, cidx))
    c, a = cidx[o], avail[o]
    new = np.ones(n, dtype=bool)
    new[1:] = (c[1:] != c[:-1]) | (a[1:] != a[:-1])
    start = np.maximum.accumulate(np.where(new, np.arange(n), 0)) if n else np.zeros(0, dtype=np.int64)
    prev = np.maximum(start - 1, 0)   # the latest row of a strictly earlier available_at (when of the same CIK)
    recent = (start > 0) & (c[prev] == c) & (a[prev] >= a - lookback_ns) if n else np.zeros(0, dtype=bool)
    out = np.empty(n, dtype=bool)
    out[o] = ~recent
    return out


def nt_notices(h, directory: Path, manifest: dict, ciks: np.ndarray, cal, end_ns: int, budget) -> tuple:
    """The flagging windows of the first NT 10-K / NT 10-Q notices (NT_RULE), their source and checks."""
    st: dict = {"rows_total": 0, "rows_not_nt_form": 0, "rows_nt_amendment": 0, "rows_sealed": 0,
                "rows_after_role": 0, "rows_unlinked_cik": 0, "rows_used": 0}
    parts: dict = {k: [] for k in ("c", "acc", "a", "kind")}
    src: dict = {}
    for batch in stage_batches(h, directory, manifest, "events.parquet", NT_COLS, budget, src):
        tb = pa.Table.from_batches([batch])
        st["rows_total"] += tb.num_rows
        kind, suffix = form_codes(h, tb, NT_ANY_FORMS)
        nt = np.flatnonzero(kind >= 0)
        st["rows_not_nt_form"] += tb.num_rows - len(nt)
        if not len(nt):
            continue
        tb, kind, suffix = tb.take(pa.array(nt)), kind[nt], suffix[nt]
        amend = pc.fill_null(h.column_of(tb, "is_amendment"), True).to_numpy(zero_copy_only=False) | suffix
        avail = h.as_instants_ns(h.column_of(tb, "available_at"), "events.parquet available_at")
        pos, linked = cik_positions(ciks, h.as_ids(h.column_of(tb, "cik"), "events.parquet cik"))
        sealed = avail >= h.SEAL_NS
        late = ~sealed & (avail >= end_ns)
        st["rows_nt_amendment"] += int(np.count_nonzero(amend))
        st["rows_sealed"] += int(np.count_nonzero(~amend & sealed))
        st["rows_after_role"] += int(np.count_nonzero(~amend & late))
        st["rows_unlinked_cik"] += int(np.count_nonzero(~amend & ~sealed & ~late & ~linked))
        keep = np.flatnonzero(~amend & ~sealed & ~late & linked)
        st["rows_used"] += len(keep)
        if not len(keep):
            continue
        parts["c"].append(pos[keep])
        parts["a"].append(avail[keep])
        parts["acc"].append(accession_keys(h.column_of(tb.take(pa.array(keep)), "accession")))
        parts["kind"].append(kind[keep])
        budget.check("v9-nt-batch")
    cat = lambda xs: np.concatenate(xs).astype(np.int64) if xs else np.empty(0, dtype=np.int64)
    c, acc, a, kind = cat(parts["c"]), cat(parts["acc"]), cat(parts["a"]), cat(parts["kind"])
    # one row per (cik, accession): a co-filed row of one accession shares the filing's clock (the later if not)
    o = np.lexsort((acc, c))
    c, acc, a, kind = c[o], acc[o], a[o], kind[o]
    new = np.ones(len(c), dtype=bool)
    new[1:] = (c[1:] != c[:-1]) | (acc[1:] != acc[:-1])
    first_row = np.flatnonzero(new)
    if len(first_row):
        st["accessions_with_clock_disagreement"] = int(np.count_nonzero(
            np.minimum.reduceat(a, first_row) != np.maximum.reduceat(a, first_row)))
        a = np.maximum.reduceat(a, first_row)
        kind = np.minimum.reduceat(kind, first_row)
    else:
        st["accessions_with_clock_disagreement"] = 0
    c = c[first_row]
    first = first_flags(c, a, NT_FIRST_DAYS * h.DAY_NS)
    flag = first & (kind < len(NT_FORMS))           # a first NT 10-K or NT 10-Q
    st["notices_used"] = {f: int(np.count_nonzero(kind == i)) for i, f in enumerate(NT_ANY_FORMS)}
    st["notices_first"] = {f: int(np.count_nonzero(first & (kind == i))) for i, f in enumerate(NT_ANY_FORMS)}
    usable = cal.usable_from(a[flag])
    window = Windowed(usable, usable + NT_WINDOW, c[flag], np.ones(len(usable), dtype=np.int64), len(ciks), np.int64)
    return window, src, st


def periodic_filings(h, directory: Path, manifest: dict, ciks: np.ndarray, cal, end_ns: int, budget) -> tuple:
    """The domestic periodic filings (NT_PRESENT) in clock order: a Latest pointer and each row's available_at."""
    st: dict = {"rows_total": 0, "rows_periodic_amendment": 0, "rows_sealed": 0, "rows_after_role": 0,
                "rows_unlinked_cik": 0}
    parts: dict = {"c": [], "a": []}
    src: dict = {}
    for batch in stage_batches(h, directory, manifest, "filings.parquet", FILING_COLS, budget, src):
        tb = pa.Table.from_batches([batch])
        st["rows_total"] += tb.num_rows
        kind, suffix = form_codes(h, tb, PERIODIC_FORMS)
        rows = np.flatnonzero(kind >= 0)
        if not len(rows):
            continue
        tb, suffix = tb.take(pa.array(rows)), suffix[rows]
        amend = pc.fill_null(h.column_of(tb, "is_amendment"), True).to_numpy(zero_copy_only=False) | suffix
        avail = h.as_instants_ns(h.column_of(tb, "available_at"), "filings.parquet available_at")
        pos, linked = cik_positions(ciks, h.as_ids(h.column_of(tb, "cik"), "filings.parquet cik"))
        sealed = avail >= h.SEAL_NS
        late = ~sealed & (avail >= end_ns)
        st["rows_periodic_amendment"] += int(np.count_nonzero(amend))
        st["rows_sealed"] += int(np.count_nonzero(~amend & sealed))
        st["rows_after_role"] += int(np.count_nonzero(~amend & late))
        st["rows_unlinked_cik"] += int(np.count_nonzero(~amend & ~sealed & ~late & ~linked))
        keep = np.flatnonzero(~amend & ~sealed & ~late & linked)
        parts["c"].append(pos[keep])
        parts["a"].append(avail[keep])
        budget.check("v9-filings-batch")
    cat = lambda xs: np.concatenate(xs).astype(np.int64) if xs else np.empty(0, dtype=np.int64)
    c, a = cat(parts["c"]), cat(parts["a"])
    o = np.argsort(a, kind="stable")
    c, a = c[o], a[o]
    st["rows_used"] = int(len(c))
    return {"latest": Latest(cal.usable_from(a), c, len(ciks)), "avail": a}, src, st


def nt_rows(h, role, output: Path, budget, options: dict) -> tuple:
    """``nt_first_126`` (its field definition): ({name: (writer, sources, extras)}, source checks)."""
    nd, n = role.n_dates, role.n
    budget.admit(nd * n * 5 + (128 << 20), "v9-nt-link-matrix")
    links, bridge_src, bridge_st = h.load_bridge(Path(options["sec_identity_bridge"]),
                                                 options["sec_identity_bridge_sha256"], role, budget)
    marks = role.days * h.DAY_NS + h.MARK_NS
    ciks, link, primary, link_st = h.resolve_links(links, role, marks, budget)
    del links
    cal = Calendar(role, h.DAY_NS, h.MARK_NS)
    end_ns = int(marks[-1])   # a row at or after the last role mark is never usable in the role
    directory = Path(options["sec_stages"]) / STAGES[NT_STAGE][0]
    manifest, man_src = h.pinned_manifest(directory, options["sec_filings_sha256"], NT_STAGE.replace("_", "-"),
                                          STAGES[NT_STAGE][1])
    window, nt_src, nt_st = nt_notices(h, directory, manifest, ciks, cal, end_ns, budget)
    periodic, pr_src, pr_st = periodic_filings(h, directory, manifest, ciks, cal, end_ns, budget)
    budget.report("v9-nt-loaded", notices=sum(nt_st["notices_used"].values()), periodic=pr_st["rows_used"])
    reasons = {"not_primary_link": 0, "absent_or_stale": 0, "out_of_rule": 0}
    per_year: dict = {}
    flagged = 0
    w = h.FieldWriter(output, "nt_first_126", role)
    try:
        for t in range(nd):
            u = t + cal.prefix
            lk = link[t]
            safe = np.maximum(lk, 0)
            pline = (lk >= 0) & primary[t]
            k = np.where(pline, periodic["latest"].at(u)[safe] if len(ciks) else -1, -1)
            present = np.zeros(n, dtype=bool)
            if len(periodic["avail"]):
                cutoff = int(cal.marks[u - 1]) - NT_PRESENCE_DAYS * h.DAY_NS
                present = (k >= 0) & (periodic["avail"][np.maximum(k, 0)] >= cutoff)
            row = np.where(present, (window.at(u)[safe] > 0).astype(np.float64), np.nan)
            w.write(row)
            member = role.member[t] != 0
            reasons["not_primary_link"] += int(np.count_nonzero(member & ~pline))
            reasons["absent_or_stale"] += int(np.count_nonzero(member & pline & ~present))
            y = per_year.setdefault(str(int(role.years[t])), [0, 0])
            y[0] += int(np.count_nonzero(member & pline))
            y[1] += int(np.count_nonzero(member & present))
            flagged += int(np.count_nonzero(member & (row == 1.0)))
            if t % 256 == 0:
                budget.check("v9-nt-write")
    except BaseException:
        w.f.close()   # refused: the partial file stays unpublished (no manifest), handle released
        raise
    w.close()
    extras = {"lag_sessions": NT_LAG_SESSIONS, "lag": NT_LAG, "min_history": NT_MIN_HISTORY, "nan_rule": NT_PRESENT,
              "stage_manifests": {NT_STAGE: {"path": man_src["path"], "sha256": man_src["sha256"]}},
              "identity_bridge_manifest_sha256": bridge_src[0]["sha256"],
              "nan_reasons_member_cells": reasons, "flagged_member_cells": flagged,
              "coverage_linked_primary": {yr: {"member_primary_cells": m, "finite_member_cells": f,
                                               "finite_frac": round(f / m, 6) if m else None}
                                          for yr, (m, f) in sorted(per_year.items())}}
    sources = list(bridge_src) + [man_src, nt_src, pr_src]
    checks = {"clock": SEC_CLOCK, "calendar": cal.stats, "identity_bridge": {**bridge_st, **link_st},
              "events": nt_st, "filings": pr_st}
    return {"nt_first_126": (w, sources, extras)}, checks


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "V9FieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_draft.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_v9: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return V9FieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of a field: its stage manifest and SEC bridge; and the code its producer imports
    (imported_code)."""
    lower = lambda k: str(options.get(k) or "").lower()
    pins = {NT_STAGE: lower(f"{NT_STAGE}_sha256"), "sec_identity_bridge": lower("sec_identity_bridge_sha256")}
    return {**pins, "imported_code": imported_code(producer_group(name))}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    stages = entry.get("stage_manifests") if isinstance(entry.get("stage_manifests"), dict) else {}
    pins = {NT_STAGE: (stages.get(NT_STAGE) or {}).get("sha256"),
            "sec_identity_bridge": entry.get("identity_bridge_manifest_sha256")}
    return {**pins, "imported_code": entry.get("imported_code")}


class V9FieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No new options: nt_first_126 reads the SEC module's --sec-stages, --sec-identity-bridge(-sha256) and
        --sec-filings-sha256 (OPTIONS)."""

    @staticmethod
    def check(selected, options: dict):
        """Before any output: each requested field's inputs."""
        need = []
        if "nt_first_126" in selected:
            need += ["sec_stages", "sec_identity_bridge", "sec_identity_bridge_sha256", "sec_filings_sha256"]
        missing = ["--" + k.replace("_", "-") for k in need if options.get(k) in (None, "")]
        if missing:
            names = [x for x in FIELDS if x in selected]
            raise ValueError(f"--fields: {', '.join(names)} need {', '.join(missing)} (run() callers pass them in "
                             "module_options)")

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        results, checks = {}, {}
        if "nt_first_126" in names:
            got, checks["nt_first_126"] = nt_rows(h, role, output, budget, options)
            results.update(got)
        source_checks[GROUP] = checks
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, 0)),
                               "min_history": spec["min_history"], **extra,
                               "imported_code": imported_code(spec["group"])}
            outcome[x] = (w, sources, w.coverage())
        budget.report("v9-complete", fields=len(names))
