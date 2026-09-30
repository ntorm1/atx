#!/usr/bin/env python3
"""Book diagnostics G-1..G-3 of one NAV cell (platform v8 lane G; code-review-v8-signal S-3, S-5, S-7, S-9, C-5).

DECLARED (v8-prereg rule 8, repeated in the output header): no diagnostic gates, selects or re-weights anything. Every
number here is descriptive; none is an input to an admission, a composition, a fit or a construction.

  book_diagnostics.py run --output OUT/diagnostics-v8.json
        [--cards C] [--marginal-ic K6.json] [--weights W/composition_weights.json]
        [--nav N] [--holdings H] [--u-pass U --role ROLE/manifest.json --role-sha256 SHA] [--work-dir FITWORK]
        [--fields F] [--risk R] [--lagged K=DIR ...] [--only G-1a,G-3a]
  book_diagnostics.py lag-combined --combined X_combined.json --combined-sha256 SHA --lag K --output NEWDIR
  book_diagnostics.py book-csv --holdings H --role ROLE/manifest.json --role-sha256 SHA --output book.csv

``run`` writes one JSON file, schema ``atx.book-diagnostics/v1``: a header (schema, declaration, research window id,
this script's SHA-256) and ``diagnostics``, one key per id, each {inputs, method, result, status ok|skipped, reason
when skipped}. A diagnostic whose inputs are absent is skipped with the reason. A present input that fails its pins,
its schema or the seal refuses the whole run (exit 2, nothing written). The seal is the research window of
atx-engine/tools/research_window.py read through backtest_integrity as nav_summ reads it: the NAV daily CSVs go through
nav_summ.load_daily_csv; the role, holdings, card daily_sleeve.csv, the K6 window and the risk book go through
backtest_integrity.refuse_sealed. Run it from the root the cell ran in (relative paths inside the inputs resolve there).

  id    diagnostic                                                 inputs
  G-1a  ic_theta, f_theta, marginal IC per member                  --cards [--marginal-ic] [--weights]
  G-1b  turnover attribution: each member's and theme's own aim     --u-pass --role --weights [--holdings --nav]
  G-1c  netting ratio: combined turnover / the sleeves' turnover    as G-1b
  G-2a  ex-ante variance split: market, industry, style, specific  --risk (risk --book-weights, --emit-exposures all)
  G-2b  IC by volatility and ADV tercile; IC in the top 1,000      --u-pass --role --weights [--fields] [--cards]
  G-2c  held / ADV and aim / ADV (does R-5's Q = .10 bind)         --holdings --role [--nav]
  G-3a  borrow stress S2-FEE (descriptive, never primary)          --nav --holdings --role --fields
  G-3b  low-risk members' IC before / after price-risk-v1          --u-pass --role --weights [--work-dir]
  G-3c  signal-lag sensitivity, combined signal delayed 1..3       --nav --lagged K=DIR (NAV cells root runs)
  G-3d  cluster map of sleeve PnL correlations vs theme labels     --cards [--weights]

Each diagnostic is one pure function of arrays or parsed documents (``member_horizon``, ``turnover_attribution``,
``netting_ratio``, ``variance_split``, ``ic_by_groups``, ``holding_over_adv``, ``borrow_fee_drag`` + ``restate_net``,
``projection_ic``, ``signal_lag``, ``cluster_map``); the CLI below them only loads inputs. Their docstrings carry the
definitions. Two helpers write inputs root's runs need: ``lag-combined`` a delayed copy of a saved combined signal
(the input of a G-3c NAV cell), ``book-csv`` the risk verb's --book-weights file from a cell's holdings (G-2a).
Numpy only.
"""
from __future__ import annotations

import argparse
import csv
from collections.abc import Mapping
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backtest_integrity as BI  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import horizon_stats  # noqa: E402
from mega_report import analysis as A  # noqa: E402
import nav_summ as NS  # noqa: E402

SCHEMA = "atx.book-diagnostics/v1"
OUTPUT_NAME = "diagnostics-v8.json"
DECLARATION = ("descriptive only (v8-prereg rule 8): no diagnostic gates, selects or re-weights anything; nothing "
               "here is an input to an admission, a composition, a fit or a construction")
DIAGNOSTICS = {
    "G-1a": "ic_theta, f_theta and marginal IC per member: which members contribute at the traded horizon",
    "G-1b": "turnover attribution, the planned turnover of each member's and theme's own aim: do the fast members "
            "carry most of the turnover",
    "G-1c": "netting ratio, combined-score turnover over the sleeves' turnover: how much trades cancel",
    "G-2a": "ex-ante variance split of the book (market, industry, style, specific): can name-level risk sizing matter",
    "G-2b": "IC by volatility tercile and by ADV tercile, IC in the top 1,000 by size: which alpha scaling holds, "
            "where capacity is",
    "G-2c": "held dollars and aim dollars over ADV (p50, p95, max): does R-5 (Q = .10) bind",
    "G-3a": "borrow stress S2-FEE, fee by decile of SI / institutional ownership: how much of net Sharpe is a flat-fee "
            "artefact (descriptive; S2 stays primary)",
    "G-3b": "low-risk members' IC before and after the beta / volatility (price-risk-v1) projection",
    "G-3c": "book net Sharpe with the combined signal delayed 1, 2, 3 sessions: the cost of slower execution",
    "G-3d": "hierarchical clusters of sleeve PnL correlations against the theme labels: are the themes separate bets",
}
FEE_SCHEDULE_BPS = (25.0, 25.0, 25.0, 25.0, 25.0, 25.0, 30.0, 50.0, 150.0, 570.0)  # S2-FEE, decile 1 (low) .. 10
ADV_HOLD_Q = 0.10                 # R-5's declared cap Q (|aim| <= Q ADV)
TOP_SIZE_NAMES = 1000
SIGNAL_LAGS = (1, 2, 3)
PROJECTION_THEME = "low_risk"
FAST_MEMBERS = 4                  # "the four fast members" (review 2.4)
IC_HORIZON = 21                   # the card's orientation horizon (cumulative label)
HAC_LAG = 21                      # overlapping h = 21 labels
GROUP_MIN_NAMES = 30              # the card's within-group minimum
VOL_WINDOW, VOL_MIN_COUNT, ADV_WINDOW = 63, 32, 63
WARMUP_ROWS = 63                  # model books start this many rows before score_begin when the role has them
ANNUAL = 252
DAY_NS = BI.DAY_NS
SIG_DIGITS = 10
NO_EXPOSURE = 255                 # risk industry_slot.u8: no exposure row
RISK_FACTORS, RISK_STYLES, RISK_INDUSTRY_SLOTS = 62, 11, 50  # atx-risk-v1.1: market, 50 industry slots, 11 styles
RISK_STYLE_NAMES = ("size", "beta", "residual_vol", "momentum", "value", "earnings_yield", "profitability",
                    "asset_growth", "leverage", "liquidity", "short_interest")
EXIT_OK, EXIT_REFUSED = 0, 2


class InputError(Exception):
    """A present input that fails its pins, its schema or the seal: the run is refused."""


class Skip(Exception):
    """A diagnostic whose inputs are absent: reported as skipped with the reason."""


def require(cond, msg: str) -> None:
    if not cond:
        raise InputError(msg)


# ================================================================================================= shared arithmetic
def moments(x) -> dict:
    """n, mean, sd (ddof 1), IR = mean / sd (daily) and the Newey-West t (Bartlett, HAC_LAG) of the finite values."""
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    n = int(x.size)
    mean = float(x.mean()) if n else None
    sd = float(x.std(ddof=1)) if n >= 2 else None
    return {"n": n, "mean": mean, "sd": sd, "ir": mean / sd if sd else None,
            "hac_t": fcw.newey_west_t(x, HAC_LAG) if n >= 2 else None}


def member_ranks(signal: np.ndarray, member: np.ndarray) -> np.ndarray:
    """The composition's per-member transform: centred tied rank in [-.5, .5] over the members with a finite signal
    (strategy_ic_composition.cpp each_centered_rank, fcw.centered_tied_ranks), 0 elsewhere."""
    return fcw.centered_tied_ranks(signal, member & np.isfinite(signal))


def desired_target(score: np.ndarray, member: np.ndarray) -> np.ndarray:
    """strategy_target_replay.cpp desired_target per row: centred tied rank of the score over the members, demeaned
    over them, scaled to gross 1; 0 off the members."""
    ranks = fcw.centered_tied_ranks(score, member & np.isfinite(score))
    count = member.sum(axis=1, keepdims=True)
    mean = np.where(member, ranks, 0.0).sum(axis=1, keepdims=True) / np.maximum(count, 1)
    x = np.where(member, ranks - mean, 0.0)
    gross = np.abs(x).sum(axis=1, keepdims=True)
    return np.where(gross > 0, x / np.where(gross > 0, gross, 1.0), 0.0)


def theta_trades(aim: np.ndarray, theta: float) -> np.ndarray:
    """The planned trades of the aim-partial move b(d) = b(d-1) + theta (aim(d) - b(d-1)) from a flat book
    (strategy_target_replay.cpp; every name moves at theta, an aim of 0 decays the position): trade(d) = b(d) - b(d-1)."""
    out = np.empty_like(aim, dtype=np.float64)
    book = np.zeros(aim.shape[1])
    for d in range(aim.shape[0]):
        np.subtract(aim[d], book, out=out[d])
        out[d] *= theta
        book += out[d]
    return out


def sharpe(x) -> float | None:
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    value = NS.sharpe(x) if x.size > 1 else float("nan")
    return value if math.isfinite(value) else None


# ================================================================================================= G-1a
def member_horizon(cards: Mapping[str, dict], book: Mapping[str, dict] | None = None,
                   k6: Mapping[str, dict] | None = None, theta: float = horizon_stats.HORIZON_THETA) -> dict:
    """G-1a: which members contribute at the traded horizon (review S-3; C-5 for the unscored members).

    Per member (the book's members with a weight when ``book`` is given, else every card), oriented by the book's
    sign s (+1 without a book): ic1 = the card's lagged one-day rank IC at h = 1; ic21 = the runner's rank IC at
    h = 21; ic_theta = the card's ``horizon.ic_theta`` or, when the card was built without --ic-theta, horizon_stats
    .ic_theta of its decay curve (sum_h theta (1 - theta)^(h-1) m(h), h = 1..63); retention = ic_theta / (ic1 x the
    truncated weight sum): 1 for a member whose IC does not decay, below 1 when the book holds it after its IC has
    gone; contribution = weight x ic_theta and its share of the summed |contribution|; f_theta / f_theta_hac_t = the admission's
    report-only factor return of the theta-averaged sleeve book (copied into the card when the fitter ran with
    --report-f-theta); marginal_ic21 / marginal_hac_t / max_abs_rho = the K6 row (``k6``, else the card's copy).
    A member with no finite ic_theta is listed as unscored (the runner kept no date for it)."""
    ids = [i for i, m in (book or {}).items() if m.get("weight", 0) > 0] if book else sorted(cards)
    w_sum = float(horizon_stats.theta_weights(horizon_stats.IC_THETA_HORIZONS, theta).sum())
    rows, missing = [], []
    for cid in sorted(ids):
        card = cards.get(cid)
        meta = (book or {}).get(cid, {})
        s = int(meta.get("sign", 1))
        if card is None:
            missing.append(cid)
            continue
        decay = (card.get("decay") or {}).get("ic") or []
        horizon = card.get("horizon") or {}
        ict, source = horizon.get("ic_theta"), "card"
        if "ic_theta" not in horizon:
            ict, source = horizon_stats.ic_theta(decay, horizon_stats.IC_THETA_HORIZONS, theta), "decay curve"
        ic1 = decay[0] if decay and decay[0] is not None else None
        ic21 = (((card.get("runner") or {}).get("horizons") or {}).get(str(IC_HORIZON)) or {}).get("mean")
        adm = card.get("admission") or {}
        k = (k6 or {}).get(cid) or card.get("marginal_ic") or {}
        o = (lambda v: s * v if isinstance(v, (int, float)) else None)
        row = {"id": cid, "theme": meta.get("theme") or card.get("theme"), "weight": meta.get("weight"), "sign": s,
               "ic1": o(ic1), "ic21": o(ic21), "ic_theta": o(ict), "ic_theta_source": source,
               "retention": (s * ict) / (s * ic1 * w_sum) if ict is not None and ic1 else None,
               "f_theta": adm.get("f_theta"), "f_theta_hac_t": adm.get("f_theta_hac_t"),
               "marginal_ic21": o(k.get("marginal_ic21")), "marginal_hac_t": o(k.get("marginal_hac_t")),
               "k6_ic21": o(k.get("ic21")), "max_abs_rho": k.get("max_abs_rho"),
               "max_rho_member": k.get("max_rho_member")}
        row["contribution"] = (row["weight"] * row["ic_theta"]
                               if row["weight"] is not None and row["ic_theta"] is not None else None)
        rows.append(row)
    total = sum(r["contribution"] for r in rows if r["contribution"] is not None)
    gross = sum(abs(r["contribution"]) for r in rows if r["contribution"] is not None)
    for r in rows:
        r["contribution_share_abs"] = (r["contribution"] / gross if r["contribution"] is not None and gross
                                       else None)
    weight = (lambda sel: sum(r["weight"] or 0.0 for r in rows if sel(r)))
    return {"members": rows, "missing_cards": missing, "theta": theta, "ic_theta_weight_sum": w_sum,
            "weighted_ic_theta": total if book else None,
            "negative_at_theta": sorted(r["id"] for r in rows if (r["ic_theta"] or 0) < 0),
            "negative_at_theta_weight": weight(lambda r: (r["ic_theta"] or 0) < 0),
            "negative_marginal_t": sorted(r["id"] for r in rows if (r["marginal_hac_t"] or 0) < 0),
            "unscored": sorted(r["id"] for r in rows if r["ic_theta"] is None),
            "unscored_weight": weight(lambda r: r["ic_theta"] is None)}


# ================================================================================================= G-1b, G-1c
def turnover_attribution(signals: Mapping[str, np.ndarray], members: list[dict], member_mask: np.ndarray,
                         theta: float, leverage: float = 1.0, book_trades: np.ndarray | None = None,
                         score_from: int = 0, fast: list[str] | None = None) -> tuple[dict, dict]:
    """G-1b: the planned turnover of each member's and each theme's own aim, and their share of the book's turnover.

    ``signals`` maps a member id to its raw signal on the decision rows (T x N; read twice, so a lazy mapping streams
    the members); ``members`` = [{id, weight, sign, theme}] with weight > 0; ``member_mask`` the decision members.
      blend       B = sum_k s_k w_k r_k, r_k = the member's centred rank (``member_ranks``): the composition.
      slices      a_k = L s_k w_k r_k / g, g(d) = sum_i |B_i(d)|: the member's exact linear share of the blend's aim
                  at the book's gross L (sum_k a_k = L B / g); a theme's slice is the sum of its members'.
      own aim     each slice moves at theta from a flat book (``theta_trades``); its planned turnover tau_k(d) =
                  sum_i |trade_k,i(d)|, averaged over rows >= ``score_from`` (earlier rows warm the books up).
      share       own share = tau_k / sum_j tau_j over the members (themes); attributed share = sum_d sum_i
                  sign(T_i(d)) trade_k,i(d) / sum_d sum_i |T_i(d)|, T the combined book's planned trade: additive,
                  summing to 1 - remainder; the remainder is what the linear slices do not explain (the final re-rank
                  of B; for the book also neutralisation, dust band, locate and fills).
      combined    the model book: L x desired_target(B) moved at theta; the book: ``book_trades`` (the NAV's planned
                  trades from --emit-holdings: target_weight - held_weight on decision rows), when given.
    ``fast``: the ids reported as the fast members (default: the FAST_MEMBERS members with the highest own-aim
    turnover per unit weight). Returns (result, series): series feeds ``netting_ratio``."""
    ids = [m["id"] for m in members]
    meta = {m["id"]: m for m in members}
    t, n = member_mask.shape
    blend = np.zeros((t, n))
    for cid in ids:
        m = meta[cid]
        blend += (int(m.get("sign", 1)) * float(m["weight"])) * member_ranks(signals[cid], member_mask)
    gross = np.abs(np.where(member_mask, blend, 0.0)).sum(axis=1)
    scale = np.where(gross > 0, leverage / np.where(gross > 0, gross, 1.0), 0.0)[:, None]
    model = theta_trades(leverage * desired_target(blend, member_mask), theta)
    del blend
    rows = np.arange(t) >= score_from
    combined = {"model": model}
    if book_trades is not None:
        combined["book"] = np.where(np.isfinite(book_trades), book_trades, 0.0)
    signs = {k: np.sign(v) for k, v in combined.items()}
    denom = {k: float(np.abs(v[rows]).sum()) for k, v in combined.items()}
    tau_comb = {k: np.abs(v).sum(axis=1) for k, v in combined.items()}
    del model, combined

    def stats(trades: np.ndarray) -> dict:
        tau = np.abs(trades).sum(axis=1)
        return {"tau": tau, "attributed": {k: float((signs[k][rows] * trades[rows]).sum()) / denom[k]
                                           if denom[k] > 0 else None for k in signs}}
    by_member, by_theme = {}, {}
    for theme in sorted({meta[c]["theme"] for c in ids}):
        acc = np.zeros((t, n))
        for cid in (c for c in ids if meta[c]["theme"] == theme):
            m = meta[cid]
            trades = theta_trades(scale * ((int(m.get("sign", 1)) * float(m["weight"])) *
                                           member_ranks(signals[cid], member_mask)), theta)
            by_member[cid] = stats(trades)
            acc += trades
            del trades
        by_theme[theme] = stats(acc)
        del acc

    def table(entries: dict, key_meta) -> dict:
        own = {k: float(v["tau"][rows].mean()) if rows.any() else None for k, v in entries.items()}
        total = sum(v for v in own.values() if v is not None)
        return {k: dict(key_meta(k), own_turnover=own[k], own_share=own[k] / total if total and own[k] is not None
                        else None, attributed_share=entries[k]["attributed"]) for k in entries}
    member_rows = table(by_member, lambda k: {"theme": meta[k]["theme"], "weight": meta[k]["weight"],
                                              "sign": int(meta[k].get("sign", 1))})
    theme_rows = table(by_theme, lambda k: {"weight": sum(meta[c]["weight"] for c in ids if meta[c]["theme"] == k),
                                            "members": [c for c in ids if meta[c]["theme"] == k]})
    speed = {k: (v["own_turnover"] or 0.0) / v["weight"] for k, v in member_rows.items() if v["weight"]}
    fast_ids = list(fast) if fast else sorted(speed, key=lambda k: (-speed[k], k))[:FAST_MEMBERS]
    fast_ids = [k for k in fast_ids if k in member_rows]
    remainder = {k: 1.0 - sum(v["attributed_share"][k] or 0.0 for v in member_rows.values()) for k in signs}
    result = {
        "theta": theta, "leverage": leverage, "rows": int(rows.sum()), "warmup_rows": int(score_from),
        "combined_turnover": {k: float(v[rows].mean()) if rows.any() else None for k, v in tau_comb.items()},
        "remainder": remainder, "members": member_rows, "themes": theme_rows,
        "fast": {"ids": fast_ids, "rule": "given" if fast else f"top {FAST_MEMBERS} by own-aim turnover per unit weight",
                 "weight": sum(member_rows[k]["weight"] for k in fast_ids),
                 "own_share": sum(member_rows[k]["own_share"] or 0.0 for k in fast_ids),
                 "attributed_share": {k: sum(member_rows[i]["attributed_share"][k] or 0.0 for i in fast_ids)
                                      for k in signs}},
        "top_by_own_share": sorted(member_rows, key=lambda k: -(member_rows[k]["own_share"] or 0.0))[:FAST_MEMBERS]}
    series = {"rows": rows, "combined": tau_comb, "themes": {k: v["tau"] for k, v in by_theme.items()},
              "members": {k: v["tau"] for k, v in by_member.items()}}
    return result, series


def netting_ratio(combined: np.ndarray, sleeves: Mapping[str, np.ndarray], rows: np.ndarray | None = None) -> dict:
    """G-1c: combined-score turnover over the weight-averaged sleeve turnover, mean(tau_combined) / sum_k mean(tau_k)
    over ``rows``, each sleeve at its weight in the book (``turnover_attribution`` series). 1: no trade cancels; below
    1: the sleeves' trades net inside the combined score. Also the daily ratio's median, p5 and p95."""
    rows = np.ones(combined.shape[0], dtype=bool) if rows is None else rows
    summed = np.sum([v for v in sleeves.values()], axis=0)
    num, den = float(combined[rows].mean()), float(summed[rows].mean())
    live = rows & (summed > 0)
    daily = combined[live] / summed[live]
    q = (lambda p: float(np.quantile(daily, p)) if daily.size else None)
    return {"ratio": num / den if den > 0 else None, "combined_turnover": num, "sleeve_turnover_sum": den,
            "sleeves": len(sleeves), "daily_ratio": {"median": q(0.5), "p5": q(0.05), "p95": q(0.95)},
            "rows": int(rows.sum())}


# ================================================================================================= G-2a
def variance_split_session(weights: np.ndarray, slot: np.ndarray, styles: np.ndarray, cov: np.ndarray,
                           specific: np.ndarray) -> dict | None:
    """One session's ex-ante daily variance of the book w under atx-risk-v1.1: x = X'w (market = sum w, industry
    slot sums, style z sums), factor variance x'Fx split by Euler contribution c_k = x_k (Fx)_k into market, industry
    and style (per style too); specific = sum w^2 D. Names without an exposure row or a specific forecast are left out
    and their gross reported. None when a factor the book is exposed to has no forecast (the harness's complete-
    forecast rule)."""
    styles = np.asarray(styles, dtype=np.float64)
    covered = ((slot != NO_EXPOSURE) & np.isfinite(specific) & np.isfinite(weights) & (weights != 0) &
               np.isfinite(styles).all(axis=1))
    w = weights[covered]
    x = np.zeros(RISK_FACTORS)
    x[0] = w.sum()
    np.add.at(x, 1 + slot[covered].astype(np.int64), w)
    x[1 + RISK_INDUSTRY_SLOTS:] = styles[covered].T @ w
    exposed = x != 0
    if not bool(np.isfinite(cov[np.ix_(exposed, exposed)]).all()):
        return None
    f = np.where(np.isfinite(cov), cov, 0.0)
    c = x * (f @ x)
    spec = float((w * w * specific[covered]).sum())
    return {"market": float(c[0]), "industry": float(c[1:1 + RISK_INDUSTRY_SLOTS].sum()),
            "style": float(c[1 + RISK_INDUSTRY_SLOTS:].sum()),
            "styles": {name: float(c[1 + RISK_INDUSTRY_SLOTS + k]) for k, name in enumerate(RISK_STYLE_NAMES)},
            "specific": spec, "total": float(c.sum()) + spec, "gross": float(np.abs(weights[np.isfinite(weights)]).sum()),
            "uncovered_gross": float(np.abs(np.where(covered, 0.0, np.nan_to_num(weights))).sum())}


def variance_split(sessions: list[dict], years: list[int] | None = None) -> dict:
    """G-2a over sessions (``variance_split_session`` rows, None = incomplete): the mean share of each block in the
    daily ex-ante variance, the share of the summed variance, the mean ex-ante volatility (annualised) and the
    per-year mean shares. 'specific' small means name-level risk sizing moves little (review S-7)."""
    done = [(s, y) for s, y in zip(sessions, years or [None] * len(sessions)) if s is not None and s["total"] > 0]
    blocks = ("market", "industry", "style", "specific")
    out = {"sessions": len(sessions), "complete_sessions": len(done), "incomplete_sessions": len(sessions) - len(done)}
    if not done:
        return out
    rows = [s for s, _ in done]
    out["mean_share"] = {b: float(np.mean([s[b] / s["total"] for s in rows])) for b in blocks}
    out["mean_share_factor_industry_specific"] = {"factor (market + style)": out["mean_share"]["market"] +
                                                  out["mean_share"]["style"], "industry": out["mean_share"]["industry"],
                                                  "specific": out["mean_share"]["specific"]}
    total = sum(s["total"] for s in rows)
    out["share_of_summed_variance"] = {b: sum(s[b] for s in rows) / total for b in blocks}
    out["style_share_of_summed_variance"] = {k: sum(s["styles"][k] for s in rows) / total for k in RISK_STYLE_NAMES}
    out["mean_ex_ante_vol_annual"] = float(np.mean([math.sqrt(ANNUAL * s["total"]) for s in rows]))
    out["mean_gross"] = float(np.mean([s["gross"] for s in rows]))
    out["mean_uncovered_gross"] = float(np.mean([s["uncovered_gross"] for s in rows]))
    if years:
        out["by_year"] = {str(y): {b: float(np.mean([s[b] / s["total"] for s, yy in done if yy == y])) for b in blocks}
                          for y in sorted({y for _, y in done})}
    return out


# ================================================================================================= G-2b
def terciles(values: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Per row: 0 low, 1 mid, 2 high by tie-averaged rank r over the valid cells (floor(3 r / n), the card's size
    split), -1 elsewhere."""
    valid = valid & np.isfinite(values)
    ranks = fcw.centered_tied_ranks(values, valid)
    n = valid.sum(axis=1, keepdims=True)
    r0 = (ranks + 0.5) * np.maximum(n - 1, 1)
    return np.where(valid, np.clip(np.floor(3.0 * r0 / np.maximum(n, 1)), 0, 2), -1).astype(np.int16)


def top_n(values: np.ndarray, valid: np.ndarray, count: int = TOP_SIZE_NAMES) -> np.ndarray:
    """Per row: 0 for the ``count`` largest valid values (ties at the boundary by tie-averaged rank), 1 for the rest
    of the valid cells, -1 elsewhere."""
    valid = valid & np.isfinite(values)
    ranks = fcw.centered_tied_ranks(values, valid)
    n = valid.sum(axis=1, keepdims=True)
    r0 = (ranks + 0.5) * np.maximum(n - 1, 1)
    return np.where(valid, np.where(r0 >= n - count, 0, 1), -1).astype(np.int16)


def group_ic(signal: np.ndarray, label: np.ndarray, groups: np.ndarray, names: list[str],
             min_names: int = GROUP_MIN_NAMES) -> dict:
    """Per row and group: the rank IC, Pearson of the signal's and the label's centred ranks taken within the group
    over the paired names (finite signal and label); a (row, group) with fewer than ``min_names`` pairs is left out.
    Returns per group the moments of the daily IC and the mean number of names."""
    paired = np.isfinite(signal) & np.isfinite(label)
    out = {}
    for g, name in enumerate(names):
        v = paired & (groups == g)
        x, y = fcw.centered_tied_ranks(signal, v), fcw.centered_tied_ranks(label, v)
        n = v.sum(axis=1)
        sxx, syy = (x * x).sum(axis=1), (y * y).sum(axis=1)
        with np.errstate(invalid="ignore", divide="ignore"):
            ic = np.where((n >= min_names) & (sxx > 0) & (syy > 0), (x * y).sum(axis=1) / np.sqrt(sxx * syy), np.nan)
        m = moments(ic)
        m["mean_names"] = float(n[np.isfinite(ic)].mean()) if np.isfinite(ic).any() else None
        out[name] = m
    return out


def ic_by_groups(signals: Mapping[str, np.ndarray], members: list[dict], label: np.ndarray,
                 groupings: Mapping[str, tuple[np.ndarray, list[str]]]) -> dict:
    """G-2b: each member's oriented rank IC at h = 21 within each grouping (``group_ic``), and the weight-averaged IC
    per group over the members. Flat across volatility terciles fits constant IC (alpha scaling with sigma, the
    Grinold-Kahn case); IC in the large names says where capacity is (review S-5, S-10)."""
    per = {}
    for m in members:
        sig = int(m.get("sign", 1)) * signals[m["id"]]
        per[m["id"]] = {g: group_ic(sig, label, groups, names) for g, (groups, names) in groupings.items()}
    weighted = {}
    for g, (_, names) in groupings.items():
        weighted[g] = {}
        for name in names:
            pairs = [(m["weight"], per[m["id"]][g][name]["mean"]) for m in members
                     if per[m["id"]][g][name]["mean"] is not None]
            wsum = sum(w for w, _ in pairs)
            weighted[g][name] = sum(w * v for w, v in pairs) / wsum if wsum > 0 else None
    return {"members": per, "weighted_mean_ic": weighted, "horizon": IC_HORIZON}


# ================================================================================================= G-2c
def holding_over_adv(dollars: np.ndarray, adv: np.ndarray, q: float = ADV_HOLD_Q) -> dict:
    """|dollars| / ADV over the cells with nonzero dollars: p50, p95, p99, max, the share of cells and of gross
    dollars above ``q`` (R-5's cap), and the cells without a positive ADV (not measured)."""
    dollars, adv = np.abs(np.asarray(dollars, dtype=np.float64)), np.asarray(adv, dtype=np.float64)
    live = np.isfinite(dollars) & (dollars > 0)
    ok = live & np.isfinite(adv) & (adv > 0)
    ratio = dollars[ok] / adv[ok]
    if not ratio.size:
        return {"cells": int(live.sum()), "measured": 0}
    above = ratio > q
    return {"cells": int(live.sum()), "measured": int(ok.sum()), "no_adv": int((live & ~ok).sum()),
            "p50": float(np.quantile(ratio, 0.5)), "p95": float(np.quantile(ratio, 0.95)),
            "p99": float(np.quantile(ratio, 0.99)), "max": float(ratio.max()), "q": q,
            "share_above_q": float(above.mean()), "gross_share_above_q": float(dollars[ok][above].sum() /
                                                                               dollars[ok].sum())}


# ================================================================================================= G-3a
def fee_deciles(short: np.ndarray, ratio: np.ndarray) -> np.ndarray:
    """Per row, the decile (0 lowest .. 9 highest) of ``ratio`` among the short names (short > 0) with a finite ratio:
    floor(10 r / n) of the tie-averaged rank r; -1 for a short with no ratio, -2 off the short book."""
    shorts = np.isfinite(short) & (short > 0)
    valid = shorts & np.isfinite(ratio)
    ranks = fcw.centered_tied_ranks(ratio, valid)
    n = valid.sum(axis=1, keepdims=True)
    r0 = (ranks + 0.5) * np.maximum(n - 1, 1)
    dec = np.clip(np.floor(10.0 * r0 / np.maximum(n, 1)), 0, 9)
    return np.where(valid, dec, np.where(shorts, -1, -2)).astype(np.int16)


def borrow_fee_drag(short: np.ndarray, ratio: np.ndarray, day_fraction: np.ndarray, nav_base: np.ndarray,
                    schedule=FEE_SCHEDULE_BPS) -> dict:
    """G-3a S2-FEE: per row s, the fee drag sum_i fee_i 1e-4 short_i day_fraction(s) / nav_base(s), fee_i =
    schedule[decile of short interest / institutional ownership within the short book] in bps per year on short
    market value (``fee_deciles``); a short with no ratio pays the median fee of the schedule. ``short`` = short
    dollars (> 0) the row's financing is charged on, ``day_fraction`` = calendar days / the day count. Returns the
    drag series and each decile's (and the missing ratio's) share of the short dollars."""
    fees = np.asarray(schedule, dtype=np.float64)
    median = float(np.median(fees))
    dec = fee_deciles(short, ratio)
    fee = np.where(dec >= 0, fees[np.clip(dec, 0, 9)], np.where(dec == -1, median, 0.0))
    dollars = np.where(dec >= -1, short, 0.0)
    with np.errstate(invalid="ignore", divide="ignore"):
        drag = (fee * 1e-4 * dollars).sum(axis=1) * day_fraction / nav_base
    total = float(dollars.sum())
    return {"drag": drag, "median_fee_bps": median,
            "decile_short_share": [float(np.where(dec == k, short, 0.0).sum()) / total if total > 0 else None
                                   for k in range(10)],
            "missing_ratio_short_share": float(np.where(dec == -1, short, 0.0).sum()) / total if total > 0 else None}


def restate_net(net: np.ndarray, charged_fee: np.ndarray, stress_fee: np.ndarray) -> dict:
    """The book's net returns with the charged fee replaced by the stress fee, row by row (first order: same book,
    same NAV path): restated = net + charged - stress. Net Sharpe of both, the difference and the annual drags."""
    ok = np.isfinite(net) & np.isfinite(charged_fee) & np.isfinite(stress_fee)
    restated = net + charged_fee - stress_fee
    base, new = sharpe(net[ok]), sharpe(restated[ok])
    return {"rows": int(ok.sum()), "rows_left_out": int((~ok).sum()), "net_sharpe": base, "restated_net_sharpe": new,
            "delta": new - base if base is not None and new is not None else None,
            "charged_fee_drag_annual": float(ANNUAL * charged_fee[ok].mean()) if ok.any() else None,
            "stress_fee_drag_annual": float(ANNUAL * stress_fee[ok].mean()) if ok.any() else None}


# ================================================================================================= G-3b
def projection_ic(raw: np.ndarray, projected: np.ndarray, support: np.ndarray, label: np.ndarray,
                  min_names: int = GROUP_MIN_NAMES) -> dict:
    """G-3b: rank IC before and after the projection. ``raw`` = the member's book before (its centred ranks over the
    support), ``projected`` = the same book after the price-risk-v1 projection (OLS residual on [1, z beta, z vol,
    z log ADV], the admission's neutralised sleeve), ``support`` = the names both are defined on, ``label`` = the
    h = 21 forward return. Per row: Pearson of each book with the label's centred ranks over the support names with a
    finite label (linear in the residual, as the marginal IC of task F-2); rows with fewer than ``min_names`` names
    are left out. retained = mean after / mean before."""
    paired = support & np.isfinite(label)
    y = fcw.centered_tied_ranks(label, paired)
    n = paired.sum(axis=1)

    def ic(book: np.ndarray) -> np.ndarray:
        x = np.where(paired, book, 0.0)
        x = x - np.where(paired, x.sum(axis=1, keepdims=True) / np.maximum(n, 1)[:, None], 0.0)
        sxx, syy = (x * x).sum(axis=1), (y * y).sum(axis=1)
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where((n >= min_names) & (sxx > 0) & (syy > 0), (x * y).sum(axis=1) / np.sqrt(sxx * syy),
                            np.nan)
    before, after = moments(ic(raw)), moments(ic(projected))
    return {"before": before, "after": after,
            "retained": after["mean"] / before["mean"] if before["mean"] and after["mean"] is not None else None}


# ================================================================================================= G-3c
def signal_lag(base: Mapping[int, float], lagged: Mapping[int, Mapping[int, float]]) -> dict:
    """G-3c: per delay k, the book's net Sharpe with the combined signal delayed k sessions against the base cell on
    their common return sessions: both Sharpe ratios, the difference (the cost of slower execution) and its Memmel
    SE (nav_summ.memmel_se)."""
    out = {}
    for k in sorted(lagged):
        common = sorted(set(base) & set(lagged[k]))
        a = np.array([lagged[k][s] for s in common])
        b = np.array([base[s] for s in common])
        sa, sb = sharpe(a), sharpe(b)
        rho = float(np.corrcoef(a, b)[0, 1]) if len(common) > 2 and a.std() > 0 and b.std() > 0 else None
        out[str(k)] = {"sessions": len(common), "net_sharpe": sa, "base_net_sharpe": sb,
                       "delta": sa - sb if sa is not None and sb is not None else None, "rho": rho,
                       "memmel_se": NS.memmel_se(sa, sb, rho, len(common))
                       if None not in (sa, sb, rho) and common else None}
    return out


# ================================================================================================= G-3d
def average_linkage(dist: np.ndarray) -> list[tuple[int, int, float, int]]:
    """UPGMA on a distance matrix: merges (a, b, height, size) with clusters numbered as in scipy (leaves 0..K-1,
    the i-th merge K + i); ties by the lowest pair of cluster numbers."""
    k = dist.shape[0]
    active = {i: [i] for i in range(k)}
    d = {(i, j): float(dist[i, j]) for i in range(k) for j in range(i + 1, k)}
    merges = []
    nxt = k
    while len(active) > 1:
        (a, b), h = min(d.items(), key=lambda kv: (kv[1], kv[0]))
        members = active.pop(a) + active.pop(b)
        d = {p: v for p, v in d.items() if a not in p and b not in p}
        for c, cm in active.items():
            d[(c, nxt)] = float(np.mean([dist[i, j] for i in members for j in cm]))
        active[nxt] = members
        merges.append((a, b, h, len(members)))
        nxt += 1
    return merges


def cut_clusters(merges: list[tuple[int, int, float, int]], k: int, clusters: int) -> list[int]:
    """Leaf labels (0..clusters-1, by first leaf) after undoing the last clusters - 1 merges."""
    parent = list(range(k + len(merges)))

    def root(i: int) -> int:
        while parent[i] != i:
            i = parent[i]
        return i
    for i, (a, b, _, _) in enumerate(merges[:max(0, len(merges) - (clusters - 1))]):
        parent[a] = parent[b] = k + i
    roots = [root(i) for i in range(k)]
    order = {r: n for n, r in enumerate(dict.fromkeys(roots))}
    return [order[r] for r in roots]


def adjusted_rand(a: list, b: list) -> float | None:
    """Hubert-Arabie adjusted Rand index of two labelings (1 = identical partitions, about 0 = chance)."""
    n = len(a)
    if n < 2:
        return None
    ua, ub = sorted(set(a), key=str), sorted(set(b), key=str)
    table = np.array([[sum(1 for x, y in zip(a, b) if x == i and y == j) for j in ub] for i in ua], dtype=np.float64)
    comb = (lambda x: x * (x - 1) / 2.0)
    s_ij, s_a, s_b = comb(table).sum(), comb(table.sum(axis=1)).sum(), comb(table.sum(axis=0)).sum()
    expected = s_a * s_b / comb(n)
    top = (s_a + s_b) / 2.0 - expected
    return float((s_ij - expected) / top) if top != 0 else None


def effective_bets(corr: np.ndarray) -> float | None:
    """Participation ratio of a correlation matrix's eigenvalues, (sum l)^2 / sum l^2 (negative ones clipped): K for
    K uncorrelated bets, 1 for one bet."""
    if corr.size == 0 or not np.isfinite(corr).all():
        return None
    lam = np.clip(np.linalg.eigvalsh((corr + corr.T) / 2.0), 0.0, None)
    return float(lam.sum() ** 2 / (lam * lam).sum()) if (lam * lam).sum() > 0 else None


def cluster_map(pnl: np.ndarray, names: list[str], themes: list[str], min_days: int = fcw.MIN_COMMON_DAYS) -> dict:
    """G-3d: are the themes separate bets. ``pnl`` = the members' daily sleeve PnL (rows = sessions, NaN = none).
    Pairwise-complete Pearson (>= ``min_days`` common days, else 0), distance sqrt((1 - rho) / 2), average-linkage
    clusters cut at the number of themes and compared with the theme labels (adjusted Rand index, contingency);
    the mean correlation within and between themes; the themes' own PnL (mean of their members') and the effective
    number of bets of their correlation matrix (participation ratio)."""
    rho, _ = A.pairwise_corr(pnl, min_days)
    thin = int((~np.isfinite(rho)).sum() // 2)
    rho = np.where(np.isfinite(rho), rho, 0.0)
    dist = np.sqrt(np.clip((1.0 - rho) / 2.0, 0.0, None))
    np.fill_diagonal(dist, 0.0)
    k = len(names)
    merges = average_linkage(dist) if k > 1 else []
    n_themes = len(set(themes))
    labels = cut_clusters(merges, k, n_themes) if k > 1 else [0] * k
    same = np.array([[ti == tj for tj in themes] for ti in themes])
    off = ~np.eye(k, dtype=bool)
    theme_names = sorted(set(themes))

    def theme_mean(th: str) -> np.ndarray:
        sub = pnl[:, [i for i, t in enumerate(themes) if t == th]]
        count = np.isfinite(sub).sum(axis=1)
        return np.where(count > 0, np.where(np.isfinite(sub), sub, 0.0).sum(axis=1) / np.maximum(count, 1), np.nan)
    theme_pnl = np.column_stack([theme_mean(th) for th in theme_names]) if k else np.zeros((0, 0))
    theme_rho, _ = A.pairwise_corr(theme_pnl, min_days) if k else (np.zeros((0, 0)), None)
    contingency = {}
    for name, th, lab in zip(names, themes, labels):
        contingency.setdefault(str(lab), {}).setdefault(th, []).append(name)
    return {"members": list(names), "themes": list(themes), "clusters_cut": n_themes, "cluster_labels": labels,
            "adjusted_rand_index": adjusted_rand(labels, list(themes)), "contingency": contingency,
            "merges": [{"a": a, "b": b, "height": h, "size": s} for a, b, h, s in merges],
            "mean_rho_within_theme": float(rho[same & off].mean()) if (same & off).any() else None,
            "mean_rho_between_themes": float(rho[~same].mean()) if (~same).any() else None,
            "pairs_below_min_days": thin, "theme_names": theme_names,
            "theme_correlation": [[float(v) if math.isfinite(v) else None for v in row] for row in theme_rho],
            "effective_bets_themes": effective_bets(theme_rho), "effective_bets_members": effective_bets(rho)}


# ================================================================================================= lag-combined
COMBINED_SCHEMA = "atx.dsl-combined-signal/v1"


def lag_combined(manifest: Path, pin: str, lag: int, output: Path) -> dict:
    """A saved combined signal delayed ``lag`` sessions (the input of a G-3c NAV cell): signal'(d) = signal(d - lag) on
    the members at d, 0 (the neutral value) where d < lag or the name had no value at d - lag, NaN off the members
    (the saved-blend contract: a member's signal is finite, a non-member's NaN). Members, sessions and ids are copied;
    the manifest keeps every key, with the two payload receipts, finite_cells, signal_lag_sessions and derived_from
    updated. Writes a new directory; returns the new manifest's path and SHA-256."""
    require(isinstance(lag, int) and lag >= 1, "lag-combined: --lag must be a positive integer")
    data = Path(manifest).read_bytes()
    require(hashlib.sha256(data).hexdigest() == pin, "lag-combined: combined manifest SHA-256 differs from the pin")
    j = json.loads(data)
    require(j.get("schema") == COMBINED_SCHEMA and j.get("status") == "complete" and
            j.get("layout") == "date-major-little-endian", f"lag-combined: not a complete {COMBINED_SCHEMA}")
    d, n, prefix = int(j["dates"]), int(j["instruments"]), f"{j['role']}_combined"
    names = {"signal": f"{prefix}.f64", "member": f"{prefix}_member.u8", "finite": f"{prefix}_finite.u8",
             "sessions": f"{prefix}_sessions.i64", "ids": f"{prefix}_ids.u64"}
    require(sorted(j["files"]) == sorted(names.values()), "lag-combined: the saved file set differs")
    base, raw = Path(manifest).parent, {}
    for key, name in names.items():
        raw[key] = (base / name).read_bytes()
        rec = j["files"][name]
        require(len(raw[key]) == int(rec["bytes"]) and hashlib.sha256(raw[key]).hexdigest() == rec["sha256"],
                f"lag-combined: payload {name} differs from its receipt")
    sessions = np.frombuffer(raw["sessions"], dtype="<i8")
    BI.refuse_sealed(sessions, f"lag-combined: {manifest}")
    signal = np.frombuffer(raw["signal"], dtype="<f8").reshape(d, n)
    member = np.frombuffer(raw["member"], dtype="u1").reshape(d, n).astype(bool)
    shifted = np.zeros((d, n))
    shifted[lag:] = signal[:-lag]
    lagged = np.where(member, np.where(np.isfinite(shifted), shifted, 0.0), np.nan)
    finite = np.isfinite(lagged).astype("u1")
    out = Path(output)
    out.mkdir(parents=True, exist_ok=False)
    payload = {"signal": lagged.astype("<f8").tobytes(), "finite": finite.tobytes(), "member": raw["member"],
               "sessions": raw["sessions"], "ids": raw["ids"]}
    files = {}
    for key, name in names.items():
        (out / name).write_bytes(payload[key])
        files[name] = dict(j["files"][name], bytes=len(payload[key]), sha256=hashlib.sha256(payload[key]).hexdigest())
    doc = dict(j, files=files, finite_cells=int(finite.sum()), signal_lag_sessions=lag,
               derived_from={"path": str(manifest).replace("\\", "/"), "sha256": pin,
                             "rule": "signal(d) = signal(d - lag) on the members at d; 0 where d < lag or no value at "
                                     "d - lag; NaN off the members (book_diagnostics.py lag-combined, G-3c)"})
    text = (json.dumps(doc, indent=2) + "\n").encode("utf-8")
    path = out / f"{prefix}.json"
    path.write_bytes(text)
    return {"manifest": str(path), "manifest_sha256": hashlib.sha256(text).hexdigest(), "lag": lag,
            "member_cells": int(member.sum()), "finite_cells": int(finite.sum())}


# ================================================================================================= input loaders
def sha256_file(path: Path) -> str:
    return BI.sha256_file(Path(path))


def read_json(path: Path, what: str):
    require(Path(path).is_file(), f"{what}: missing {path}")
    try:
        return json.loads(Path(path).read_bytes().decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise InputError(f"{what}: JSON parse: {exc}") from exc


def seal(sessions, what: str) -> None:
    """The research seal (backtest_integrity.refuse_sealed, nav_summ's check): a refusal is an InputError."""
    try:
        BI.refuse_sealed(np.asarray(sessions, dtype=np.int64), what)
    except ValueError as exc:
        raise InputError(str(exc)) from exc


def read_csv_columns(path: Path, columns: Mapping[str, str]) -> dict[str, np.ndarray]:
    """The named columns of a CSV as typed arrays (array codes: 'q' int64, 'Q' uint64, 'd' float64, "nan" allowed),
    parsed row by row into compact buffers: a multi-million-row holdings or book file costs 8 bytes per value."""
    from array import array  # noqa: PLC0415
    dtypes = {"q": np.int64, "Q": np.uint64, "d": np.float64}
    with Path(path).open(newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        head = next(reader, [])
        missing = [c for c in columns if c not in head]
        require(not missing, f"{path}: no column {', '.join(missing)}")
        at = [(head.index(c), array(code), float if code == "d" else int) for c, code in columns.items()]
        for row in reader:
            if row:
                for k, buf, conv in at:
                    buf.append(conv(row[k]))
    return {c: np.frombuffer(buf, dtype=dtypes[code]).copy() for (c, code), (_, buf, _) in zip(columns.items(), at)}


def csv_header(path: Path) -> list[str]:
    with Path(path).open(newline="", encoding="utf-8") as f:
        return next(csv.reader(f), [])


def read_book(path: Path) -> dict:
    """{id: {weight, sign, theme, tier, tau}} of a composition weights file (atx.dsl-composition-weights/v1 or v2)."""
    j = read_json(path, "weights")
    require(j.get("schema") in (fcw.WEIGHTS_SCHEMA, fcw.WEIGHTS_SCHEMA_V2), "weights: schema")
    rows = {r["id"]: r for r in (j.get("provenance") or {}).get("candidates") or [] if isinstance(r, dict)}
    signs = j.get("signs") or {}
    out = {}
    for cid, w in (j.get("weights") or {}).items():
        row = rows.get(cid, {})
        sign = signs.get(cid, row.get("sign", 1))
        out[cid] = {"weight": float(w), "sign": int(1 if sign is None else sign),
                    "theme": row.get("theme") or row.get("family") or "unknown", "tier": row.get("tier"),
                    "tau": row.get("tau")}
    return out


def read_cards(directory: Path) -> tuple[dict, dict, Path | None]:
    """(cards by id, index.json, daily_sleeve.csv path) of an alpha_report_card output; the index window and the
    daily sleeve sessions are checked against the seal."""
    d = Path(directory)
    index = read_json(d / "index.json", "cards index")
    window = index.get("window") or {}
    if isinstance(window.get("last_session_ns"), int):
        seal([window["last_session_ns"]], f"cards {d}")
    cards = {}
    for p in sorted(d.glob("card-*.json")):
        c = read_json(p, "card")
        require(c.get("schema") == "atx.alpha-report-card/v1" and isinstance(c.get("id"), str), f"card: schema {p}")
        cards[c["id"]] = c
    daily = d / "daily_sleeve.csv"
    return cards, index, daily if daily.is_file() else None


def read_sleeve_pnl(path: Path, ids: list[str]) -> np.ndarray:
    """The card's daily_sleeve.csv as a (sessions x ids) PnL matrix (NaN = none), sessions checked against the seal."""
    by = {cid: {} for cid in ids}
    sessions = set()
    with Path(path).open(newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            s = int(r["session_ns"])
            sessions.add(s)
            if r["id"] in by:
                v = float(r["pnl"]) if r["pnl"] not in ("", "nan", "NaN") else math.nan
                by[r["id"]][s] = v
    order = sorted(sessions)
    seal(order, f"cards daily sleeve {path}")
    return np.array([[by[c].get(s, math.nan) for c in ids] for s in order], dtype=np.float64).reshape(len(order),
                                                                                                        len(ids))


def read_k6(path: Path) -> dict:
    """{id: K6 row} of a marginal_ic.json (contract K6), its window checked against the seal."""
    j = read_json(path, "marginal IC")
    rows = j.get("candidates") if isinstance(j, dict) else j
    require(isinstance(rows, list), "marginal IC: expected a list of K6 rows or {candidates: [...]}")
    if isinstance(j, dict) and isinstance((j.get("window") or {}).get("last_decision_session_ns"), int):
        seal([j["window"]["last_decision_session_ns"]], f"marginal IC {path}")
    out = {}
    for r in rows:
        require(isinstance(r, dict) and isinstance(r.get("id"), str) and r["id"] not in out,
                "marginal IC: a row lacks an id or repeats one")
        out[r["id"]] = r
    return out


class Holdings:
    """A --emit-holdings directory (strategy_nav_replay.cpp publish_holdings; strategy_holdings.hpp): atx.nav-holdings/v2
    (holdings.f64 + holdings_index.json, the default) or v1 (holdings.csv). Per reported session: role row, session_ns,
    nav_post, decision flag and a slice of the rows; per row: role index, held dollars, desired and target weight."""

    def __init__(self, directory: Path, role_sessions: np.ndarray, role_ids: np.ndarray):
        d = Path(directory)
        man = read_json(d / "manifest.json", "holdings manifest")
        require(man.get("status") == "complete", "holdings: manifest status")
        files = man.get("files") or {}
        self.path, self.manifest_sha256 = d, sha256_file(d / "manifest.json")
        if man.get("schema") == "atx.nav-holdings/v2":
            self._f64(d, files, role_ids)
        else:
            require(man.get("schema") == "atx.nav-holdings/v1", "holdings: schema")
            self._csv(d, files, role_sessions, role_ids)
        seal(self.session_ns, f"holdings {d}")
        self.row_of = {int(t): k for k, t in enumerate(self.t)}

    def _f64(self, d: Path, files: dict, role_ids: np.ndarray) -> None:
        text = (d / "holdings_index.json").read_bytes()
        require(hashlib.sha256(text).hexdigest() == files.get("holdings_index.json"), "holdings: index SHA-256")
        index = json.loads(text)
        require(index.get("schema") == "atx.nav-holdings-f64/v1", "holdings: index schema")
        data = index["data"]
        require(data["columns"] == ["session", "name", "flags", "held_dollars", "filled_dollars", "fill_cost_dollars",
                                    "unfilled_dollars", "desired_weight", "rule_weight", "target_weight",
                                    "order_dollars"] and data["row_width"] == 11, "holdings: f64 layout")
        raw = (d / "holdings.f64").read_bytes()
        require(len(raw) == int(data["bytes"]) and hashlib.sha256(raw).hexdigest() == data["sha256"] ==
                files.get("holdings.f64"), "holdings: holdings.f64 extent or SHA-256")
        rows = np.frombuffer(raw, dtype="<f8").reshape(-1, 11)
        ids = np.asarray(index["instrument_ids"], dtype=np.uint64)
        require(ids.shape == role_ids.shape and bool(np.all(ids == role_ids)), "holdings: instrument ids differ from the role")
        table = np.asarray(index["sessions"], dtype=np.int64).reshape(-1, 6)
        self.t, self.session_ns = table[:, 0], table[:, 1]
        self.nav_post = table[:, 2].astype(np.uint64).view(np.float64)
        self.decision = table[:, 3].astype(bool)
        self.first, self.count = table[:, 4], table[:, 5]
        self.name = rows[:, 1].astype(np.int64)
        self.held, self.desired, self.target = rows[:, 3].copy(), rows[:, 7].copy(), rows[:, 9].copy()

    def _csv(self, d: Path, files: dict, role_sessions: np.ndarray, role_ids: np.ndarray) -> None:
        require(sha256_file(d / "holdings.csv") == files.get("holdings.csv"), "holdings: holdings.csv SHA-256")
        cols = read_csv_columns(d / "holdings.csv", {"session_ns": "q", "instrument_id": "Q", "held_dollars": "d",
                                                     "desired_weight": "d", "target_weight": "d"})
        ses, ids = cols["session_ns"], cols["instrument_id"]
        self.name = np.searchsorted(role_ids, ids).astype(np.int64)
        require(bool(np.all(role_ids[np.minimum(self.name, len(role_ids) - 1)] == ids)), "holdings: unknown instrument")
        self.held, self.desired, self.target = cols["held_dollars"], cols["desired_weight"], cols["target_weight"]
        days = {}
        with (d / "holdings_days.csv").open(newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                days[int(r["session_ns"])] = (int(r["decision"]), float(r["posttrade_nav"]))
        self.session_ns = np.array(sorted(days), dtype=np.int64)
        self.t = np.searchsorted(role_sessions, self.session_ns).astype(np.int64)
        require(bool(np.all(role_sessions[np.minimum(self.t, len(role_sessions) - 1)] == self.session_ns)),
                "holdings: a session is not on the role axis")
        self.decision = np.array([bool(days[s][0]) for s in self.session_ns])
        self.nav_post = np.array([days[s][1] for s in self.session_ns])
        order = np.argsort(ses, kind="stable")
        require(bool(np.all(order == np.arange(len(ses)))), "holdings: rows not in session order")
        self.first = np.searchsorted(ses, self.session_ns, side="left")
        self.count = np.searchsorted(ses, self.session_ns, side="right") - self.first

    def rows(self, k: int) -> slice:
        return slice(int(self.first[k]), int(self.first[k] + self.count[k]))


class Sources:
    """The named inputs, loaded once, pinned (path + SHA-256 of the primary file) and checked against the seal."""

    def __init__(self, a):
        self.a, self.pins, self._cache = a, {}, {}

    def pin(self, key: str, path: Path, sha: str | None = None) -> None:
        self.pins[key] = {"path": str(path).replace("\\", "/"), "sha256": sha or sha256_file(path)}

    def inputs(self, *keys: str) -> dict:
        return {k: self.pins[k] for k in keys if k in self.pins}

    def need(self, flag: str, value) -> None:
        if value is None:
            raise Skip(f"{flag} not given")

    def cached(self, key: str, build):
        if key not in self._cache:
            self._cache[key] = build()
        return self._cache[key]

    # -- individual inputs
    def book(self) -> dict:
        self.need("--weights", self.a.weights)

        def build():
            self.pin("weights", Path(self.a.weights))
            return read_book(Path(self.a.weights))
        return self.cached("book", build)

    def book_members(self) -> list[dict]:
        return [dict(m, id=k) for k, m in sorted(self.book().items()) if m["weight"] > 0 and m["sign"] != 0]

    def cards(self) -> tuple[dict, dict, Path | None]:
        self.need("--cards", self.a.cards)

        def build():
            self.pin("cards", Path(self.a.cards) / "index.json")
            return read_cards(Path(self.a.cards))
        return self.cached("cards", build)

    def k6(self) -> dict | None:
        if self.a.marginal_ic is None:
            return None

        def build():
            self.pin("marginal_ic", Path(self.a.marginal_ic))
            return read_k6(Path(self.a.marginal_ic))
        return self.cached("k6", build)

    def role(self):
        self.need("--role", self.a.role)

        def build():
            require(self.a.role_sha256 is not None, "--role needs --role-sha256")
            try:
                role = fcw.RoleManifest(Path(self.a.role), self.a.role_sha256)
            except fcw.FitError as exc:
                raise InputError(f"role: {exc}") from exc
            seal(role.sessions, f"role {self.a.role}")
            self.pin("role", Path(self.a.role), self.a.role_sha256)
            return role
        return self.cached("role", build)

    def panel(self) -> dict:
        role = self.role()
        return self.cached("panel", lambda: role.payload())

    def geometry(self):
        """The report card's decision geometry (alpha_report_card.Geometry: eligible names, guard, h = 21 labels)."""
        import alpha_report_card as arc  # noqa: PLC0415  (the card's labels: the IC here is the card's IC)
        return self.cached("geometry", lambda: arc.Geometry(self.role(), self.panel()))

    def member_mask(self) -> np.ndarray:
        """The decision members: member & present & finite positive close (the saved blend's member semantics)."""
        def build():
            p = self.panel()
            with np.errstate(invalid="ignore"):
                return p["member"].astype(bool) & p["present"].astype(bool) & np.isfinite(p["close"]) & (p["close"] > 0)
        return self.cached("member_mask", build)

    def upass(self):
        self.need("--u-pass", self.a.u_pass)
        role = self.role()

        def build():
            u = Path(self.a.u_pass)
            summary_sha, o_sha = sha256_file(u / "summary.json"), sha256_file(u / "orientations.json")
            orient = read_json(u / "orientations.json", "u pass orientations")
            require(orient.get("train_manifest_sha256") in (None, role.sha), "u pass: another role")
            try:
                layout = fcw.CacheLayout(u / "summary.json", summary_sha, role, o_sha, orient.get("recipe_sha256"))
            except fcw.FitError as exc:
                raise InputError(f"u pass: {exc}") from exc
            self.pin("u_pass", u / "summary.json", summary_sha)
            summary = read_json(u / "summary.json", "u pass summary")
            fields = (summary["roles"][0].get("research_fields") or {}).get("directory")
            return {"layout": layout, "cands": {r["id"]: r for r in orient.get("candidates") or []},
                    "fields_dir": fields}
        return self.cached("upass", build)

    def signals(self, rows: slice) -> "LazySignals":
        return LazySignals(self.upass(), self.role(), rows)

    def fields(self, names: tuple[str, ...]) -> dict:
        """{name: dates x names array or None when absent} from --fields (else the u pass's pinned fields dir)."""
        role = self.role()
        directory = self.a.fields or (self.upass()["fields_dir"] if self.a.u_pass else None)
        if directory is None:
            return {name: None for name in names}
        d = Path(directory)
        man = read_json(d / "manifest.json", "fields manifest")
        require(man.get("schema") == "atx.research-role-fields/v1" and
                (man.get("role") or {}).get("manifest_sha256") == role.sha, "fields: schema or another role")
        self.pin("fields", d / "manifest.json")
        out = {}
        for name in names:
            rec = (man.get("files") or {}).get(f"{name}.f64")
            if rec is None:
                out[name] = None
                continue
            key = f"field:{name}"
            if key not in self._cache:
                data = (d / f"{name}.f64").read_bytes()
                require(len(data) == role.dates * role.instruments * 8 and int(rec["bytes"]) == len(data) and
                        hashlib.sha256(data).hexdigest() == rec["sha256"], f"fields: {name}.f64 extent or SHA-256")
                self._cache[key] = np.frombuffer(data, dtype="<f8").reshape(role.dates, role.instruments)
            out[name] = self._cache[key]
        return out

    def holdings(self) -> Holdings:
        self.need("--holdings", self.a.holdings)
        role = self.role()

        def build():
            h = Holdings(Path(self.a.holdings), role.sessions, role.ids)
            self.pin("holdings", Path(self.a.holdings) / "manifest.json", h.manifest_sha256)
            return h
        return self.cached("holdings", build)

    def nav(self) -> dict:
        self.need("--nav", self.a.nav)
        return self.cached("nav", lambda: self._nav(Path(self.a.nav), "nav"))

    def _nav(self, d: Path, key: str) -> dict:
        summary = read_json(d / "summary.json", f"{key} summary")
        require(summary.get("status") == "complete", f"{key}: summary status")
        scen = NS.scenario_of(summary)
        daily_path = d / f"daily_{scen['scenario']}.csv"
        require(daily_path.is_file(), f"{key}: missing {daily_path}")
        daily = NS.load_daily_csv(daily_path)  # nav_summ's seal check (SystemExit) before any column is parsed
        recipe = read_json(d / "recipe.json", f"{key} recipe") if (d / "recipe.json").is_file() else {}
        self.pin(key, d / "summary.json")
        return {"summary": summary, "scenario": scen, "daily": daily, "recipe": recipe, "dir": d}

    def lagged(self) -> dict:
        if not self.a.lagged:
            raise Skip("no --lagged K=DIR cells (NAV cells on a combined signal delayed 1, 2, 3 sessions; see "
                       "task-G-report.md for the argv)")
        out = {}
        for item in self.a.lagged:
            k, _, path = item.partition("=")
            require(k.isdigit() and int(k) > 0 and path, f"--lagged {item!r}: expected K=DIR")
            out[int(k)] = self._nav(Path(path), f"lagged_{k}")
        return out

    def risk(self) -> dict:
        self.need("--risk", self.a.risk)
        return self.cached("risk", lambda: read_risk(Path(self.a.risk)))


class LazySignals(Mapping):
    """Member id -> its raw cached signal on rows ``rows`` (fcw.load_candidate_signal: SHA-verified), read on access."""

    def __init__(self, upass: dict, role, rows: slice):
        self.u, self.role, self.rows = upass, role, rows

    def __getitem__(self, cid: str) -> np.ndarray:
        cand = self.u["cands"].get(cid)
        require(cand is not None, f"u pass: member {cid} is not in the orientations")
        try:
            entry = self.u["layout"].resolve(cand, self.role)
            return np.array(fcw.load_candidate_signal(entry, self.role)[self.rows], dtype=np.float64)
        except fcw.FitError as exc:
            raise InputError(str(exc)) from exc

    def __iter__(self):
        return iter(self.u["cands"])

    def __len__(self) -> int:
        return len(self.u["cands"])


def read_risk(d: Path) -> dict:
    """A risk-model directory written with --book-weights and --emit-exposures all (strategy_risk_verb.cpp): the
    manifest, the role axes, the book (session -> (role columns, weights)) and memory-mapped per-session arrays."""
    man = read_json(d / "manifest.json", "risk manifest")
    require(man.get("schema") == "atx.risk-model/v1" and man.get("status") == "complete", "risk: schema or status")
    geo = man.get("geometry") or {}
    require(geo.get("factors") == RISK_FACTORS and geo.get("styles") == RISK_STYLES, "risk: model geometry")
    book = man.get("book_weights")
    if not book:
        raise Skip("the risk run has no --book-weights")
    files = man.get("files") or {}
    for name in ("style_exposures.f32", "industry_slot.u8"):
        if name not in files:
            raise Skip("the risk run was not made with --emit-exposures all (style_exposures.f32, industry_slot.u8)")
    role_rec = man.get("role") or {}
    try:
        role = fcw.RoleManifest(Path(role_rec["path"]), role_rec["manifest_sha256"])
    except (fcw.FitError, KeyError, OSError) as exc:
        raise InputError(f"risk: role {exc}") from exc
    seal(role.sessions, f"risk role {role_rec.get('path')}")
    dates, names = int(geo["dates"]), int(geo["instruments"])
    require(dates == role.dates and names == role.instruments, "risk: geometry differs from its role")
    arrays = {}
    for name, dtype, shape in (("factor_covariance.f64", "<f8", (dates, RISK_FACTORS, RISK_FACTORS)),
                               ("specific_variance.f64", "<f8", (dates, names)),
                               ("style_exposures.f32", "<f4", (dates, names, RISK_STYLES)),
                               ("industry_slot.u8", "u1", (dates, names))):
        p = d / name
        require(sha256_file(p) == files[name]["sha256"], f"risk: {name} SHA-256")
        arrays[name] = np.memmap(p, dtype=dtype, mode="r", shape=shape)
    bpath = Path(book["path"])
    require(sha256_file(bpath) == book["sha256"], "risk: book weights SHA-256")
    head = csv_header(bpath)  # the risk verb's columns: session|session_ns, instrument_id, weight|held_weight
    s_col, w_col = ("session" if "session" in head else "session_ns"), ("weight" if "weight" in head else "held_weight")
    cols = read_csv_columns(bpath, {s_col: "q", "instrument_id": "Q", w_col: "d"})
    order = np.argsort(cols[s_col], kind="stable")
    ses, ids, w = cols[s_col][order], cols["instrument_id"][order], cols[w_col][order]
    starts = np.flatnonzero(np.r_[True, ses[1:] != ses[:-1]]) if ses.size else np.zeros(0, dtype=np.int64)
    ends = np.r_[starts[1:], ses.size]
    per = {int(ses[a]): (ids[a:b], w[a:b]) for a, b in zip(starts, ends)}
    seal(sorted(per), f"risk book {bpath}")
    return {"manifest": man, "role": role, "arrays": arrays, "book": per, "sha256": sha256_file(d / "manifest.json")}


# ================================================================================================= runners
def ok(inputs: dict, method: str, result) -> dict:
    return {"status": "ok", "inputs": inputs, "method": method, "result": result}


def model_rows(role) -> tuple[slice, int]:
    """Decision rows of the model books: [score_begin - warm-up, score_end), and the first scored row within them."""
    start = max(0, role.score_begin - WARMUP_ROWS)
    return slice(start, role.score_end), role.score_begin - start


def book_trades(src: Sources, role, rows: slice) -> np.ndarray | None:
    """The NAV's planned trades on the model rows (target_weight - held_weight on decision sessions), or None."""
    if src.a.holdings is None:
        return None
    h = src.holdings()
    out = np.zeros((rows.stop - rows.start, role.instruments))
    for k in np.flatnonzero(h.decision):
        t = int(h.t[k]) - rows.start
        if not 0 <= t < out.shape[0]:
            continue
        r = h.rows(k)
        planned = h.target[r] - h.held[r] / h.nav_post[k]
        ok_ = np.isfinite(planned)
        out[t, h.name[r][ok_]] = planned[ok_]
    return out


def construction(src: Sources) -> tuple[float, float, str]:
    """(theta, L, source): the NAV recipe's trade_fraction and aim_leverage when --nav is given."""
    if src.a.nav is not None:
        recipe = src.nav()["recipe"]
        if isinstance(recipe.get("trade_fraction"), (int, float)):
            return float(recipe["trade_fraction"]), float(recipe.get("aim_leverage", 1.0)), "nav recipe"
    return horizon_stats.HORIZON_THETA, 1.0, "horizon_stats.HORIZON_THETA, L 1 (no --nav)"


def run_g1a(src: Sources) -> dict:
    cards, _, _ = src.cards()
    book = src.book() if src.a.weights else None
    res = member_horizon(cards, book, src.k6())
    return ok(src.inputs("cards", "marginal_ic", "weights"), member_horizon.__doc__, res)


def run_g1b(src: Sources, keep: dict) -> dict:
    members = src.book_members()
    role = src.role()
    rows, score_from = model_rows(role)
    theta, lev, basis = construction(src)
    fast = [m["id"] for m in sorted(members, key=lambda m: (-(m["tau"] or 0), m["id"]))
            if m["tau"] is not None][:FAST_MEMBERS] or None
    res, series = turnover_attribution(src.signals(rows), members, src.member_mask()[rows], theta, lev,
                                       book_trades(src, role, rows), score_from, fast)
    res["construction_source"] = basis
    res["fast"]["rule"] = ("the four highest admission tau (weights provenance) among the members" if fast
                           else res["fast"]["rule"])
    keep["g1b"] = series
    return ok(src.inputs("u_pass", "role", "weights", "holdings", "nav"), turnover_attribution.__doc__, res)


def run_g1c(src: Sources, keep: dict) -> dict:
    if "g1b" not in keep:
        raise Skip("G-1b did not run")
    s = keep["g1b"]
    res = {k: netting_ratio(v, s["themes"], s["rows"]) for k, v in s["combined"].items()}
    res["members_model"] = netting_ratio(s["combined"]["model"], s["members"], s["rows"])
    return ok(src.inputs("u_pass", "role", "weights", "holdings", "nav"), netting_ratio.__doc__,
              {"themes_" + k if k in ("model", "book") else k: v for k, v in res.items()})


def run_g2a(src: Sources) -> dict:
    risk = src.risk()
    role, arr = risk["role"], risk["arrays"]
    cov, spec = arr["factor_covariance.f64"], arr["specific_variance.f64"]
    styles, slots = arr["style_exposures.f32"], arr["industry_slot.u8"]
    rows, years = [], []
    for session in sorted(risk["book"]):
        t = int(np.searchsorted(role.sessions, session))
        require(t < role.dates and role.sessions[t] == session, "risk: a book session is not on the role axis")
        ids, w = risk["book"][session]
        cols = np.searchsorted(role.ids, np.asarray(ids, dtype=np.uint64))
        require(bool(np.all(role.ids[np.minimum(cols, role.instruments - 1)] == np.asarray(ids, dtype=np.uint64))),
                "risk: a book instrument is not on the role axis")
        weights = np.zeros(role.instruments)
        np.add.at(weights, cols, np.asarray(w, dtype=np.float64))
        rows.append(variance_split_session(weights, np.asarray(slots[t]), np.asarray(styles[t]),
                                           np.asarray(cov[t]), np.asarray(spec[t])))
        years.append(int(BI.session_date(session).year))
    src.pins["risk"] = {"path": str(Path(src.a.risk) / "manifest.json").replace("\\", "/"), "sha256": risk["sha256"]}
    return ok(src.inputs("risk"), variance_split_session.__doc__ + "\n" + variance_split.__doc__,
              variance_split(rows, years))


def rolling_vol(returns: np.ndarray, window: int = VOL_WINDOW, min_count: int = VOL_MIN_COUNT) -> np.ndarray:
    """Sample SD of the valid daily returns over rows (d - window, d] (NaN below ``min_count``)."""
    ok_ = np.isfinite(returns)
    x = np.where(ok_, returns, 0.0)
    cs = lambda a: np.vstack([np.zeros((1, a.shape[1])), np.cumsum(a, axis=0)])  # noqa: E731
    c1, c2, cn = cs(x), cs(x * x), cs(ok_.astype(np.float64))
    lo = np.maximum(np.arange(returns.shape[0]) + 1 - window, 0)
    hi = np.arange(returns.shape[0]) + 1
    n = cn[hi] - cn[lo]
    s1, s2 = c1[hi] - c1[lo], c2[hi] - c2[lo]
    with np.errstate(invalid="ignore", divide="ignore"):
        var = (s2 - s1 * s1 / np.maximum(n, 1)) / np.maximum(n - 1, 1)
    return np.where(n >= min_count, np.sqrt(np.clip(var, 0.0, None)), np.nan)


def dollar_volume(panel: dict) -> np.ndarray:
    raw, vol = panel["raw_close"], panel["volume"]
    with np.errstate(invalid="ignore"):
        usable = panel["present"].astype(bool) & np.isfinite(raw) & (raw > 0) & np.isfinite(vol) & (vol >= 0)
        return np.where(usable, raw * vol, 0.0)


def rolling_mean(x: np.ndarray, window: int, include_current: bool) -> np.ndarray:
    """Mean of x over rows (d - window, d] (include_current, the card's ADV63) or [d - window, d) (the NAV's execution
    ADV: absent days add 0, divided by window); NaN before a full window."""
    cs = np.vstack([np.zeros((1, x.shape[1])), np.cumsum(x, axis=0)])
    out = np.full(x.shape, np.nan)
    shift = 1 if include_current else 0
    for d in range(window - shift, x.shape[0]):
        out[d] = (cs[d + shift] - cs[d + shift - window]) / window
    return out


def run_g2b(src: Sources) -> dict:
    members = src.book_members()
    role, panel = src.role(), src.panel()
    geo = src.geometry()
    price = fcw.PricePanel(panel["close"], panel["raw_close"], panel["volume"], panel["present"])
    vol = geo.gather(rolling_vol(price.returns), 0)
    adv = geo.gather(rolling_mean(dollar_volume(panel), ADV_WINDOW, True), 0)
    size_field = src.fields(("me_company",))["me_company"]
    size = geo.gather(size_field, 0) if size_field is not None else adv
    with np.errstate(invalid="ignore"):
        groupings = {"volatility": (terciles(vol, geo.colmask & (vol > 0)), ["low", "mid", "high"]),
                     "adv": (terciles(adv, geo.colmask & (adv > 0)), ["low", "mid", "high"]),
                     f"size_top{TOP_SIZE_NAMES}": (top_n(size, geo.colmask & (size > 0)), [f"top{TOP_SIZE_NAMES}",
                                                                                            "rest"]),
                     "all": (np.where(geo.colmask, 0, -1).astype(np.int16), ["all"])}
    sig = LazyGathered(src.signals(slice(0, role.dates)), geo)
    res = ic_by_groups(sig, members, geo.label21, groupings)
    res["size_field"] = "me_company" if size_field is not None else "adv63_dollar_volume (me_company unavailable)"
    res["definitions"] = {"volatility": f"SD of valid daily returns over (d-{VOL_WINDOW}, d], min {VOL_MIN_COUNT}",
                          "adv": f"mean raw_close x volume over (d-{ADV_WINDOW}, d] (the card's ADV63)",
                          "label": "cumulative close[d+22]/close[d+1]-1 with the runner's endpoint rules (card)"}
    if src.a.cards:
        cards, _, _ = src.cards()
        res["card_size_terciles"] = {m["id"]: (cards.get(m["id"]) or {}).get("ic_by_size_tercile") for m in members}
    return ok(src.inputs("u_pass", "role", "weights", "fields", "cards"), ic_by_groups.__doc__ + "\n" +
              group_ic.__doc__, res)


class LazyGathered(Mapping):
    """Signals on the card's compressed decision grid (arc.Geometry.gather), read on access."""

    def __init__(self, signals: LazySignals, geo):
        self.s, self.geo = signals, geo

    def __getitem__(self, cid: str) -> np.ndarray:
        return self.geo.gather(self.s[cid], 0)

    def __iter__(self):
        return iter(self.s)

    def __len__(self) -> int:
        return len(self.s)


def run_g2c(src: Sources) -> dict:
    h, role, panel = src.holdings(), src.role(), src.panel()
    window, lev = ADV_WINDOW, 1.0
    if src.a.nav is not None:
        recipe = src.nav()["recipe"]
        window = int(recipe.get("liquidity_window", ADV_WINDOW))
        lev = float(recipe.get("aim_leverage", 1.0))
    adv = rolling_mean(dollar_volume(panel), window, False)
    held, held_adv, side, aim, aim_adv = [], [], [], [], []
    for k in range(len(h.t)):
        t = int(h.t[k])
        if t < role.score_begin:
            continue
        r = h.rows(k)
        names = h.name[r]
        held.append(h.held[r])
        held_adv.append(adv[t, names])
        side.append(np.sign(h.held[r]))
        if h.decision[k]:
            a = lev * h.nav_post[k] * h.desired[r]
            aim.append(np.where(np.isfinite(a), a, 0.0))
            aim_adv.append(adv[t, names])
    cat = (lambda xs: np.concatenate(xs) if xs else np.zeros(0))
    held, held_adv, side, aim, aim_adv = map(cat, (held, held_adv, side, aim, aim_adv))
    res = {"held": holding_over_adv(held, held_adv), "long": holding_over_adv(held[side > 0], held_adv[side > 0]),
           "short": holding_over_adv(held[side < 0], held_adv[side < 0]),
           "aim": holding_over_adv(aim, aim_adv), "aim_leverage": lev, "liquidity_window": window,
           "adv": "sum over [t-w, t) of present raw_close x volume / w (the NAV's execution ADV)",
           "aim_dollars": "L x nav_post x |desired_weight| on decision sessions (R-5 caps it at Q x ADV)"}
    return ok(src.inputs("holdings", "role", "nav"), holding_over_adv.__doc__, res)


def run_g3a(src: Sources) -> dict:
    nav = src.nav()
    daily, scen = nav["daily"], nav["scenario"]
    spec = ((scen.get("financing") or {}).get("spec")) or {}
    tier_cols = ("short_gc_dollars", "short_warm_dollars", "short_special_dollars")
    if not spec.get("tier_fee_bps") or not all(c in daily for c in tier_cols):
        raise Skip("the NAV cell ran without borrow fields (no tiered financing columns to restate)")
    fields = src.fields(("si_shares", "shares_out", "inst_own_share"))
    absent = [k for k, v in fields.items() if v is None]
    if absent:
        raise Skip(f"fields without {', '.join(absent)}")
    h, role = src.holdings(), src.role()
    basis = float(str(spec.get("day_count", "ACT/360")).split("/")[-1])
    fee = spec["tier_fee_bps"]
    mask = NS.return_mask(daily)
    idx = np.flatnonzero(mask)
    s_idx = daily["session_index"].astype(np.int64)
    ses = daily["session_ns"].astype(np.int64)
    days = (ses[idx] - ses[idx - 1]) / DAY_NS
    frac = days / basis
    base = daily["pretrade_nav"][idx - 1]
    charged = sum(daily[c][idx] * float(fee[c.split("_")[1]]) for c in tier_cols) * 1e-4 * frac / base
    with np.errstate(invalid="ignore", divide="ignore"):
        si, so, io = fields["si_shares"], fields["shares_out"], fields["inst_own_share"]
        ratio_all = np.where((so > 0) & (io > 0) & (si >= 0), (si / so) / io, np.nan)
    short = np.zeros((idx.size, role.instruments))
    ratio = np.full((idx.size, role.instruments), np.nan)
    have = np.zeros(idx.size, dtype=bool)
    for j, row in enumerate(idx):
        prev = int(s_idx[row - 1])
        k = h.row_of.get(prev)
        if k is None:
            continue
        r = h.rows(k)
        neg = h.held[r] < 0
        short[j, h.name[r][neg]] = -h.held[r][neg]
        ratio[j] = ratio_all[prev]
        have[j] = True
    drag = borrow_fee_drag(short, ratio, frac, base)
    stress = np.where(have, drag["drag"], np.nan)
    booked = sum(daily[c][idx] for c in tier_cols)
    held_short = short.sum(axis=1)
    gap = np.abs(held_short - booked)[have & (booked > 0)] / booked[have & (booked > 0)]
    res = {"scenario": "S2-FEE (descriptive, never primary; S2 stays primary)", "s2": scen["scenario"],
           "rule": "restated = S2 net + S2's tier fee (gc / warm / special bps by borrow tier) - the S2-FEE fee "
                   "(schedule by decile of (si_shares / shares_out) / inst_own_share within the short book of t-1, "
                   "fields of t-1); the short spread, long financing, trading costs and the book are S2's",
           "schedule_bps": list(FEE_SCHEDULE_BPS), "median_fee_bps": drag["median_fee_bps"],
           "s2_tier_fee_bps": fee, "day_count": spec.get("day_count"),
           **restate_net(daily["net_return"][idx], charged, stress),
           "decile_short_share": drag["decile_short_share"],
           "missing_ratio_short_share": drag["missing_ratio_short_share"],
           "rows_without_holdings": int((~have).sum()),
           "short_dollar_reconciliation_max_rel_gap": float(gap.max()) if gap.size else None,
           "series": {"session_ns": ses[idx].tolist(), "s2_tier_fee_return": charged.tolist(),
                      "s2_fee_return": stress.tolist()}}
    return ok(src.inputs("nav", "holdings", "role", "fields"), borrow_fee_drag.__doc__ + "\n" + restate_net.__doc__,
              res)


def run_g3b(src: Sources) -> dict:
    members = [m for m in src.book_members() if m["theme"] == src.a.projection_theme]
    if not members:
        raise Skip(f"no book member of theme {src.a.projection_theme!r}")
    role = src.role()
    ctx = None
    if src.a.work_dir:
        ctx = fcw.WorkStore(Path(src.a.work_dir), role).load_context()
    ctx = ctx or fcw.Context.build(role)
    geo = src.geometry()
    full = np.full((geo.t, role.instruments), np.nan)
    full[geo.cell_rows, geo.cell_cols] = geo.label21[geo.colmask]
    label = full[:ctx.end - ctx.begin][:, ctx.columns]
    signals = src.signals(slice(0, role.dates))
    out = {}
    for m in members:
        signal = signals[m["id"]]
        slab = signal[ctx.begin:ctx.end][:, ctx.columns]
        support = ctx.used & np.isfinite(slab)
        s = int(m.get("sign", 1))
        raw = s * fcw.centered_tied_ranks(slab, support)
        projected, live = ctx.book(signal, s)
        out[m["id"]] = dict(projection_ic(raw, projected, support & live[:, None], label), weight=m["weight"])
    res = {"theme": src.a.projection_theme, "members": out,
           "projection": "price-risk-v1: the fitter's Context (OLS residual on [1, z beta252, z vol63, z ladv63], "
                         "two passes), the admission's neutralised sleeve"}
    return ok(src.inputs("u_pass", "role", "weights"), projection_ic.__doc__, res)


def run_g3c(src: Sources) -> dict:
    base = src.nav()
    lagged = src.lagged()
    for k, cell in lagged.items():
        require(cell["scenario"]["scenario"] == base["scenario"]["scenario"],
                f"--lagged {k}: primary scenario differs from the base cell's")
    res = {"delays": signal_lag(NS.net_series(base["daily"]), {k: NS.net_series(v["daily"]) for k, v in lagged.items()}),
           "base_scenario": base["scenario"]["scenario"],
           "combined_sha256": {"base": base["summary"].get("combined_sha256"),
                               **{str(k): v["summary"].get("combined_sha256") for k, v in lagged.items()}}}
    return ok(src.inputs("nav", *[f"lagged_{k}" for k in sorted(lagged)]), signal_lag.__doc__, res)


def run_g3d(src: Sources) -> dict:
    cards, index, daily = src.cards()
    if daily is None:
        raise Skip("the cards have no daily_sleeve.csv")
    if src.a.weights:
        members = src.book_members()
        ids, themes = [m["id"] for m in members], [m["theme"] for m in members]
    else:
        ids = [i for i in index.get("admitted") or [] if i in cards]
        themes = [cards[i].get("theme") or "unknown" for i in ids]
    require(bool(ids), "cluster map: no members")
    pnl = read_sleeve_pnl(daily, ids)
    return ok(src.inputs("cards", "weights"), cluster_map.__doc__, cluster_map(pnl, ids, themes))


RUNNERS = {"G-1a": lambda s, k: run_g1a(s), "G-1b": run_g1b, "G-1c": run_g1c, "G-2a": lambda s, k: run_g2a(s),
           "G-2b": lambda s, k: run_g2b(s), "G-2c": lambda s, k: run_g2c(s), "G-3a": lambda s, k: run_g3a(s),
           "G-3b": lambda s, k: run_g3b(s), "G-3c": lambda s, k: run_g3c(s), "G-3d": lambda s, k: run_g3d(s)}
METHODS = {"G-1a": member_horizon, "G-1b": turnover_attribution, "G-1c": netting_ratio, "G-2a": variance_split,
           "G-2b": ic_by_groups, "G-2c": holding_over_adv, "G-3a": borrow_fee_drag, "G-3b": projection_ic,
           "G-3c": signal_lag, "G-3d": cluster_map}


# ================================================================================================= output
def jsonable(x):
    """A JSON-ready value: floats rounded to SIG_DIGITS significant digits, NaN/inf -> None, numpy -> python."""
    if isinstance(x, dict):
        return {str(k): jsonable(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [jsonable(v) for v in x]
    if isinstance(x, np.ndarray):
        return jsonable(x.tolist())
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, (np.integer,)):
        return int(x)
    if isinstance(x, (float, np.floating)):
        x = float(x)
        return float(f"{x:.{SIG_DIGITS}g}") if math.isfinite(x) else None
    return x


def run(a) -> dict:
    src, keep = Sources(a), {}
    only = set(a.only.split(",")) if a.only else set(DIAGNOSTICS)
    require(only <= set(DIAGNOSTICS), f"--only: unknown {sorted(only - set(DIAGNOSTICS))}")
    out = {}
    for gid in DIAGNOSTICS:
        if gid not in only:
            continue
        try:
            out[gid] = RUNNERS[gid](src, keep)
        except Skip as exc:
            out[gid] = {"status": "skipped", "reason": str(exc), "inputs": {}, "method": METHODS[gid].__doc__,
                        "result": None}
        out[gid]["question"] = DIAGNOSTICS[gid]
    return {"schema": SCHEMA, "declaration": DECLARATION, "window_id": BI.window_id(),
            "tool": {"script": "atx-impl/tools/book_diagnostics.py", "script_sha256": sha256_file(Path(__file__))},
            "diagnostics": jsonable(out)}


def parse_run(argv):
    ap = argparse.ArgumentParser(prog="book_diagnostics.py run", description=__doc__.split("\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--output", required=True, help=f"the JSON file to write (new; e.g. .../{OUTPUT_NAME})")
    ap.add_argument("--cards", default=None, help="alpha_report_card output dir (card-<id>.json, daily_sleeve.csv)")
    ap.add_argument("--marginal-ic", default=None, help="marginal_ic.json (contract K6)")
    ap.add_argument("--weights", default=None, help="composition_weights.json of the book")
    ap.add_argument("--nav", default=None, help="the NAV cell directory (summary.json, recipe.json, daily CSVs)")
    ap.add_argument("--holdings", default=None, help="the cell's --emit-holdings directory (f64 or csv)")
    ap.add_argument("--u-pass", default=None, help="the u pass directory (member signals from its candidate cache)")
    ap.add_argument("--role", default=None, help="the role manifest the cell ran on")
    ap.add_argument("--role-sha256", default=None)
    ap.add_argument("--work-dir", default=None, help="the fitter's --work-dir (reuses its price-risk context)")
    ap.add_argument("--fields", default=None, help="research fields dir (default: the u pass's pinned fields)")
    ap.add_argument("--risk", default=None, help="risk output dir made with --book-weights and --emit-exposures all")
    ap.add_argument("--lagged", action="append", default=[], help="K=DIR: the NAV cell with the signal delayed K")
    ap.add_argument("--projection-theme", default=PROJECTION_THEME)
    ap.add_argument("--only", default=None, help="comma list of diagnostic ids")
    return ap.parse_args(argv)


def book_csv(h: Holdings, role_ids: np.ndarray, output: Path) -> dict:
    """The risk verb's plain book file (strategy_risk_verb.cpp load_book: session_ns, instrument_id, weight) from a
    cell's holdings: one row per held name and reported session, weight = held_dollars / nav_post (the held_weight of
    the v1 holdings.csv: the weight held at the session's close, after its fills). A new file; returns its SHA-256."""
    out = Path(output)
    require(not out.exists(), f"book-csv: output exists (never overwritten): {out}")
    lines, rows = ["session_ns,instrument_id,weight"], 0
    for k in range(len(h.t)):
        r = h.rows(k)
        held = h.held[r] != 0
        for name, weight in zip(h.name[r][held], h.held[r][held] / h.nav_post[k]):
            lines.append(f"{int(h.session_ns[k])},{int(role_ids[name])},{float(weight)!r}")
            rows += 1
    data = ("\n".join(lines) + "\n").encode("utf-8")
    out.write_bytes(data)
    return {"book_weights": str(out), "sha256": hashlib.sha256(data).hexdigest(), "rows": rows,
            "sessions": int(len(h.t))}


def parse_book(argv):
    ap = argparse.ArgumentParser(prog="book_diagnostics.py book-csv", description=book_csv.__doc__)
    ap.add_argument("--holdings", required=True, help="the cell's --emit-holdings directory (f64 or csv)")
    ap.add_argument("--role", required=True)
    ap.add_argument("--role-sha256", required=True)
    ap.add_argument("--output", required=True, help="a new CSV file (risk --book-weights)")
    return ap.parse_args(argv)


def parse_lag(argv):
    ap = argparse.ArgumentParser(prog="book_diagnostics.py lag-combined", description=lag_combined.__doc__)
    ap.add_argument("--combined", required=True, help="the saved combined signal manifest (<role>_combined.json)")
    ap.add_argument("--combined-sha256", required=True)
    ap.add_argument("--lag", type=int, required=True)
    ap.add_argument("--output", required=True, help="a new directory")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    parsers = {"run": parse_run, "lag-combined": parse_lag, "book-csv": parse_book}
    verb = argv[0] if argv and argv[0] in parsers else "run"
    rest = argv[1:] if argv and argv[0] == verb else argv
    a = parsers[verb](rest)
    try:
        if verb == "lag-combined":
            print(json.dumps(lag_combined(Path(a.combined), a.combined_sha256, a.lag, Path(a.output)), indent=2))
            return EXIT_OK
        if verb == "book-csv":
            src = Sources(argparse.Namespace(role=a.role, role_sha256=a.role_sha256, holdings=a.holdings))
            print(json.dumps(book_csv(src.holdings(), src.role().ids, Path(a.output)), indent=2))
            return EXIT_OK
        out = Path(a.output)
        require(not out.exists(), f"output exists (never overwritten): {out}")
        doc = run(a)
    except (InputError, OSError, KeyError, ValueError) as exc:
        print(f"book_diagnostics: refused: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    except SystemExit as exc:  # nav_summ.load_daily_csv's seal refusal
        print(f"book_diagnostics: refused: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes((json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode("utf-8"))
    summary = {gid: d["status"] for gid, d in doc["diagnostics"].items()}
    print(json.dumps({"output": str(out), "diagnostics": summary}))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
