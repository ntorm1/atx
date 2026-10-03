"""Connected-stocks research field (platform v8 lane YDATA round 2, draft): the 13F common-ownership network.

Anton and Polk (2014, JF 69(3), 1099-1127, "Connected stocks"): two stocks are connected through the active funds that
hold both; the common ownership of a pair is FCAP_ij = sum over the common funds f of (S_i^f P_i + S_j^f P_j) / (S_i P_i
+ S_j P_j) at the quarter end; a stock's connected portfolio weights the stocks it is connected to by the rank of FCAP
(FCAP** = rank if FCAP > 0, else 0) and its return r_iC = sum_j FCAP**_ij r_j / sum_j FCAP**_ij over the past three
months. Price pressure from the common owners' trading moves connected stocks together and then reverses: a low
connected return forecasts a high return over months 2-6 after the sort (the paper's cross-stock-reversal strategy:
independent quintile sorts on the own and the connected three-month return, a month skipped, five months held).
Cohen and Frazzini (2008, JF) customer momentum was the alternative basis; no point-in-time customer-supplier link
exists in house (atx-db PARITY_GAP.md: ``sc_edge`` absent; the segment major-customer capture is a shared-DB surface
without an alpha-panel export), so it is not built.

Field ``conn_ret63`` (formula ``ap-connected-ret63-13f-v1``): the field the v9 library draft pre-registered for C-7
``conn_rev`` (docs/plans/2026-10-01-v9-library-draft.md, D-L3, planned on N-PORT), built here on the 13F route:
* positions of quarter P: the screened mapped 13F rows (research_fields_holdings ``F13_ROWS_RULE``, read by
  research_fields_mgr13f ``quarter_rows`` / ``quarter_positions``): shares N per (filer CIK, security), each security's
  price p = the median implied price of its kept rows;
* nodes (``NODE_RULE``): the role lines that are members at the last role session t_P on or before the quarter end,
  with a finite positive ``shares_out`` at t_P and a 13F price; market value m = shares_out x p;
* active owners (``ACTIVE_RULE``): filers with at least two node positions whose within-book active share
  AS = 1/2 sum_i |V_i / sum V - m_i / sum m| (V = N p over the filer's node positions; the deviation of the book from
  the cap weights of its own holdings, Cremers-Petajisto 2009 with the held set as the benchmark) is at least
  ``ACTIVE_SHARE_MIN``; cap-weighted (index-like) books are not active owners;
* FCAP_ij over the active owners holding both nodes (Anton-Polk's formula); a pair is connected iff FCAP > 0;
* weights (``WEIGHT_RULE``): FCAP** = the average-tie rank of FCAP_ij among all connected pairs of P divided by their
  count; each node keeps its ``TOP_K`` largest-FCAP connections (ties: the lower role column first);
* value at session t: sum w_ij r_j / sum w_ij over the kept connections j with a valid 63-session return ending at
  t-1, r_j = close_(t-1) / close_(t-64) - 1 (role adjusted close, present at both ends, finite and positive), NaN with
  fewer than ``CONN_MIN``.

Clock: the network of quarter P is used under research_fields_holdings ``F13_CLOCK`` (visible at V(P), its last
deadline filing + 46 h; row t reads the latest quarter with V(P) < 22:00 UTC of session t-1, at most 150 days old, in
which the line is a connected node); the returns read the role's close rows <= t-1 (``LAG_SESSIONS`` 1, D-L3's
``price-close-lag1``). Seal (reader side): ``compute``
refuses unless the builder's ``SEAL`` is ``research_window.SEAL``; holdings parts filed on or after the seal are never
opened and a quarter visible on or after the seal is never used.

Memory and time (N nodes <= the role's member count at a quarter end, top_n 3,000 on the v8 roles; F active filers):
per quarter one holdings scan (as ``stio_chg_q``), a dense common-value accumulation A = V H' in filer chunks of
``FILER_CHUNK`` (2 N^2 F flops, BLAS), FCAP and the pair ranks (O(N^2 log N)), top-K by row chunks; peak about
3 x 8 N^2 + 16 N x FILER_CHUNK bytes (216 MiB + 25 MiB at N = 3,000), admitted against the budget before it is
allocated; the kept networks take Q x n x K x 12 bytes (role width n); the daily pass is O(T n K).

Draft and off by default: ``prepare_research_fields_ydata.py`` registers it. It reads the same thirteenf stage as
research_fields_mgr13f (``--mgr13f-stage`` and ``--mgr13f-stage-sha256``) and requires ``shares_out`` in the same run.
``--reuse`` (v8 C-3): the stage pin, the seal and ``imported_code`` (the AST closures of the names imported from
research_fields_holdings.py and research_fields_mgr13f.py) are the input pins.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc

import code_fingerprint  # same directory: the AST closure fingerprints --reuse keys on
import research_fields_holdings as hold  # same directory; it does not import this module
import research_fields_mgr13f as mgr     # same directory; it does not import this module
import research_window as rw
from research_fields_sec import _Host

GROUP = "connected"
OPTIONS = mgr.OPTIONS       # the same thirteenf stage and pin as the 13F manager-horizon module
HOST_HANDLES = ("h",)
BUILDER = "prepare_research_fields.py"
BUILDER_ORCHESTRATION = frozenset({"run", "main"})
NAME = "conn_ret63"         # the v9 library draft's D-L3 field name (C-7 conn_rev)
ACTIVE_SHARE_MIN = 0.2      # Cremers-Petajisto (2009): Active Share below 20% is index-like
TOP_K = 50                  # Anton-Polk Table I: the typical active fund holds 58.5 big stocks; rounded down to 50
WINDOW = 63                 # Anton-Polk: the connected return over the past three months
LAG_SESSIONS = 1            # D-L3: the 63-session return ends at t-1 (price-close-lag1)
CONN_MIN = 10               # mechanical: a fifth of TOP_K valid connected returns for a portfolio
FILER_CHUNK = 512           # filers per dense block of the common-value accumulation (memory bound)
ROW_CHUNK = 256             # nodes per block of the top-K selection (memory bound)

HOLD_IMPORTS = mgr.HOLD_IMPORTS
MGR_IMPORTS = ("quarter_rows", "quarter_positions", "price_on", "open_stage", "shares_out_matrix", "STAGE_KEY")
IMPORTS = ((hold, HOLD_IMPORTS, hold.HOST_HANDLES), (mgr, MGR_IMPORTS, mgr.HOST_HANDLES))

NODE_RULE = (
    "ap-nodes-member-v1: nodes of quarter P = role lines that are members at the last role session t_P <= P, with a "
    "finite positive shares_out at t_P and a 13F price p (median implied price of the kept rows); m = shares_out x p")
ACTIVE_RULE = (
    f"ap-active-share-book-v1: a filer is an active owner iff it has >= 2 node positions and AS = 1/2 sum_i "
    f"|V_i / sum V - m_i / sum m| >= {ACTIVE_SHARE_MIN} over its node positions (V = N x p)")
FCAP_RULE = (
    "ap-fcap-v1: FCAP_ij = sum over the active owners f holding both i and j of (N_i^f p_i + N_j^f p_j) / (m_i + m_j), "
    "i != j; connected iff FCAP > 0")
WEIGHT_RULE = (
    f"ap-rank-top{TOP_K}-v1: w_ij = average-tie rank of FCAP_ij among the connected pairs of P / their count; each node "
    f"keeps its {TOP_K} largest-FCAP connections, ties to the lower role column")
VALUE_RULE = (
    f"ap-connected-ret-v1: sum w_ij r_j / sum w_ij over kept connections with a valid r_j = close_(t-{LAG_SESSIONS}) / "
    f"close_(t-{LAG_SESSIONS + WINDOW}) - 1 (present at both ends, finite, > 0); NaN with fewer than {CONN_MIN} valid")

FIELDS: dict = {
    NAME: {
        "group": "y_conn", "point_in_time": True, "lagged": False, "requires": ["shares_out"],
        "units": "decimal three-month return of the connected-stock portfolio",
        "clock": hold.F13_CLOCK + "; returns: role close rows <= t-1 (price-close-lag1)",
        "staleness": hold.F13_STALENESS,
        "definition": "Anton-Polk (2014) connected-stock return. " + " ".join(
            (hold.F13_ROWS_RULE + ".", NODE_RULE + ".", ACTIVE_RULE + ".", FCAP_RULE + ".", WEIGHT_RULE + ".",
             VALUE_RULE + ".")),
        "source_columns": hold.F13_COLUMNS[:8] + hold.F13_HOLD_COLUMNS + ["shares_out", "role.close", "role.present",
                                                                          "role.member"],
        "caveats": hold.F13_CAVEATS + [
            "13F filers are managers, not funds: a filer's book aggregates its funds, so the network is denser than "
            "Anton-Polk's fund network; the top-K rule keeps the strongest links",
            "active owners are identified from the books (within-book active share), not from fund type "
            "(thirteenf_filer_type is not built): factor and equal-weighted index books count as active",
            "FCAP is not orthogonalized to the pair controls of Anton-Polk Table II (the 'abnormal' connection needs "
            "I/B/E/S common coverage, five-year correlations, headquarters state, S&P 500 and exchange dummies)",
            "shares_out is the house A8 90-day lagged vendor count (one line, not the issuer total)"],
        "formula_id": "ap-connected-ret63-13f-v1",
        "min_history": f"{WINDOW + LAG_SESSIONS} role sessions and one visible 13F quarter (the stage holds 2013q2 on)",
        "needs": "thirteenf_stage",
    }
}

PRODUCERS = {"y_conn": ("open_stage", "conn_rows")}


# ---------------------------------------------------------------------------------------------------------------
# Imported code: the --reuse input pin of what the producers import
# ---------------------------------------------------------------------------------------------------------------

def imported_code(group: str, sources: dict | None = None, builder: bytes | None = None) -> dict:
    """{"modules": [{"module", "names", "sha256"}]}: SHA-256 (code_fingerprint) of the AST closure of the names imported
    from research_fields_holdings.py and research_fields_mgr13f.py, with the builder definitions they read."""
    if group not in PRODUCERS:
        raise ValueError(f"research_fields_connected: unknown producer group {group!r}")
    host = (Path(__file__).resolve().parent / BUILDER).read_bytes() if builder is None else builder
    out = []
    for module, names, handles in IMPORTS:
        path = Path(module.__file__)
        src = path.read_bytes() if sources is None or path.name not in sources else sources[path.name]
        fp = code_fingerprint.fingerprints(src.replace(b"\r\n", b"\n"), {group: names},
                                           host=code_fingerprint.Host(host.replace(b"\r\n", b"\n"), handles,
                                                                      BUILDER_ORCHESTRATION))[group]
        if fp is None:
            raise ValueError(f"research_fields_connected: {path.name} lacks one of {names}")
        out.append({"module": path.name, "names": list(names), "sha256": fp})
    return {"modules": out}


# ---------------------------------------------------------------------------------------------------------------
# Pure helpers (unit-tested directly)
# ---------------------------------------------------------------------------------------------------------------

def active_owners(filer_idx: np.ndarray, node_idx: np.ndarray, value: np.ndarray, cap: np.ndarray, n_filers: int):
    """(active mask, within-book active share, node-position count) per filer (``ACTIVE_RULE``) from the node
    positions (filer index, node index, value N x p) and the nodes' market values ``cap``."""
    count = np.bincount(filer_idx, minlength=n_filers)
    book = np.bincount(filer_idx, weights=value, minlength=n_filers)
    capsum = np.bincount(filer_idx, weights=cap[node_idx], minlength=n_filers)
    ok = (book > 0) & (capsum > 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        dev = np.abs(value / np.where(ok, book, 1.0)[filer_idx] - cap[node_idx] / np.where(ok, capsum, 1.0)[filer_idx])
    share = np.where(ok, 0.5 * np.bincount(filer_idx, weights=dev, minlength=n_filers), np.nan)
    active = ok & (count >= 2) & (share >= ACTIVE_SHARE_MIN)
    return active, share, count


def fcap_matrix(filer_idx: np.ndarray, node_idx: np.ndarray, value: np.ndarray, cap: np.ndarray, n_nodes: int,
                budget=None, chunk: int | None = None) -> np.ndarray:
    """FCAP (``FCAP_RULE``, dense n_nodes x n_nodes, zero diagonal) from the active owners' node positions (filer
    index 0..F-1, node index, value). A = V H' is accumulated over blocks of ``chunk`` filers; FCAP = (A + A') / (m_i
    + m_j)."""
    chunk = chunk or FILER_CHUNK
    n_filers = int(filer_idx.max()) + 1 if len(filer_idx) else 0
    a = np.zeros((n_nodes, n_nodes))
    order = np.argsort(filer_idx, kind="stable")
    fi, ni, va = filer_idx[order], node_idx[order], value[order]
    bounds = np.searchsorted(fi, np.arange(0, n_filers + chunk, chunk))
    for b in range(len(bounds) - 1):
        lo, hi = int(bounds[b]), int(bounds[b + 1])
        if lo == hi:
            continue
        cols = fi[lo:hi] - b * chunk
        v = np.zeros((n_nodes, chunk))
        v[ni[lo:hi], cols] = va[lo:hi]
        hmat = (v > 0).astype(np.float64)
        a += v @ hmat.T
        if budget is not None:
            budget.check("conn-fcap")
    s = a + a.T
    del a
    np.fill_diagonal(s, 0.0)
    for r0 in range(0, n_nodes, ROW_CHUNK):
        r1 = min(r0 + ROW_CHUNK, n_nodes)
        s[r0:r1] /= cap[r0:r1, None] + cap[None, :]
    return s


def top_connections(fcap: np.ndarray, k: int | None = None) -> tuple:
    """(neighbour node index (n x k, -1 pad), weight (n x k, 0 pad), connected pairs) under ``WEIGHT_RULE``."""
    k = TOP_K if k is None else k
    n = fcap.shape[0]
    ups = np.sort(np.concatenate([fcap[i, i + 1:][fcap[i, i + 1:] > 0] for i in range(n)] or [np.zeros(0)]))
    m = len(ups)
    nbr = np.full((n, k), -1, dtype=np.int64)
    wgt = np.zeros((n, k))
    kk = min(k, n)
    for r0 in range(0, n, ROW_CHUNK):
        r1 = min(r0 + ROW_CHUNK, n)
        block = fcap[r0:r1]
        order = np.argsort(-block, axis=1, kind="stable")[:, :kk]
        vals = np.take_along_axis(block, order, axis=1)
        keep = vals > 0
        lo = np.searchsorted(ups, vals, side="left")
        hi = np.searchsorted(ups, vals, side="right")
        rank = (lo + 1 + hi) / 2.0
        nbr[r0:r1, :kk] = np.where(keep, order, -1)
        wgt[r0:r1, :kk] = np.where(keep, rank / max(m, 1), 0.0)
    return nbr, wgt, m


def connected_value(nbr_cols: np.ndarray, weight: np.ndarray, ret: np.ndarray) -> tuple:
    """(value, valid count) per row of kept connections (role columns, -1 pad) under ``VALUE_RULE``."""
    r = ret[np.maximum(nbr_cols, 0)]
    valid = (nbr_cols >= 0) & np.isfinite(r)
    w = np.where(valid, weight, 0.0)
    num = np.sum(w * np.where(valid, r, 0.0), axis=1)
    den = np.sum(w, axis=1)
    cnt = np.count_nonzero(valid, axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        out = np.where((cnt >= CONN_MIN) & (den > 0), num / np.where(den > 0, den, 1.0), np.nan)
    return out, cnt


def window_returns(ring_close: np.ndarray, ring_present: np.ndarray, t: int) -> np.ndarray:
    """r_j = close_e / close_(e-WINDOW) - 1, e = t - LAG_SESSIONS, from the ring holding rows t-R+1..t (row s at s % R,
    R = its length >= WINDOW + LAG_SESSIONS + 1); NaN before row WINDOW + LAG_SESSIONS."""
    size, n = ring_close.shape
    e = t - LAG_SESSIONS
    if e - WINDOW < 0:
        return np.full(n, np.nan)
    if size < WINDOW + LAG_SESSIONS + 1:
        raise ValueError("conn_ret63: the return ring is shorter than the window and the lag")
    a, b = e % size, (e - WINDOW) % size
    c1, c0 = ring_close[a], ring_close[b]
    ok = ring_present[a] & ring_present[b] & np.isfinite(c1) & np.isfinite(c0) & (c1 > 0) & (c0 > 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(ok, c1 / np.where(ok, c0, 1.0) - 1.0, np.nan)


def network(pos: dict, node_cols: np.ndarray, node_sids: np.ndarray, so_row: np.ndarray, budget=None) -> tuple:
    """One quarter's kept connections: (neighbour role columns (n_nodes x TOP_K), weights, node role columns, stats)
    from its positions ``pos`` and the candidate nodes (member role columns with a positive shares_out)."""
    price = mgr.price_on(pos, node_sids)
    cap = so_row[node_cols] * price
    good = np.isfinite(cap) & (cap > 0)
    node_cols, node_sids, price, cap = node_cols[good], node_sids[good], price[good], cap[good]
    n_nodes = len(node_cols)
    sel = np.isin(pos["pair_sid"], node_sids)
    filers, fidx = np.unique(pos["pair_filer"][sel], return_inverse=True)
    nidx = np.searchsorted(node_sids, pos["pair_sid"][sel])
    value = pos["pair_shares"][sel] * price[nidx]
    active, share, count = active_owners(fidx, nidx, value, cap, len(filers))
    st = {"nodes": int(n_nodes), "filers_with_node_positions": int(len(filers)),
          "filers_single_node": int(np.count_nonzero(count == 1)),
          "filers_index_like": int(np.count_nonzero((count >= 2) & ~active)), "active_owners": int(active.sum())}
    keep = active[fidx]
    _, aidx = np.unique(fidx[keep], return_inverse=True)
    if budget is not None:
        budget.admit(3 * 8 * n_nodes * n_nodes + 2 * 8 * n_nodes * FILER_CHUNK, "conn-network")
    fcap = fcap_matrix(aidx, nidx[keep], value[keep], cap, n_nodes, budget)
    nbr, wgt, pairs = top_connections(fcap)
    del fcap
    st["connected_pairs"] = int(pairs)
    st["pair_density"] = round(pairs / (n_nodes * (n_nodes - 1) / 2), 6) if n_nodes > 1 else None
    kept = np.count_nonzero(nbr >= 0, axis=1)
    st["nodes_with_connections"] = int(np.count_nonzero(kept))
    st["median_kept_connections"] = float(np.median(kept)) if n_nodes else None
    cols = np.where(nbr >= 0, node_cols[np.maximum(nbr, 0)], -1)
    return cols, wgt, node_cols, st


# ---------------------------------------------------------------------------------------------------------------
# The producer
# ---------------------------------------------------------------------------------------------------------------

def open_stage(options: dict):
    return mgr.open_stage(options)


def conn_rows(h, stage, role, output: Path, budget, so) -> tuple:
    """``conn_ret63`` (its field definition): ({name: (writer, extras)}, role sources, source checks)."""
    n = role.n
    days = role.days.astype(np.int64)
    quarters = hold.needed_quarters(hold.date_of(int(days[0])), hold.date_of(int(days[-1])))[1:]
    qdays = np.array([hold.day_of(q) for q in quarters], dtype=np.int64)
    nq = len(quarters)
    budget.admit(nq * n * TOP_K * 12 + n * (WINDOW + LAG_SESSIONS + 1) * 9 + (384 << 20), "conn-quarters")
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
          "rules": {"node": NODE_RULE, "active": ACTIVE_RULE, "fcap": FCAP_RULE, "weight": WEIGHT_RULE,
                    "value": VALUE_RULE}}
    del f
    checks = stage.table("filing_checks.parquet", ["accession", "unit_factor"])
    uf_acc, uf_val = hold._col(checks, "accession"), hold._f64(hold._col(checks, "unit_factor"))
    del checks
    cmap = stage.table("cusip_map_pit.parquet", ["period_q", "cusip", "security_id"])
    cm_q, cm_cusip, cm_sid = hold._days(hold._col(cmap, "period_q")), hold._col(cmap, "cusip"), \
        hold._ids(hold._col(cmap, "security_id"))
    del cmap
    budget.check("conn-inputs")
    eq = np.searchsorted(qdays, period[eidx])
    nets: dict = {}
    has = np.zeros((nq, n), dtype=bool)
    per_q = {}
    for q in range(nq):
        tq = int(np.searchsorted(days, qdays[q], side="right")) - 1
        rec = {}
        if has_filing[q] and not sealed[q] and tq >= 0:
            cm = cm_q == qdays[q]
            raw = mgr.quarter_rows(stage, eidx[eq == q], acc_all, filer_all, src_all, uf_acc, uf_val,
                                   cm_cusip.filter(pa.array(cm)), cm_sid[cm], st, budget)
            pos = mgr.quarter_positions(raw["filer"], raw["sid"], raw["shares"], raw["value"])
            del raw
            so_row = np.asarray(so[tq], dtype=np.float64)
            cand = np.flatnonzero((role.member[tq] != 0) & np.isfinite(so_row) & (so_row > 0))
            cols, wgt, node_cols, rec = network(pos, cand, role.ids[cand], so_row, budget)
            del pos
            full_c = np.full((n, TOP_K), -1, dtype=np.int32)
            full_w = np.zeros((n, TOP_K))
            full_c[node_cols] = cols
            full_w[node_cols] = wgt
            nets[q] = (full_c, full_w)
            has[q, node_cols[np.any(cols >= 0, axis=1)]] = True
            rec["role_session"] = hold.date_of(int(days[tq])).isoformat()
        elif has_filing[q] and not sealed[q]:
            rec["not_built"] = "the quarter end is before the first role session"
        per_q[quarters[q].isoformat()] = rec
        hold._release()
        budget.report("conn-quarter", quarter=quarters[q].isoformat())
    has &= ~(vis[:, None] == hold.NEVER)
    st["per_quarter"] = per_q
    pm = hold.prev_marks(days)
    reasons = {"no_fresh_visible_network": 0, "fewer_than_conn_min_valid_returns": 0}
    size = WINDOW + LAG_SESSIONS + 1
    ring_c = np.full((size, n), np.nan)
    ring_p = np.zeros((size, n), dtype=bool)
    rows = h.RoleRows(role, ["close.f64", "present.u8"])
    w = None
    try:
        try:
            w = h.FieldWriter(output, NAME, role)
            for t in range(role.n_dates):
                ring_c[t % size] = rows.row("close.f64")
                ring_p[t % size] = rows.row("present.u8") != 0
                ret = window_returns(ring_c, ring_p, t)
                anchor = hold.anchor_quarters(qdays, vis, has, int(days[t]), int(pm[t]))
                row = np.full(n, np.nan)
                for q in np.unique(anchor[anchor >= 0]):
                    lines = np.flatnonzero(anchor == q)
                    c, wt = nets[int(q)]
                    row[lines] = connected_value(c[lines].astype(np.int64), wt[lines], ret)[0]
                w.write(row)
                member = role.member[t] != 0
                reasons["no_fresh_visible_network"] += int(np.count_nonzero(member & (anchor < 0)))
                reasons["fewer_than_conn_min_valid_returns"] += int(np.count_nonzero(
                    member & (anchor >= 0) & ~np.isfinite(row)))
                if t % 256 == 0:
                    budget.check("conn-write")
            sources = rows.verify()
        finally:
            rows.close()
    except BaseException:
        if w is not None:
            w.f.close()
        raise
    w.close()
    return {NAME: (w, {"nan_reasons_member_cells": reasons})}, sources, st


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "ConnectedFieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object.
    The plain builder never calls it (draft: prepare_research_fields_ydata.register)."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_connected: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return ConnectedFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of the field: the stage manifest, the research seal and the imported code."""
    return {"stage_manifest_sha256": str(options.get("mgr13f_stage_sha256") or "").lower(), "seal": rw.SEAL_DATE,
            "imported_code": imported_code(producer_group(name))}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    return {"stage_manifest_sha256": entry.get("stage_manifest_sha256"), "seal": entry.get("seal_date"),
            "imported_code": entry.get("imported_code")}


class ConnectedFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """The stage options are the 13F manager-horizon module's; added here only when that module is absent."""
        if "--mgr13f-stage" in parser._option_string_actions:
            return
        mgr.Mgr13fFieldModule.add_arguments(parser)

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
            raise rw.SealError(f"research_fields_connected: the builder's seal {h.SEAL.isoformat()} is not "
                               f"research_window's {rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        stage = open_stage(options)
        so, so_pin = mgr.shares_out_matrix(output, role)
        try:
            results, role_sources, st = conn_rows(h, stage, role, output, budget, so)
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
                               "imported_code": imported_code(spec["group"]),
                               "constants": {"active_share_min": ACTIVE_SHARE_MIN, "top_k": TOP_K,
                                             "window": WINDOW, "lag_sessions": LAG_SESSIONS, "conn_min": CONN_MIN},
                               **extra}
            outcome[x] = (w, stage.sources() + [so_pin] + role_sources, w.coverage())
        budget.report("conn-complete", fields=len(names))
