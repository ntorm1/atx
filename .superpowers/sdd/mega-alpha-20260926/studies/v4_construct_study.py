#!/usr/bin/env python3
"""v4_construct_study.py -- T26 v4 construction study: industry-neutral and liquidity-aware paper books.

TRAIN only (2020-2022); analysis only, the root runs it. Book level only: it reads the saved v4 combined
signal (never a candidate or theme series) and writes book-level statistics only.

Paper books on the fitter's price-risk context columns, one decision d per session, gross P&L
p(d) = w(d) . r[d+2] (r = the fitter's valid guarded simple return, invalid -> 0; the NAV's decide d /
fill close d+1 / first return row d+2 timing):

  P0  price-risk-v1 baseline: fit_composition_weights.Context.book of the combined, i.e. centered tied
      rank over used & finite, OLS residual on [1, z_beta252, z_vol63, z_ladv63] (projected twice), gross 1.
      This is exactly postmortem_v3 section E's "neutral" book.
  P1  P0's design + FF12 industry dummies (grp_ff12; NaN -> one "unknown" group), residual projected
      twice, gross 1.
  P2  P0's design + FF49 dummies (grp_ff49), groups per FF49_RULE below.
  P3  P1 weights x min(1, ADV63_i / median ADV63)^0.5 (median over the decision's used names; ADV63 =
      mean raw dollar volume over the 63 sessions ending d, as price-risk-v1's ladv), re-neutralized on
      P1's design, gross 1.
  P4  partial adjustment toward the P1 target: h_t = w_{t-1} with forced exits (0) for names that are not
      decision members (member & present) at t, then w_t = h_t + 0.25 (P1_t - h_t); deployed at the first
      live P1 target; a non-live P1 target holds h_t.

Per book: annualized mean / vol / Sharpe (sqrt 252, ddof 1), Sharpe by decision year, mean and p95 daily
turnover sum_i |w_t - w_{t-1}| of the (drift-free) paper weights with the first decision (deployment)
excluded, a cost proxy of 20 bps (declared constant) x that turnover -> net-proxy series and Sharpe, and
the correlation with the NAV gross series (row d+2). Diagnostics: mean gross, turnover per unit of prior
gross, FF12 net industry exposure, size (log me_company) exposure, share of gross below the median ADV63.

Refuses: a role, combined, fields manifest or NAV row dated on/after 2023-01-01; inputs not bound to the
same TRAIN role. Budget (<= 180 s, <= 1536 MiB): the real TRAIN context build is ~19 s / ~0.4 GiB; the
whole study is expected at ~40-70 s and ~0.8 GiB.
"""
from __future__ import annotations

import os

for _var in ("OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "OMP_NUM_THREADS"):
    os.environ.setdefault(_var, "1")

import argparse  # noqa: E402
import csv  # noqa: E402
import json  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402
from pathlib import Path  # noqa: E402

import numpy as np  # noqa: E402

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import postmortem_v3 as PM  # noqa: E402  (loaders, stats, combined/NAV helpers)

SCHEMA = "atx.v4-construct-study/v1"
BOOKS = ("P0", "P1", "P2", "P3", "P4")
DEFAULT_ROOT = Path("C:/atx-wt/pool-2")
SCENARIO = "modeled-1bn-stale5-v1+swap-fin-v1"
FIELDS_SCHEMA = "atx.research-role-fields/v1"
TRAIN_END_NS = 1_672_531_200_000_000_000  # 2023-01-01T00:00Z, exclusive
# Declared constants (never tuned on results).
COST_BPS = 20.0
PARTIAL = 0.25
MIN_GROUP = 5
LIQ_EXPONENT = 0.5
LAG = 2
RCOND = 1e-10  # relative singular-value floor of the per-decision design span
UNKNOWN = 0  # industry label of names with no usable code
REMAINDER = 100  # FF49 fallback groups: REMAINDER + FF12 parent code
# Pins of the v4 TRAIN run (reported as checks, never used to select anything).
V4_ROLE_SHA = "210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de"
V4_COMBINED_SHA = "24a6cc76fc110a91b8e204dfde3c34d6a94f94dbe291159eb63a9994c1d030f3"
V4_FIELDS_SHA = "32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8"
FIELD_NAMES = ("grp_ff12", "grp_ff49", "me_company")
FF12_RULE = "grp_ff12 code 1..12 per name and decision; NaN -> one 'unknown' group"
FF49_RULE = ("per decision over the used names: a name keeps its FF49 code when that code is finite and its FF49 "
             "group has >= 5 used names; otherwise (FF49 NaN, or a group < 5) it joins its FF12-parent remainder "
             "group (its own grp_ff12 code on that decision), or 'unknown' when grp_ff12 is NaN; a remainder group "
             "left with < 5 names joins 'unknown'")
DESCRIPTIONS = {
    "P0": "price-risk-v1 (baseline; fitter Context.book == postmortem E neutral)",
    "P1": "price-risk-v1 + FF12 dummies",
    "P2": "price-risk-v1 + FF49 dummies (small groups merged)",
    "P3": "P1 x min(1, ADV63/median)^0.5, re-neutralized (P1 design)",
    "P4": "P1 target, partial adjustment 0.25/day",
}

need = PM.need
StudyError = PM.PostmortemError
_F = None  # the imported fitter module (bind_fitter)


def bind_fitter(path: Path):
    global _F
    _F = PM.import_fitter(Path(path))
    return _F


# ------------------------------------------------------------------------------ inputs
def load_fields(fields_dir: Path, role, names=FIELD_NAMES) -> tuple[dict, str, dict]:
    """Receipt-verified research-role fields bound to ``role`` (full dates x instruments arrays)."""
    manifest = Path(fields_dir) / "manifest.json"
    raw = manifest.read_bytes()
    j = json.loads(raw)
    need(j.get("schema") == FIELDS_SCHEMA and j.get("status") == "complete", "fields: schema/status")
    rj = j.get("role")
    need(isinstance(rj, dict), "fields: no role binding")
    need(rj.get("manifest_sha256") == role.sha, "fields: bound to another role manifest")
    need(int(rj.get("dates", -1)) == role.dates and int(rj.get("instruments", -1)) == role.instruments,
         "fields: role axes differ")
    last = rj.get("last_session")
    need(isinstance(last, str) and last < "2023-01-01", "fields: role not TRAIN-only (last_session >= 2023-01-01)")
    entries = {e.get("name"): e for e in j.get("fields", []) if isinstance(e, dict)}
    out = {}
    for name in names:
        e = entries.get(name)
        need(e is not None, f"fields: {name} absent")
        need(e.get("dtype") == "<f8" and list(e.get("shape", [])) == [role.dates, role.instruments],
             f"fields: {name} dtype/shape")
        data = (Path(fields_dir) / e["file"]).read_bytes()
        need(len(data) == role.dates * role.instruments * 8, f"fields: {name} extent")
        need(PM.sha256_bytes(data) == e["sha256"], f"fields: {name} SHA-256")
        out[name] = np.frombuffer(data, dtype="<f8").reshape(role.dates, role.instruments)
    return out, PM.sha256_bytes(raw), rj


def check_codes(x: np.ndarray, lo: int, hi: int, name: str) -> None:
    v = x[np.isfinite(x)]
    need(bool(np.all((v == np.round(v)) & (v >= lo) & (v <= hi))), f"fields: {name} codes outside integer {lo}..{hi}")


def read_nav_train(path: Path) -> dict[int, tuple[float, float]]:
    """{session_ns: (gross, net)} of the return rows; refuses any row dated on/after 2023-01-01."""
    out = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            s = int(row["session_ns"])
            need(s < TRAIN_END_NS, f"{path}: NAV row on/after 2023-01-01 (TRAIN only)")
            if row.get("return_observation") == "1":
                out[s] = (float(row["gross_return"]), float(row["net_return"]))
    return out


# ------------------------------------------------------------------------------ neutralization
def ff12_labels(ff12: np.ndarray) -> np.ndarray:
    lab = np.full(ff12.shape, UNKNOWN, dtype=np.int64)
    ok = np.isfinite(ff12)
    lab[ok] = ff12[ok].astype(np.int64)
    return lab


def ff49_labels(ff49: np.ndarray, ff12: np.ndarray, min_group: int = MIN_GROUP) -> np.ndarray:
    """FF49_RULE on one decision's used names (1-D rows)."""
    lab = np.full(ff49.shape, -1, dtype=np.int64)
    ok = np.isfinite(ff49)
    lab[ok] = ff49[ok].astype(np.int64)
    _, inverse, counts = np.unique(lab, return_inverse=True, return_counts=True)
    fallback = (lab < 0) | (counts[inverse] < min_group)
    parent = np.full(ff12.shape, UNKNOWN, dtype=np.int64)
    ok12 = np.isfinite(ff12)
    parent[ok12] = REMAINDER + ff12[ok12].astype(np.int64)
    lab = np.where(fallback, parent, lab)
    remainder = lab >= REMAINDER
    if remainder.any():
        _, inverse, counts = np.unique(lab, return_inverse=True, return_counts=True)
        lab = np.where(remainder & (counts[inverse] < min_group), UNKNOWN, lab)
    return lab


def dummy_columns(labels: np.ndarray) -> np.ndarray:
    """One-hot group columns without the largest group (the design's intercept spans it)."""
    codes, inverse, counts = np.unique(labels, return_inverse=True, return_counts=True)
    if codes.size <= 1:
        return np.zeros((labels.size, 0))
    ref = int(np.argmax(counts))
    keep = np.array([k for k in range(codes.size) if k != ref])
    return (inverse[:, None] == keep[None, :]).astype(np.float64)


def orthonormal_span(x: np.ndarray) -> np.ndarray:
    """Orthonormal basis of the column span of x (singular values <= RCOND * max dropped)."""
    if x.shape[1] == 0:
        return x
    u, s, _ = np.linalg.svd(x, full_matrices=False)
    if not (s.size and s[0] > 0):
        return u[:, :0]
    return u[:, s > RCOND * s[0]]


def normalized_residual(span: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, bool]:
    """Residual of y off span (projected twice, as the fitter/C++), gross 1; live as Context.book."""
    r = y - span @ (span.T @ y)
    r = r - span @ (span.T @ r)
    entry = float(np.abs(y).sum())
    gross = float(np.abs(r).sum())
    live = entry > 0 and np.isfinite(gross) and gross > _F.MIN_RESIDUAL_FRACTION * entry
    return (r / gross if live else np.zeros_like(r)), live


def liquidity_scale(adv: np.ndarray, exponent: float = LIQ_EXPONENT) -> np.ndarray:
    """min(1, ADV_i / median ADV)^exponent over the decision's used names (ADV > 0 required)."""
    need(bool(np.all(np.isfinite(adv) & (adv > 0))), "ADV63 not finite positive on a used name")
    return np.minimum(1.0, adv / float(np.median(adv))) ** exponent


def partial_book(target: np.ndarray, target_live: np.ndarray, member: np.ndarray,
                 fraction: float = PARTIAL) -> tuple[np.ndarray, np.ndarray]:
    """w_t = h_t + fraction (T_t - h_t), h_t = w_{t-1} zeroed off ``member[t]`` (forced exits).

    Deployed at the first live target (w = T); a non-live target holds h_t (forced exits still apply).
    """
    t, width = target.shape
    w = np.zeros((t, width))
    prev = np.zeros(width)
    started = False
    for j in range(t):
        h = np.where(member[j], prev, 0.0)
        if target_live[j] and not started:
            cur = target[j].copy()
            started = True
        elif target_live[j]:
            cur = h + fraction * (target[j] - h)
        else:
            cur = h
        w[j] = cur
        prev = cur
    return w, np.abs(w).sum(axis=1) > 0


def construct_books(ctx, combined: np.ndarray, ff12: np.ndarray, ff49: np.ndarray, adv: np.ndarray,
                    member: np.ndarray, wanted, log=None) -> tuple[dict, dict, dict]:
    """Requested books (decisions x ctx.columns), their live masks and per-decision group statistics.

    ff12 / ff49 / adv / member are (decisions x ctx.columns) slabs aligned with ctx rows/columns.
    """
    t, width = ctx.used.shape
    wanted = set(wanted)
    books, live = {}, {}
    if "P0" in wanted:
        books["P0"], live["P0"] = ctx.book(combined, 1)
    need1 = bool(wanted & {"P1", "P3", "P4"})
    need2, need3 = "P2" in wanted, "P3" in wanted
    info = {"used": np.zeros(t), "unknown12": np.zeros(t), "groups12": np.zeros(t), "groups49": np.zeros(t),
            "ff49_own": np.zeros(t), "ff49_remainder": np.zeros(t), "unknown49": np.zeros(t)}
    if not (need1 or need2):
        return books, live, info
    slab = combined[ctx.begin:ctx.end][:, ctx.columns]
    q = _F.centered_tied_ranks(slab, ctx.used & np.isfinite(slab))
    del slab
    p1 = np.zeros((t, width)) if need1 else None
    p2 = np.zeros((t, width)) if need2 else None
    p3 = np.zeros((t, width)) if need3 else None
    l1, l2, l3 = np.zeros(t, dtype=bool), np.zeros(t, dtype=bool), np.zeros(t, dtype=bool)
    tick = time.perf_counter()
    for j in range(t):
        u = np.flatnonzero(ctx.used[j])
        if u.size == 0:
            continue
        y = q[j, u]
        base = ctx.basis[j][:, u].T  # orthonormal span of [1, z_beta, z_vol, z_ladv] on the used rows
        lab12 = ff12_labels(ff12[j, u])
        info["used"][j] = u.size
        info["unknown12"][j] = float(np.mean(lab12 == UNKNOWN))
        info["groups12"][j] = np.unique(lab12).size
        if need1:
            span1 = orthonormal_span(np.column_stack([base, dummy_columns(lab12)]))
            w1, ok1 = normalized_residual(span1, y)
            if ok1:
                p1[j, u], l1[j] = w1, True
                if need3:
                    w3, ok3 = normalized_residual(span1, w1 * liquidity_scale(adv[j, u]))
                    if ok3:
                        p3[j, u], l3[j] = w3, True
        if need2:
            lab49 = ff49_labels(ff49[j, u], ff12[j, u])
            info["groups49"][j] = np.unique(lab49).size
            info["ff49_own"][j] = float(np.mean((lab49 > 0) & (lab49 < REMAINDER)))
            info["ff49_remainder"][j] = float(np.mean(lab49 >= REMAINDER))
            info["unknown49"][j] = float(np.mean(lab49 == UNKNOWN))
            w2, ok2 = normalized_residual(orthonormal_span(np.column_stack([base, dummy_columns(lab49)])), y)
            if ok2:
                p2[j, u], l2[j] = w2, True
    if log:
        log(f"industry books: {t} decisions in {time.perf_counter() - tick:.1f} s")
    if "P1" in wanted:
        books["P1"], live["P1"] = p1, l1
    if need2:
        books["P2"], live["P2"] = p2, l2
    if need3:
        books["P3"], live["P3"] = p3, l3
    if "P4" in wanted:
        books["P4"], live["P4"] = partial_book(p1, l1, member, PARTIAL)
        info["p4_tracking_l1"] = np.abs(books["P4"] - p1).sum(axis=1)
    return books, live, info


# ------------------------------------------------------------------------------ statistics
def book_series(w: np.ndarray, forward: np.ndarray, cost_bps: float = COST_BPS) -> dict:
    """Paper P&L, turnover (deployment row 0 excluded: turnover[0] = 0), cost proxy and net-proxy series."""
    pnl = (w * forward).sum(axis=1)
    turn = np.zeros(w.shape[0])
    if w.shape[0] > 1:
        turn[1:] = np.abs(np.diff(w, axis=0)).sum(axis=1)
    cost = cost_bps * 1e-4 * turn
    return {"pnl": pnl, "turnover": turn, "cost": cost, "net": pnl - cost, "gross": np.abs(w).sum(axis=1)}


def book_stats(s: dict, years: np.ndarray) -> dict:
    turn = s["turnover"][1:]
    prior = s["gross"][:-1]
    per_gmv = turn[prior > 0] / prior[prior > 0]
    ys = sorted(set(years.tolist()))
    return {
        "gross": PM.ann_stats(s["pnl"]), "net_proxy": PM.ann_stats(s["net"]),
        "by_year_sharpe": {int(y): PM.sharpe(s["pnl"][years == y]) for y in ys},
        "by_year_net_proxy_sharpe": {int(y): PM.sharpe(s["net"][years == y]) for y in ys},
        "tau_mean": float(turn.mean()) if turn.size else None,
        "tau_p95": float(np.quantile(turn, 0.95)) if turn.size else None,
        "tau_per_prior_gross_mean": float(per_gmv.mean()) if per_gmv.size else None,
        "cost_proxy_total": float(s["cost"].sum()), "cost_proxy_ann": float(s["cost"].mean()) * PM.ANN,
        "mean_gross": float(s["gross"].mean()),
    }


def exposures(w: np.ndarray, ff12: np.ndarray, used: np.ndarray, zsize: np.ndarray, below: np.ndarray) -> dict:
    lab = np.where(np.isfinite(ff12), ff12, UNKNOWN).astype(np.int64)
    net = np.zeros(w.shape[0])
    for g in np.unique(lab[used]):
        net += np.abs((w * (lab == g)).sum(axis=1))
    size = (w * zsize).sum(axis=1)
    gross = np.abs(w).sum(axis=1)
    live = gross > 0
    illiquid = (np.abs(w) * below).sum(axis=1)[live] / gross[live]
    return {"ff12_net_abs_mean": float(net[live].mean()) if live.any() else None,
            "size_exposure_mean": float(size[live].mean()) if live.any() else None,
            "size_exposure_abs_mean": float(np.abs(size[live]).mean()) if live.any() else None,
            "gross_share_below_median_adv": float(illiquid.mean()) if live.any() else None}


def size_z(me: np.ndarray, used: np.ndarray) -> tuple[np.ndarray, float]:
    """Per decision z of log me_company over used names with a finite positive value (clip 5), else 0."""
    z = np.zeros(me.shape)
    cover = []
    with np.errstate(invalid="ignore", divide="ignore"):
        for j in range(me.shape[0]):
            ok = used[j] & np.isfinite(me[j]) & (me[j] > 0)
            if used[j].any():
                cover.append(ok.sum() / used[j].sum())
            if ok.sum() >= 3:
                x = np.log(me[j, ok])
                sd = x.std(ddof=1)
                if sd > 0:
                    z[j, ok] = np.clip((x - x.mean()) / sd, -_F.CLIP_Z, _F.CLIP_Z)
    return z, float(np.mean(cover)) if cover else 0.0


# ------------------------------------------------------------------------------ study
def run_study(a, log=None) -> dict:
    F = _F
    timings = {}
    tick = time.perf_counter()
    msha = PM.sha256_file(a.train_role)
    role = F.RoleManifest(a.train_role, msha)  # TRAIN seal: refuses anything scored/read on/after 2023-01-01
    combined, comb_meta, comb_sha = PM.load_combined(Path(a.train_run), a.prefix, role)
    ctx = F.Context.build(role, log)
    timings["context"] = time.perf_counter() - tick
    tick = time.perf_counter()
    cols, begin, end = ctx.columns, ctx.begin, ctx.end
    t = end - begin
    p = role.payload()
    panel = F.PricePanel(p["close"], p["raw_close"], p["volume"], p["present"])
    member = (p["member"].astype(bool) & p["present"].astype(bool))[begin:end][:, cols]
    del p
    r0 = np.where(np.isnan(panel.returns), 0.0, panel.returns)
    fwd_diff = float(np.max(np.abs(ctx.forward - r0[begin + LAG:end + LAG][:, cols]))) if t else 0.0
    r_after = r0[begin + 1:][:, cols]  # r[d+L] for the P0 lag-1/2/3 NAV alignment check
    del r0
    adv = np.zeros((t, cols.size))
    for j in range(t):  # price-risk-v1 ladv window: sessions (d-63, d]
        d = begin + j
        adv[j] = panel.dollars[d + 1 - F.ADV_WINDOW:d + 1][:, cols].sum(axis=0) / F.ADV_WINDOW
    del panel
    fields, fields_sha, fields_role = load_fields(Path(a.fields), role)
    ff12 = np.array(fields["grp_ff12"][begin:end][:, cols])
    ff49 = np.array(fields["grp_ff49"][begin:end][:, cols])
    me = np.array(fields["me_company"][begin:end][:, cols])
    del fields
    check_codes(ff12, 1, 12, "grp_ff12")
    check_codes(ff49, 1, 49, "grp_ff49")
    timings["inputs"] = time.perf_counter() - tick
    tick = time.perf_counter()
    books, live, info = construct_books(ctx, combined, ff12, ff49, adv, member, a.books, log)
    del combined
    timings["books"] = time.perf_counter() - tick
    tick = time.perf_counter()
    used = ctx.used
    zsize, size_cover = size_z(me, used)
    del me
    below = np.zeros(adv.shape, dtype=bool)
    for j in range(t):
        if used[j].any():
            below[j] = used[j] & (adv[j] < np.median(adv[j][used[j]]))
    years = PM.years_of(role.sessions[begin:end])
    nav_path = Path(a.train_nav) / f"daily_{a.scenario}.csv"
    nav = read_nav_train(nav_path) if nav_path.is_file() else None
    recipe_path = Path(a.train_nav) / "recipe.json"
    recipe = json.loads(recipe_path.read_bytes()) if recipe_path.is_file() else None
    out_books = {}
    series = {}
    for name in BOOKS:
        if name not in books:
            continue
        s = book_series(books[name], ctx.forward)
        series[name] = s
        st = book_stats(s, years)
        st.update(exposures(books[name], ff12, used, zsize, below))
        st["description"] = DESCRIPTIONS[name]
        st["live_decisions"] = int(live[name].sum())
        if nav is not None:
            xs, ys = PM.aligned(s["pnl"], role.sessions, begin, LAG, nav, 0)
            xn, yn = PM.aligned(s["net"], role.sessions, begin, LAG, nav, 1)
            st["nav"] = {"n": int(xs.size), "corr_gross": PM.corr(xs, ys), "corr_net_proxy_vs_nav_net": PM.corr(xn, yn)}
        out_books[name] = st
    if "P0" in books:
        out_books["P0"]["tau_equals_fitter_standalone_turnover"] = (
            out_books["P0"]["tau_mean"] == _F.standalone_turnover(books["P0"]))
        if nav is not None:
            out_books["P0"]["nav_alignment"] = lag_alignment(books["P0"], r_after, role.sessions, begin, nav)
    del r_after
    if "P4" in books:
        out_books["P4"]["tracking_l1_mean"] = float(info["p4_tracking_l1"].mean())
        if "P1" in series:
            out_books["P4"]["corr_pnl_with_P1"] = PM.corr(series["P4"]["pnl"], series["P1"]["pnl"])
    for name in out_books:
        if name != "P0" and "P0" in out_books:
            b, b0 = out_books[name], out_books["P0"]
            b["vs_P0"] = {"d_gross_sharpe": _diff(b["gross"]["sharpe"], b0["gross"]["sharpe"]),
                          "d_net_proxy_sharpe": _diff(b["net_proxy"]["sharpe"], b0["net_proxy"]["sharpe"]),
                          "tau_ratio": (b["tau_mean"] / b0["tau_mean"]) if b0["tau_mean"] else None,
                          "corr_pnl": PM.corr(series[name]["pnl"], series["P0"]["pnl"])}
    timings["stats"] = time.perf_counter() - tick
    nav_out = {"file": str(nav_path), "status": "absent" if nav is None else "read"}
    if nav is not None:
        g = np.array([v[0] for _, v in sorted(nav.items())])
        n_ = np.array([v[1] for _, v in sorted(nav.items())])
        nav_out.update(return_rows=len(nav), gross=PM.ann_stats(g), net=PM.ann_stats(n_))
    if recipe is not None:
        nav_out.update(binds_combined=recipe.get("combined_sha256") == comb_sha,
                       binds_role=recipe.get("role_sha256") == msha,
                       rule=recipe.get("rule"), cadence=recipe.get("cadence"),
                       band_multiple=recipe.get("band_multiple"), trade_fraction=recipe.get("trade_fraction"))
    live_rows = info["used"] > 0
    groups = {"ff12_rule": FF12_RULE, "ff49_rule": FF49_RULE, "min_group": MIN_GROUP}
    if live_rows.any():
        groups.update(used_names_mean=float(info["used"][live_rows].mean()),
                      unknown12_share_mean=float(info["unknown12"][live_rows].mean()),
                      groups12_mean=float(info["groups12"][live_rows].mean()))
        if "P2" in books:
            groups.update(groups49_mean=float(info["groups49"][live_rows].mean()),
                          ff49_own_share_mean=float(info["ff49_own"][live_rows].mean()),
                          ff49_remainder_share_mean=float(info["ff49_remainder"][live_rows].mean()),
                          unknown49_share_mean=float(info["unknown49"][live_rows].mean()))
    groups["size_coverage_of_used_mean"] = size_cover
    return {
        "receipts": {
            "role_manifest_sha256": msha, "role_is_v4_pin": msha == V4_ROLE_SHA,
            "combined_meta_sha256": comb_sha, "combined_is_v4_pin": comb_sha == V4_COMBINED_SHA,
            "combined_composition_weights_sha256": comb_meta.get("composition_weights_sha256"),
            "fields_manifest_sha256": fields_sha, "fields_is_v6_pin": fields_sha == V4_FIELDS_SHA,
            "combined_binds_fields": comb_meta.get("research_fields_manifest_sha256") == fields_sha,
            "fields_role_last_session": fields_role.get("last_session"),
            "fitter_script_sha256": _F.SCRIPT_SHA256,
        },
        "window": {"decisions": t, "first_decision": PM.import_nav_recon().date_of(role.sessions[begin]),
                   "last_decision": PM.import_nav_recon().date_of(role.sessions[end - 1])},
        "context": {"digest": ctx.digest, "columns": int(cols.size), "refused_decisions": len(ctx.refused),
                    "forward_lag2_max_abs_diff": fwd_diff},
        "constants": {"cost_bps": COST_BPS, "partial_fraction": PARTIAL, "min_group": MIN_GROUP,
                      "liquidity_exponent": LIQ_EXPONENT, "lag": LAG, "rcond": RCOND},
        "groups": groups, "books": out_books, "nav": nav_out, "timings": timings,
        "semantics": ("paper books are drift-free target weights on the fitter's price-risk context columns; "
                      "p(d) = w(d) . r[d+2] (valid guarded simple return, else 0), aligned to NAV row d+2; turnover "
                      "sum_i |w_t - w_{t-1}| with the first decision excluded; cost proxy = 20 bps x turnover "
                      "(constant, declared); net-proxy = gross - cost proxy (no financing/borrow). Not modelled: "
                      "the NAV band, fills at d+1 prices, price drift, S2 impact nonlinearity, financing, "
                      "neutralize-guard skips, stale carry."),
    }


def lag_alignment(w: np.ndarray, r_after: np.ndarray, sessions: np.ndarray, begin: int, nav: dict) -> dict:
    """Corr of p_L(d) = w(d) . r[d+L] with the NAV gross at row d+L, L = 1, 2, 3 (r_after[k] = r[begin+1+k])."""
    t = w.shape[0]
    cells = {}
    for lag in (1, 2, 3):
        s = np.full(t, np.nan)
        rows = max(0, min(t, r_after.shape[0] - (lag - 1)))
        if rows:
            s[:rows] = (w[:rows] * r_after[lag - 1:lag - 1 + rows]).sum(axis=1)
        xs, ys = PM.aligned(s, sessions, begin, lag, nav, 0)
        cells[f"lag{lag}"] = {"n": int(xs.size), "corr_gross": PM.corr(xs, ys)}
    cells["best_lag"] = max((k for k in cells if cells[k]["corr_gross"] is not None),
                            key=lambda k: cells[k]["corr_gross"], default=None)
    return cells


def _diff(a, b):
    return None if a is None or b is None else a - b


# ------------------------------------------------------------------------------ report
def f3(v, fmt="{:+.3f}") -> str:
    return "n/a" if v is None else fmt.format(v)


def render_md(doc: dict) -> str:
    r, w, c, g = doc["receipts"], doc["window"], doc["context"], doc["groups"]
    lines = ["# v4 construction study (T26, TRAIN only, book level)", "",
             f"schema {doc['schema']}; updated {doc.get('updated')}; decisions {w['decisions']} "
             f"({w['first_decision']}..{w['last_decision']})", "",
             f"- role {r['role_manifest_sha256'][:12]} (v4 pin {r['role_is_v4_pin']}); combined {r['combined_meta_sha256'][:12]} "
             f"(v4 pin {r['combined_is_v4_pin']}); fields {r['fields_manifest_sha256'][:12]} (v6 pin {r['fields_is_v6_pin']}, "
             f"bound by the combined {r['combined_binds_fields']})",
             f"- context {c['digest'][:12]}: {c['columns']} columns, refused {c['refused_decisions']}, "
             f"forward lag-2 max |diff| {c['forward_lag2_max_abs_diff']:.3g}",
             f"- constants: cost proxy {doc['constants']['cost_bps']} bps x turnover; partial {doc['constants']['partial_fraction']}; "
             f"FF49 min group {doc['constants']['min_group']}; liquidity exponent {doc['constants']['liquidity_exponent']}", ""]
    nav = doc["nav"]
    if nav.get("status") == "read":
        lines.append(f"NAV ({nav['return_rows']} return rows, rule {nav.get('rule')}, band {nav.get('band_multiple')}, "
                     f"fraction {nav.get('trade_fraction')}; binds combined {nav.get('binds_combined')}, role "
                     f"{nav.get('binds_role')}): gross SR {f3(nav['gross']['sharpe'])}, net SR {f3(nav['net']['sharpe'])}")
    else:
        lines.append(f"NAV: {nav.get('status')} ({nav.get('file')})")
    books = doc["books"]
    years = sorted({y for b in books.values() for y in b["by_year_sharpe"]})
    lines += ["", "## Books (gross paper, lag 2)", "",
              "| book | description | gross SR | mean/yr | vol/yr | tau mean | tau p95 | cost/yr | net-proxy SR | corr NAV gross | "
              + " | ".join(f"{y} SR" for y in years) + " | " + " | ".join(f"{y} net" for y in years) + " |",
              "|---|---|---|---|---|---|---|---|---|---|" + "---|" * (2 * len(years))]
    for name, b in books.items():
        corr = (b.get("nav") or {}).get("corr_gross")
        lines.append(f"| {name} | {b['description']} | {f3(b['gross']['sharpe'])} | {f3(b['gross']['mean_ann'], '{:+.4f}')} | "
                     f"{f3(b['gross']['vol_ann'], '{:.4f}')} | {f3(b['tau_mean'], '{:.4f}')} | {f3(b['tau_p95'], '{:.4f}')} | "
                     f"{f3(b['cost_proxy_ann'], '{:.4f}')} | {f3(b['net_proxy']['sharpe'])} | {f3(corr)} | "
                     + " | ".join(f3(b["by_year_sharpe"].get(y)) for y in years) + " | "
                     + " | ".join(f3(b["by_year_net_proxy_sharpe"].get(y)) for y in years) + " |")
    lines += ["", "## Diagnostics", "",
              "| book | live | mean gross | tau / prior gross | FF12 net abs | size expo | abs size expo | gross share < median ADV | "
              "dSR gross vs P0 | dSR net vs P0 | tau / P0 | corr P&L w/ P0 |", "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for name, b in books.items():
        v = b.get("vs_P0", {})
        lines.append(f"| {name} | {b['live_decisions']} | {f3(b['mean_gross'], '{:.3f}')} | {f3(b['tau_per_prior_gross_mean'], '{:.4f}')} | "
                     f"{f3(b['ff12_net_abs_mean'], '{:.4f}')} | {f3(b['size_exposure_mean'], '{:+.4f}')} | "
                     f"{f3(b['size_exposure_abs_mean'], '{:.4f}')} | {f3(b['gross_share_below_median_adv'], '{:.3f}')} | "
                     f"{f3(v.get('d_gross_sharpe'))} | {f3(v.get('d_net_proxy_sharpe'))} | {f3(v.get('tau_ratio'), '{:.3f}')} | "
                     f"{f3(v.get('corr_pnl'))} |")
    if "P4" in books and "tracking_l1_mean" in books["P4"]:
        b = books["P4"]
        lines += ["", f"P4 tracking: mean sum|w - P1 target| {b['tracking_l1_mean']:.4f}; corr of P4 with P1 P&L "
                      f"{f3(b.get('corr_pnl_with_P1'))}"]
    if "P0" in books:
        lines.append(f"P0 tau equals fitter standalone_turnover: {books['P0'].get('tau_equals_fitter_standalone_turnover')}")
        al = books["P0"].get("nav_alignment")
        if al:
            lines.append("P0 vs NAV gross alignment: " + ", ".join(
                f"{k} corr {f3(al[k]['corr_gross'])} (n {al[k]['n']})" for k in ("lag1", "lag2", "lag3"))
                + f"; best {al['best_lag']}")
    lines += ["", "## Groups", "", f"- FF12: {g['ff12_rule']}", f"- FF49: {g['ff49_rule']}"]
    stats = {k: v for k, v in g.items() if k not in ("ff12_rule", "ff49_rule")}
    lines.append("- " + ", ".join(f"{k} {v:.4g}" if isinstance(v, float) else f"{k} {v}" for k, v in stats.items()))
    lines += ["", f"Semantics: {doc['semantics']}", "",
              "Timings (s): " + ", ".join(f"{k} {v:.1f}" for k, v in doc["timings"].items()), ""]
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------------------ CLI
def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--output", type=Path, required=True, help="directory for construct_study.json/.md")
    p.add_argument("--books", default=",".join(BOOKS), help="comma list of P0..P4")
    p.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="base for every default input path")
    p.add_argument("--fitter", type=Path, default=None)
    p.add_argument("--train-role", type=Path, default=None, help="TRAIN role manifest.json")
    p.add_argument("--train-run", type=Path, default=None, help="dir with <prefix>_combined.*")
    p.add_argument("--prefix", default="train")
    p.add_argument("--fields", type=Path, default=None, help="research-role fields dir (manifest.json)")
    p.add_argument("--train-nav", type=Path, default=None, help="NAV dir (daily_<scenario>.csv, recipe.json)")
    p.add_argument("--scenario", default=SCENARIO)
    a = p.parse_args(argv)
    r, be = a.root, a.root / "build-equity"
    defaults = {"fitter": r / "atx-impl/tools/fit_composition_weights.py",
                "train_role": be / "recent-fast-train-2020-2022-v2/manifest.json",
                "train_run": be / "mega-v4w-train-1",
                "fields": be / "recent-fast-train-2020-2022-v2-fields-v6",
                "train_nav": be / "mega-nav-v4-train-b1"}
    for k, v in defaults.items():
        if getattr(a, k) is None:
            setattr(a, k, v)
    a.books = [b.strip().upper() for b in a.books.split(",") if b.strip()]
    return a


def main(argv=None) -> int:
    started = time.perf_counter()
    try:
        a = parse_args(argv)
        need(a.books and all(b in BOOKS for b in a.books), f"--books must be a subset of {BOOKS}")
        bind_fitter(a.fitter)
        NR = PM.import_nav_recon()

        def log(line):
            print(f"[construct] {line}", flush=True)

        doc = {"schema": SCHEMA, "books_requested": a.books}
        doc.update(run_study(a, log))
        doc["timings"]["total"] = time.perf_counter() - started
        doc["peak_rss_mib"] = NR.peak_rss_mb()
        doc["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        doc = PM.clean(doc)
        out = Path(a.output)
        out.mkdir(parents=True, exist_ok=True)
        PM.write_atomic(out / "construct_study.json",
                        (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode())
        PM.write_atomic(out / "construct_study.md", render_md(doc).encode("utf-8"))
    except (StudyError, OSError, KeyError, ValueError) as exc:
        print(f"v4_construct_study: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # the fitter's FitError (TRAIN seal, pins) and anything unexpected: loud
        traceback.print_exc()
        print(f"v4_construct_study: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"[construct] total {time.perf_counter() - started:.1f} s, peak RSS {doc['peak_rss_mib']:.0f} MiB; "
          f"wrote {out / 'construct_study.json'} and construct_study.md", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
