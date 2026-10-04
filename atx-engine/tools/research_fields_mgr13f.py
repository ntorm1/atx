"""13F manager-horizon research field (platform v8 lane YDATA, draft): short-term institutional trading.

XDATA ranked family 7 ("13F manager network") in the one form whose canonical definition needs nothing but 13F holdings:
Yan and Zhang (2009, RFS 22(2), "Institutional investors and equity returns: are short-term institutions better
informed?"). Institutions are split by their portfolio churn, computed from their own 13F holdings as in Gaspar, Massa
and Matos (2005, JFE); short-term institutions are the top tercile of the churn averaged over the previous four quarters;
their trading (the quarterly change of the fraction of shares outstanding they hold) forecasts returns with a positive
sign and does not reverse. No manager type (filer_type, not built) and no fund data (N-PORT, not built) is needed.

Field ``stio_chg_q`` at anchor quarter P (the latest visible quarter, ``F13_CLOCK``):
* positions of quarter q: the screened mapped 13F rows (research_fields_holdings ``F13_ROWS_RULE``: effective filings by
  the deadline, SH, no put/call, shares > 0, finite value >= 0, value unit factor, CUSIP -> security_id by the PIT map
  of q, the [1/10, 10] implied-price screen) summed per (filer CIK, security): shares N, value V; each security's price
  p = the median implied price value / shares of its kept rows; a filer's total = the sum of its kept values;
* churn CR_k(q) of filer k holding in q-1 and q (``CHURN_RULE``): buys and sells valued at q's price, the smaller of
  the two divided by the mean of the filer's totals of q-1 and q (Gaspar-Massa-Matos; the minimum, Yan-Zhang); shares
  of q-1 restated in q's share basis where the continuing holders show a basis change (a split: ``BASIS_RULE``);
* short-term set ST(q): filers with CR in each of q-3..q, ranked by their mean (average ties), rank > 2m/3 of m;
* STIO(q, S) = the shares held by the filers of S / ``shares_out`` at the last role session on or before the quarter end
  (q's own share basis, as ``inst_own_share``); 0 for a security with a 13F row in q but no S holder; NaN without a row,
  without a finite positive shares_out, or above 2 (declared plausibility);
* value = STIO(P, ST(P-1)) - STIO(P-1, ST(P-1)): the trading over P of the institutions classified short-term as of
  P-1 (the classification is known before the trading it scores; the difference is share-basis invariant).

Clock: research_fields_holdings ``F13_CLOCK`` (a quarter visible at V(P), its last deadline filing + 46 h; row t reads
the latest quarter with V(P) < 22:00 UTC of session t-1, at most 150 days old, in which the security has a row). The
value of P reads quarters P-5..P only, every one visible no later than P.

Seal (reader side, research_window): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``;
holdings parts of filing quarters that begin on or after the seal are never opened, and a quarter visible on or after
the seal is never used (research_fields_holdings ``source_part_is_sealed`` and the ``SEAL_NS`` rule).

Draft and off by default: ``prepare_research_fields_ydata.py`` registers it. Options (its own; the holdings module's
``--thirteenf`` is consumed by that module's wrapper): ``--mgr13f-stage`` (the atx-db alpha-panel ``thirteenf/`` stage
directory) and ``--mgr13f-stage-sha256`` (its manifest pin). The field requires ``shares_out`` in the same run.
``--reuse`` (v8 C-3): the stage pin, the seal and ``imported_code`` (the AST closure of the names imported from
research_fields_holdings.py, which no producer fingerprint follows) are the input pins.
"""
from __future__ import annotations

from pathlib import Path
import hashlib
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import code_fingerprint  # same directory: the AST closure fingerprints --reuse keys on
import research_fields_holdings as hold  # same directory; it does not import this module
import research_window as rw
from research_fields_sec import _Host

GROUP = "mgr13f"
OPTIONS = ("mgr13f_stage", "mgr13f_stage_sha256")
HOST_HANDLES = ("h",)
BUILDER = "prepare_research_fields.py"
BUILDER_ORCHESTRATION = frozenset({"run", "main"})
STAGE_KEY = "thirteenf"
CHURN_QUARTERS = 4          # Yan-Zhang: churn averaged over the previous four quarters
ST_FRACTION = 2.0 / 3.0     # short-term = the top tercile of the average churn
BASIS_SHARE_TOL = 1.2       # a continuing holders' median share ratio outside [1/1.2, 1.2] (a 6:5 split or more) ...
BASIS_VALUE_TOL = 1.5       # ... whose price ratio offsets it within [1/1.5, 1.5] (value moved < 50%): a basis change
BASIS_MIN_HOLDERS = 5       # at least five continuing holders before a basis change is called
HISTORY_QUARTERS = CHURN_QUARTERS + 1   # quarters before an anchor quarter its value reads (P-5..P-1)

# The names the producers call from research_fields_holdings.py: their closure is an input pin (review B-1).
HOLD_IMPORTS = ("Stage", "effective_filings", "group_median", "needed_quarters", "prev_quarter_end",
                "source_part_is_sealed", "anchor_quarters", "prev_marks", "day_of", "date_of", "_col", "_days", "_ids", "_f64",
                "_text", "_instants", "_release", "F13_FORMS", "F13_PRICE_OUTLIER", "F13_IO_MAX", "NEVER",
                "BEFORE_ALL", "SEAL_NS")

CHURN_RULE = (
    "gmm-yz-min-churn-v1: for filer k with positions in q-1 and q, over every security i it holds in either: "
    "d = N_q - a_i N_(q-1) (0 for an absent side), price x = p_q(i), else p_(q-1)(i) / a_i, else 0; buys = sum of "
    "max(d, 0) x, sells = sum of max(-d, 0) x; CR = min(buys, sells) / ((total_(q-1) + total_q) / 2), NaN when that "
    "mean is not positive")
BASIS_RULE = (
    "13f-basis-change-v1: per security, s = the median of N_q / N_(q-1) over the filers holding it in both quarters; "
    f"a = s when at least {BASIS_MIN_HOLDERS} such filers, s outside [1/{BASIS_SHARE_TOL}, {BASIS_SHARE_TOL}] and "
    f"s x p_q / p_(q-1) inside [1/{BASIS_VALUE_TOL}, {BASIS_VALUE_TOL}] (shares and price moved inversely: a split "
    "or a stock distribution), else a = 1")
ST_RULE = (
    f"yz-short-term-tercile-v1: m = the filers with a finite CR in each of q-{CHURN_QUARTERS - 1}..q (consecutive "
    "calendar quarters); their mean CR ranked ascending with average ties (1..m); ST(q) = the filers whose rank "
    f"exceeds {ST_FRACTION:.6f} m; undefined when m < 3")
STIO_RULE = (
    "yz-stio-chg-q-v1: STIO(q, S) = sum over filers in S of the security's position shares in q / shares_out at the "
    "last role session on or before the quarter end q; 0 when the security has positions in q but none by S; NaN "
    "without positions in q, without a finite positive shares_out, or above 2; value at anchor P = STIO(P, ST(P-1)) - "
    "STIO(P-1, ST(P-1)), NaN when P-1 is not the previous calendar quarter or ST(P-1) is undefined")

FIELDS: dict = {
    "stio_chg_q": {
        "group": "y_stio", "point_in_time": True, "lagged": False, "requires": ["shares_out"],
        "units": "change of the fraction of shares outstanding held by short-term 13F institutions (decimal, signed)",
        "clock": hold.F13_CLOCK, "staleness": hold.F13_STALENESS,
        "definition": "Yan-Zhang (2009) short-term institutional trading. " + " ".join(
            (hold.F13_ROWS_RULE + ".", CHURN_RULE + ".", BASIS_RULE + ".", ST_RULE + ".", STIO_RULE + ".")),
        "source_columns": hold.F13_COLUMNS[:8] + hold.F13_HOLD_COLUMNS + ["shares_out"],
        "caveats": hold.F13_CAVEATS + [
            "churn is computed on the mapped (security_id) part of each filer's 13F book; unmapped rows (about 1% of "
            "13F value) and options are not in it",
            "a share-basis change is detected from the holders, not from a corporate-action ledger (BASIS_RULE)",
            "shares_out is the house A8 90-day lagged vendor count (one line, not the issuer total)"],
        "formula_id": "yz-stio-chg-q-v1",
        "min_history": f"{HISTORY_QUARTERS} filing quarters before the anchor quarter (the stage holds 2013q2 on)",
        "needs": "thirteenf_stage",
    }
}

PRODUCERS = {"y_stio": ("open_stage", "stio_rows")}


# ---------------------------------------------------------------------------------------------------------------
# Imported code: the --reuse input pin of what the producers import
# ---------------------------------------------------------------------------------------------------------------

def imported_code(group: str, source: bytes | None = None, builder: bytes | None = None) -> dict:
    """{"module", "names", "sha256"}: SHA-256 (code_fingerprint) of the AST closure of ``HOLD_IMPORTS`` in
    research_fields_holdings.py, plus the closure of every builder definition they read through ``ns``."""
    if group not in PRODUCERS:
        raise ValueError(f"research_fields_mgr13f: unknown producer group {group!r}")
    path = Path(hold.__file__)
    src = path.read_bytes() if source is None else source
    host = (Path(__file__).resolve().parent / BUILDER).read_bytes() if builder is None else builder
    fp = code_fingerprint.fingerprints(src.replace(b"\r\n", b"\n"), {group: HOLD_IMPORTS},
                                       host=code_fingerprint.Host(host.replace(b"\r\n", b"\n"), hold.HOST_HANDLES,
                                                                  BUILDER_ORCHESTRATION))[group]
    if fp is None:
        raise ValueError(f"research_fields_mgr13f: {path.name} lacks one of {HOLD_IMPORTS}")
    return {"module": path.name, "names": list(HOLD_IMPORTS), "sha256": fp}


# ---------------------------------------------------------------------------------------------------------------
# Pure helpers (unit-tested directly)
# ---------------------------------------------------------------------------------------------------------------

def quarter_positions(filer, sid, shares, value, outlier=hold.F13_PRICE_OUTLIER) -> dict:
    """One quarter's screened mapped positions: unique (filer, security) pairs (sorted by filer, then security) with
    summed shares and values, each security's median implied price, each filer's total value."""
    filer, sid = np.asarray(filer, dtype=np.int64), np.asarray(sid, dtype=np.int64)
    shares, value = np.asarray(shares, dtype=np.float64), np.asarray(value, dtype=np.float64)
    mapped = sid >= 0
    filer, sid, shares, value = filer[mapped], sid[mapped], shares[mapped], value[mapped]
    priced = value > 0
    price = np.where(priced, value / np.where(priced, shares, 1.0), np.nan)
    us, med = hold.group_median(sid[priced], price[priced])
    ref = np.full(len(sid), np.nan)
    ref[priced] = med[np.searchsorted(us, sid[priced])]
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio = price / ref
    out = priced & ((ratio > outlier) | (ratio < 1.0 / outlier))
    keep = ~out
    filer, sid, shares, value, price, priced = (x[keep] for x in (filer, sid, shares, value, price, priced))
    filers = np.unique(filer)
    sids = np.unique(sid)
    ns = max(len(sids), 1)
    key = np.searchsorted(filers, filer) * ns + np.searchsorted(sids, sid)
    pairs, inv = np.unique(key, return_inverse=True)
    pf = pairs // ns
    ps_, pp = hold.group_median(sid[priced], price[priced])
    return {"filers": filers, "filer_value": np.bincount(pf, weights=np.bincount(inv, weights=value,
                                                                                 minlength=len(pairs)),
                                                         minlength=len(filers)),
            "pair_filer": filers[pf], "pair_sid": sids[pairs % ns],
            "pair_shares": np.bincount(inv, weights=shares, minlength=len(pairs)),
            "price_sids": ps_, "price": pp, "rows_mapped": int(np.count_nonzero(mapped)),
            "rows_price_outlier": int(np.count_nonzero(out))}


def price_on(pos: dict, sids: np.ndarray) -> np.ndarray:
    """The quarter's price of each security in ``sids`` (NaN when it has none)."""
    out = np.full(len(sids), np.nan)
    if not len(pos["price_sids"]):
        return out
    at = np.minimum(np.searchsorted(pos["price_sids"], sids), len(pos["price_sids"]) - 1)
    hit = pos["price_sids"][at] == sids
    out[hit] = pos["price"][at[hit]]
    return out


def churn(prev: dict, cur: dict) -> tuple:
    """(filers holding in both quarters, their CR (``CHURN_RULE``), stats) for consecutive quarters prev -> cur."""
    F = np.intersect1d(prev["filers"], cur["filers"])
    st = {"filers_both": int(len(F)), "basis_changes": 0}
    if not len(F):
        return F, np.zeros(0), st
    mp, mc = np.isin(prev["pair_filer"], F), np.isin(cur["pair_filer"], F)
    U = np.union1d(prev["pair_sid"][mp], cur["pair_sid"][mc])
    nu = len(U)
    kp = np.searchsorted(F, prev["pair_filer"][mp]) * nu + np.searchsorted(U, prev["pair_sid"][mp])
    kc = np.searchsorted(F, cur["pair_filer"][mc]) * nu + np.searchsorted(U, cur["pair_sid"][mc])
    Np, Nc = prev["pair_shares"][mp], cur["pair_shares"][mc]
    common, ip, ic = np.intersect1d(kp, kc, assume_unique=True, return_indices=True)
    uc = common % nu
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio = Nc[ic] / Np[ip]
    s = np.ones(nu)
    um, med = hold.group_median(uc, ratio)
    s[um] = med
    cnt = np.bincount(uc, minlength=nu)
    Pp, Pc = price_on(prev, U), price_on(cur, U)
    with np.errstate(invalid="ignore", divide="ignore"):
        moved = np.abs(np.log(s)) > np.log(BASIS_SHARE_TOL)
        offset = np.abs(np.log(s * Pc / Pp)) <= np.log(BASIS_VALUE_TOL)
    basis = (cnt >= BASIS_MIN_HOLDERS) & moved & offset
    a = np.where(basis, s, 1.0)
    st["basis_changes"] = int(np.count_nonzero(basis))
    allk = np.union1d(kp, kc)
    n_prev, n_cur = np.zeros(len(allk)), np.zeros(len(allk))
    n_prev[np.searchsorted(allk, kp)] = Np
    n_cur[np.searchsorted(allk, kc)] = Nc
    u, f = allk % nu, allk // nu
    d = n_cur - a[u] * n_prev
    x = np.where(np.isfinite(Pc[u]), Pc[u], Pp[u] / a[u])
    x = np.where(np.isfinite(x), x, 0.0)
    buy = np.bincount(f, weights=np.maximum(d, 0.0) * x, minlength=len(F))
    sell = np.bincount(f, weights=np.maximum(-d, 0.0) * x, minlength=len(F))
    vp = prev["filer_value"][np.searchsorted(prev["filers"], F)]
    vc = cur["filer_value"][np.searchsorted(cur["filers"], F)]
    denom = (vp + vc) / 2.0
    with np.errstate(invalid="ignore", divide="ignore"):
        cr = np.where(denom > 0, np.minimum(buy, sell) / denom, np.nan)
    return F, cr, st


def average_ranks(x: np.ndarray) -> np.ndarray:
    """Ascending ranks 1..m with ties sharing the mean of their ranks."""
    o = np.argsort(x, kind="stable")
    _, start, counts = np.unique(x[o], return_index=True, return_counts=True)
    ranks = np.empty(len(x))
    ranks[o] = np.repeat(start + (counts + 1) / 2.0, counts)
    return ranks


def short_term_set(history: list):
    """ST(q) (``ST_RULE``) from the (filers, CR) of quarters q-3..q (oldest first); None when undefined."""
    if len(history) != CHURN_QUARTERS or any(h is None for h in history):
        return None
    F = history[0][0]
    for filers, _ in history[1:]:
        F = np.intersect1d(F, filers)
    total = np.zeros(len(F))
    ok = np.ones(len(F), dtype=bool)
    for filers, cr in history:
        v = cr[np.searchsorted(filers, F)] if len(F) else np.zeros(0)
        ok &= np.isfinite(v)
        total += np.where(np.isfinite(v), v, 0.0)
    F, mean = F[ok], total[ok] / CHURN_QUARTERS
    m = len(F)
    if m < 3:
        return None
    return F[average_ranks(mean) > ST_FRACTION * m]


def stio(pos: dict, members: np.ndarray, so_row: np.ndarray, role) -> tuple:
    """(STIO of every role line for the set ``members`` (``STIO_RULE``), cells above the plausibility bound)."""
    n = role.n
    level = np.full(n, np.nan)
    held = np.unique(pos["pair_sid"])
    p0, o0 = role.columns_of(held)
    level[p0[o0]] = 0.0
    sel = np.isin(pos["pair_filer"], members)
    us, inv = np.unique(pos["pair_sid"][sel], return_inverse=True)
    tot = np.bincount(inv, weights=pos["pair_shares"][sel], minlength=len(us))
    p1, o1 = role.columns_of(us)
    level[p1[o1]] = tot[o1]
    ok = np.isfinite(so_row) & (so_row > 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        v = np.where(ok, level / np.where(ok, so_row, 1.0), np.nan)
    bad = np.isfinite(v) & (v > hold.F13_IO_MAX)
    return np.where(bad, np.nan, v), int(np.count_nonzero(bad))


# ---------------------------------------------------------------------------------------------------------------
# Inputs and the producer
# ---------------------------------------------------------------------------------------------------------------

def open_stage(options: dict):
    """The pinned thirteenf stage (manifest SHA-256, schema, clock and staleness rules; research_fields_holdings)."""
    return hold.Stage(STAGE_KEY, Path(options["mgr13f_stage"]), options["mgr13f_stage_sha256"])


def shares_out_matrix(output: Path, role):
    """This run's shares_out.f64 (written by the builder before the modules compute, or copied by --reuse), mapped
    read-only, and its pin."""
    path = Path(output) / "shares_out.f64"
    size = path.stat().st_size
    if size != role.n_dates * role.n * 8:
        raise ValueError("stio_chg_q: this run's shares_out.f64 has the wrong size")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    so = np.memmap(path, dtype="<f8", mode="r", shape=(role.n_dates, role.n))
    return so, {"path": str(path.resolve()), "bytes": size, "sha256": digest}


def quarter_rows(stage, mine, acc_all, filer_all, src_all, uf_acc, uf_val, keys, ksid, st, budget) -> dict:
    """The screened holdings rows (``F13_ROWS_RULE`` before the price screen) of the effective filings ``mine`` of one
    quarter: filer, security (-1 unmapped), shares, value."""
    order = np.argsort(acc_all[mine].astype(str))
    acc_q = pa.array(acc_all[mine][order].tolist(), pa.string())
    filer_q = filer_all[mine][order]
    ufp = np.ones(len(mine))
    hi = pc.fill_null(pc.index_in(acc_q, value_set=uf_acc), -1).to_numpy(zero_copy_only=False)
    ufp[hi >= 0] = uf_val[hi[hi >= 0]]
    ufp = np.where(np.isfinite(ufp) & (ufp > 0), ufp, 1.0)
    rows = {"filer": [], "sid": [], "shares": [], "value": []}
    for part in sorted(set(src_all[mine].tolist())):
        if hold.source_part_is_sealed(part):  # filed on or after the seal: never opened
            st["holdings_parts_not_read_sealed"] = st.get("holdings_parts_not_read_sealed", 0) + 1
            continue
        path, ident = stage.verified_path(f"parts/source={part}/holdings.parquet")
        pf = pq.ParquetFile(path)
        for batch in pf.iter_batches(batch_size=65_536, columns=["accession", "cusip", "shares", "sshprnamt_type",
                                                                 "put_call", "value_usd"], use_threads=False):
            ai = pc.fill_null(pc.index_in(batch.column("accession"), value_set=acc_q), -1).to_numpy(zero_copy_only=False)
            ok = ai >= 0
            ok &= pc.fill_null(pc.equal(batch.column("sshprnamt_type"), "SH"), False).to_numpy(zero_copy_only=False)
            pcall = batch.column("put_call")
            ok &= pc.fill_null(pc.or_kleene(pc.is_null(pcall), pc.equal(pcall, "")), True).to_numpy(zero_copy_only=False)
            sh, val = hold._f64(batch.column("shares")), hold._f64(batch.column("value_usd"))
            ok &= np.isfinite(sh) & (sh > 0) & np.isfinite(val) & (val >= 0)
            if not ok.any():
                continue
            k = np.flatnonzero(ok)
            ci = pc.fill_null(pc.index_in(batch.column("cusip").take(pa.array(k)), value_set=keys), -1).to_numpy(
                zero_copy_only=False)
            rows["filer"].append(filer_q[ai[k]])
            rows["sid"].append(np.where(ci >= 0, ksid[np.maximum(ci, 0)], -1))
            rows["shares"].append(sh[k])
            rows["value"].append(val[k] * ufp[ai[k]])
        pf.close()
        stage.unchanged(path, ident)
        del pf
        hold._release()
        budget.check("mgr13f-holdings")
    return {k: np.concatenate(v) if v else np.zeros(0) for k, v in rows.items()}


def stio_rows(h, stage, role, output: Path, budget, so) -> tuple:
    """``stio_chg_q`` (its field definition): ({name: (writer, extras)}, source checks)."""
    n = role.n
    days = role.days.astype(np.int64)
    fresh = hold.needed_quarters(hold.date_of(int(days[0])), hold.date_of(int(days[-1])))
    early = [fresh[0]]
    for _ in range(HISTORY_QUARTERS - 1):
        early.insert(0, hold.prev_quarter_end(early[0]))
    quarters = early + fresh[1:]
    qdays = np.array([hold.day_of(q) for q in quarters], dtype=np.int64)
    nq = len(quarters)
    budget.admit(nq * n * 9 + (384 << 20), "mgr13f-quarters")
    f = stage.table("filings.parquet", ["accession", "filer_cik", "period_q", "filing_date", "submission_type",
                                        "amendment_type", "available_at", "deadline", "source_period"])
    f = f.filter(pa.array(np.isin(hold._days(hold._col(f, "period_q")), qdays)))
    period, fdate = hold._days(hold._col(f, "period_q")), hold._days(hold._col(f, "filing_date"))
    deadline, avail = hold._days(hold._col(f, "deadline")), hold._instants(hold._col(f, "available_at"))
    subtype, amend = hold._text(hold._col(f, "submission_type")), hold._text(hold._col(f, "amendment_type"))
    in_q = fdate <= deadline
    qidx = np.searchsorted(qdays, period)
    has_filing = np.zeros(nq, dtype=bool)
    has_filing[qidx[in_q]] = True
    vq = np.full(nq, hold.BEFORE_ALL, dtype=np.int64)
    np.maximum.at(vq, qidx[in_q], avail[in_q])      # V(P): every filing of P by its deadline, notices included
    vis = np.where(has_filing, vq, hold.NEVER)
    sealed = vis >= hold.SEAL_NS
    vis[sealed] = hold.NEVER
    sel = in_q & np.isin(subtype, hold.F13_FORMS)
    filer_all = hold._ids(pc.cast(pc.fill_null(hold._col(f, "filer_cik"), "-1"), pa.int64()))
    acc_all, src_all = hold._text(hold._col(f, "accession")), hold._text(hold._col(f, "source_period"))
    idx = np.flatnonzero(sel)
    eidx = idx[hold.effective_filings(period[idx], filer_all[idx], fdate[idx], acc_all[idx], subtype[idx], amend[idx])]
    st = {"quarters": [q.isoformat() for q in quarters], "effective_filings": int(len(eidx)),
          "filings_after_deadline_ignored": int(np.count_nonzero(~in_q)),
          "quarters_sealed": int(np.count_nonzero(sealed & has_filing)),
          "quarter_visible_at": {quarters[i].isoformat(): (None if vis[i] == hold.NEVER else
                                                           str(np.datetime64(int(vis[i]), "ns")) + "Z") for i in range(nq)},
          "churn_rule": CHURN_RULE, "basis_rule": BASIS_RULE, "short_term_rule": ST_RULE}
    del f
    checks = stage.table("filing_checks.parquet", ["accession", "unit_factor"])
    uf_acc, uf_val = hold._col(checks, "accession"), hold._f64(hold._col(checks, "unit_factor"))
    del checks
    cmap = stage.table("cusip_map_pit.parquet", ["period_q", "cusip", "security_id"])
    cm_q, cm_cusip, cm_sid = hold._days(hold._col(cmap, "period_q")), hold._col(cmap, "cusip"), \
        hold._ids(hold._col(cmap, "security_id"))
    del cmap
    budget.check("mgr13f-inputs")
    eq = np.searchsorted(qdays, period[eidx])
    dstio = np.full((nq, n), np.nan)
    has = np.zeros((nq, n), dtype=bool)
    per_q, implausible = {}, 0
    prev = prev_st = None
    history: list = []
    for q in range(nq):
        consecutive = q > 0 and quarters[q - 1] == hold.prev_quarter_end(quarters[q])
        cur = None
        if has_filing[q] and not sealed[q]:
            cm = cm_q == qdays[q]
            raw = quarter_rows(stage, eidx[eq == q], acc_all, filer_all, src_all, uf_acc, uf_val,
                               cm_cusip.filter(pa.array(cm)), cm_sid[cm], st, budget)
            cur = quarter_positions(raw["filer"], raw["sid"], raw["shares"], raw["value"])
            del raw
        rec = {}
        cr = None
        if cur is not None and prev is not None and consecutive:
            F, c, cst = churn(prev, cur)
            cr = (F, c)
            rec.update(cst, filers_with_churn=int(np.count_nonzero(np.isfinite(c))))
        history = (history + [cr])[-CHURN_QUARTERS:] if consecutive else [cr]
        st_set = short_term_set(history)
        if cur is not None:
            p0, o0 = role.columns_of(np.unique(cur["pair_sid"]))
            has[q, p0[o0]] = True
            rec.update(filers=int(len(cur["filers"])), pairs=int(len(cur["pair_sid"])),
                       rows_mapped=cur["rows_mapped"], rows_price_outlier=cur["rows_price_outlier"])
            if prev is not None and consecutive and prev_st is not None:
                t1 = int(np.searchsorted(days, qdays[q], side="right")) - 1
                t0 = int(np.searchsorted(days, qdays[q - 1], side="right")) - 1
                if t0 >= 0 and t1 >= 0:
                    v1, b1 = stio(cur, prev_st, np.asarray(so[t1]), role)
                    v0, b0 = stio(prev, prev_st, np.asarray(so[t0]), role)
                    dstio[q] = v1 - v0
                    implausible += b0 + b1
        rec["short_term_filers"] = None if st_set is None else int(len(st_set))
        per_q[quarters[q].isoformat()] = rec
        prev, prev_st = cur, st_set
        budget.report("mgr13f-quarter", quarter=quarters[q].isoformat())
    del prev
    hold._release()
    has &= ~(vis[:, None] == hold.NEVER)
    st["per_quarter"] = per_q
    st["cells_above_declared_max_2"] = implausible
    pm = hold.prev_marks(days)
    reasons = {"no_fresh_visible_quarter_row": 0, "anchor_value_nan": 0}
    w = h.FieldWriter(output, "stio_chg_q", role)
    try:
        for t in range(role.n_dates):
            anchor = hold.anchor_quarters(qdays, vis, has, int(days[t]), int(pm[t]))
            ok = anchor >= 0
            row = np.where(ok, dstio[np.maximum(anchor, 0), np.arange(n)], np.nan)
            w.write(row)
            member = role.member[t] != 0
            reasons["no_fresh_visible_quarter_row"] += int(np.count_nonzero(member & ~ok))
            reasons["anchor_value_nan"] += int(np.count_nonzero(member & ok & ~np.isfinite(row)))
            if t % 256 == 0:
                budget.check("mgr13f-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    return {"stio_chg_q": (w, {"nan_reasons_member_cells": reasons})}, st


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "Mgr13fFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_ydata.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_mgr13f: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return Mgr13fFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of the field: the stage manifest, the research seal (review N-1) and the code imported from
    research_fields_holdings.py (imported_code)."""
    return {"stage_manifest_sha256": str(options.get("mgr13f_stage_sha256") or "").lower(), "seal": rw.SEAL_DATE,
            "imported_code": imported_code(producer_group(name))}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    return {"stage_manifest_sha256": entry.get("stage_manifest_sha256"), "seal": entry.get("seal_date"),
            "imported_code": entry.get("imported_code")}


class Mgr13fFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        g = parser.add_argument_group("13F manager-horizon field (research_fields_mgr13f.py: " + ",".join(FIELDS) + ")")
        g.add_argument("--mgr13f-stage", type=Path, help="the atx-db alpha-panel thirteenf/ stage directory")
        g.add_argument("--mgr13f-stage-sha256", help="SHA-256 of that stage's manifest.json")

    @staticmethod
    def check(selected, options: dict):
        """Before any output: the stage and its pin are given."""
        names = [f for f in selected if f in FIELDS]
        if names and (options.get("mgr13f_stage") is None or not options.get("mgr13f_stage_sha256")):
            raise ValueError(f"--fields: {', '.join(names)} need --mgr13f-stage and --mgr13f-stage-sha256")

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL or hold.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_mgr13f: the builder's seal {h.SEAL.isoformat()} is not "
                               f"research_window's {rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        stage = open_stage(options)
        so, so_pin = shares_out_matrix(output, role)
        try:
            results, st = stio_rows(h, stage, role, output, budget, so)
        finally:
            del so
        source_checks[GROUP] = {"visibility_rule": hold.VISIBILITY_RULE, "seal": rw.SEAL_DATE, "window": rw.WINDOW_ID,
                                "stage": stage.check(), "thirteenf": st}
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        for x in names:
            w, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, 1)),
                               "visibility_rule": hold.VISIBILITY_RULE, "min_history": spec["min_history"],
                               "stage_manifest_sha256": stage.manifest_source["sha256"], "seal_date": rw.SEAL_DATE,
                               "imported_code": imported_code(spec["group"]), **extra}
            outcome[x] = (w, stage.sources() + [so_pin], w.coverage())
        budget.report("mgr13f-complete", fields=len(names))
