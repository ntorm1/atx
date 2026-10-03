"""Lane YSIG (round 2, the Y screen set): static and semantic checks of the frozen DSL strings. Python only.

Run from the repository root (no arguments; synthetic data only, reads no data payload):
    "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/ysig_check.py

1. Readers: the 52 v8.0 members, the X-2 / X-3 / X-4 members of v8x-prereg.md section 14 (each string's SHA-256
   equal to the pinned one) and the 12 X-7 (XWQ) strings (SHA-256 prefixes of task-XWQ-report.md). Prints the fields
   of fields v14 (v13's 75 + open_adj / high_adj / low_adj) that none of them reads.
2. The Y candidates: frozen string, SHA-256, bars, slots, nodes, bytes and extra fields through lane XSIG's compiler
   mirror (xsig_check.py, calibrated on 16 recorded rows), against the house budget; no candidate equals a reader
   string (bytes or canonical tree). The three carried strings are byte-equal to their LIB2 registration (the
   add-alpha lines of task-LIB2-report.md section 4 up to `--parent`).
3. Semantics (seeded synthetic panels): a numpy interpreter of the house op semantics (oracle.cpp / lit_ops.hpp:
   NaN propagation, full-window rolling ops, group ops over the non-NaN set) evaluates each new string; an
   independent direct implementation of the registered definition must agree in every cell and in the NaN pattern;
   planted errors (mutants) must fail; a causality probe (every input row after t0 moved: rows <= t0 unchanged) must
   pass for every string and fail for a deliberately leaking reference.
4. The frozen add-alpha lines, each with the SHA-256 prefix of the line.
Round 2b (Ruling PM8-6) appends `ins_cluster` to the same tables and checks (ROUND_2B); the round-2 strings and lines
are unchanged.
K1 (`--plan-only` through add-alpha) remains the checker of record; this file states what K1 should print.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xsig_check as xs  # noqa: E402  (lane XSIG's compiler mirror)
import xwq_check as xwq  # noqa: E402  (lane XWQ's 101 table; importing it adds open_adj / high_adj / low_adj)

REPO = xs.REPO
SPEC_V13 = json.loads((REPO / "scripts/specs/v8/lib-v8x4b.json").read_text(encoding="utf-8"))["fields"]
V13 = list(SPEC_V13["list"])
BAR_FIELDS = list(xwq.NEW_FIELDS)
V14 = V13 + BAR_FIELDS
xs.FIELDS |= set(V14) | {"exch_up_365d"}
REG = json.loads((REPO / "atx-impl/strategies/alphas/registry.json").read_text(encoding="utf-8"))
BY_ID = {a["id"]: a for a in REG["alphas"]}

# ------------------------------------------------------------------------------------------------ readers (who reads)
X_LIST = {  # v8x-prereg.md section 14: id -> SHA-256 of the DSL (X-2, X-3, X-4 string rows)
    "q5_eg_f49g": "07a61a9eeedf47ba8743086054475298ff892f5444cf66dd47316e6ab35cbf41",
    "iv_rv_spread_xe": "29e7d9cf564cacf2424ce917718613c3e1787e0c8fdc77fec19c41369054e0ae",
    "ind_adj_rev_5_nx": "9c1d051d4a6ff79a34c4a7057cd48ec64c9d5f6957ea317f5a3f6254e052b46f",
    "ins_opp_buy": "981d01b22ba05cfa768ae1b7a009001d18172dad81a9f2bbeb9a6f3c1f75a68a",
    "bac_vq": "116135c031f95c4655f9e7e6ae7efae71162c622eeb9887697a670e3a1123ecc",
    "stmom": "06dc6238d7e94089391ca1405731ea8ebfe000eb8c9ea45d131fbecf53ab907d",
    "earn_season": "64a0be8f2c71efd1f7a2de920bb5163e99b9dfcef3ea3f4da15f673bc0b328e2",
    "k8_intensity": "0b7d6cdeb6c2f86e328543eb7c996c5d4939fd960631cabce65b4a11e1c7cb05",
    "inst_persist": "ea338c04bdff78ef224eb4244568fa672b051eb2c94020749add9f3ba184ab9c",
    "nt_late": "bafc4e3a93204e59b4a455e7c88335167ed612f01072192526551eea58a7aeed",
    "div_season": "5a1380a1d482c34aca88450b8de9a065150afeaa0c3c8cd359956d4cfd06d0dc",
    "vol_beta": "91583af314a4f9dcec2c6a2c2fc1a507fbfdd39a60068bfe744e8158b6458e94",
    "season_y2_5": "13fe28b662804e2d4daaad505475d65837cd8aaa95dc57650835e3c5a5d818eb",
    "value_composite_v49": "09fb156c56c6de79548fe94d905db524717b8fa561867a79965d0585f56b864f",
    "bm_v49": "a37c3ecabe83b6982017e247a7a802a1546712b078596d68b9d4b0d62473d09d",
    "ep_v49": "0d1975e5a8f11aa523aaf7d7db62f0c6424edb465f6cd78b7149653cd7e976e4",
    "cfp_v49": "86aed3229acc7590722d6eb5172844ebf2e0ba79059dd2e5b2487417893aaf7d",
    "fcfp_v49": "408ba943fe971124d44c2eb4948a4d0dded6965f49c617cb8043fa71ef604a4d",
    "ebit_ev_v49": "4a2b9ba77881892f947213d3929fbf7aa21c57011940f637b5b592c926fb78cb",
    "net_payout_v49": "8b6e8423c1997896f0d2ed88e865b36de2245e53e9d093df85c31f7ef7021f8a",
    "sp_v49": "1153ac469a8e35fc89df89dd7f504203df006c81c8a3647befed0a1a36e9c962",
    "rd_me_v49": "bf89d6a635c81096135fcfe25236067d091f0447189c63846f59ca40d70df07d",
}
XWQ_12 = {  # paper number -> (id, SHA-256 prefix of task-XWQ-report.md sections 5 and 5.8); #33 and #38 withdrawn (PM7-36 c)
    99: ("wq_099", "3ea99660fd87ccf3"), 35: ("wq_035", "cf7571ddb7ae552f"), 55: ("wq_055", "e9b110e3cb7d01f4"),
    6: ("wq_006", "9393d144e5ace0b0"), 2: ("wq_002", "9fdd5972c3390803"), 101: ("wq_101", "88e1e37aa66d1832"),
    95: ("wq_095", "f1f9d043f2889876"), 85: ("wq_085", "d01d6894c982221e"), 30: ("wq_030", "c8c2edf5b89f0f3a"),
    43: ("wq_043", "21026ffb6ddd72c6"), 14: ("wq_014", "5fa625c88df2977e"), 44: ("wq_044", "bc2db9e791d98b42"),
}
LAPSED = ("comp_eq_iss_5y", "coskew_60m", "gscore_lowbm", "nonreliance_402", "earn_consistency")  # R-7, not accepted


def sha(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def readers() -> dict:
    """id -> DSL of every string the Y set must not restate."""
    lib = json.loads((REPO / "atx-impl/strategies/libraries/v80.json").read_text(encoding="utf-8"))
    out = {m: BY_ID[m]["dsl"] for m in lib["members"]}
    assert len(out) == 52, "v8.0 roster"
    for xid, h in X_LIST.items():
        assert sha(BY_ID[xid]["dsl"]) == h, f"{xid}: registry string is not the X list's"
        out[xid] = BY_ID[xid]["dsl"]
    for n, (wid, h16) in XWQ_12.items():
        text = xwq.house_form(xwq.dsl(n))[0]
        assert sha(text)[:16] == h16, f"{wid}: not the XWQ registration"
        out[wid] = text
    return out


# ------------------------------------------------------------------------------------------------ the Y screen set
R21 = "((close / delay(close, 21)) - 1)"
N49 = f"group_count({R21}, grp_ff49)"
CANDIDATES = {  # rank order (prior of marginal contribution to the book's Sharpe and gross return)
    "peer_mom_1m": f"rank(decay_linear((((group_mean({R21}, grp_ff49) * {N49}) - {R21}) / ({N49} - 1)), 21))",
    "so_wang_rev": "rank((ea_window_pre5 * (-1 * group_neutralize(((close / delay(close, 3)) - 1), grp_ff49))))",
    "iv_vol_of_vol": "rank(decay_linear(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + "
                     "(0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 21))",
    "day_rev_freq": "rank(decay_linear(ts_mean_mp((((ret_overnight > 0) && (ret_intraday < 0)) ? 1 : 0), 21, 15), 21))",
    "mom_turn": "rank(decay_linear(((rank(((delay(close, 21) / delay(close, 252)) - 1)) - 0.5) * "
                "(rank(delay(ts_mean((volume / shares_out), 231), 21)) - 0.5)), 21))",
    "ea_uvol": "rank(decay_linear(ts_backfill((log((ts_sum((volume / shares_out), 3) / (3 * delay(ts_mean((volume / "
               "shares_out), 60), 7)))) + (0 * log((1 - abs((ea_days_since - 1)))))), 126), 21))",
    "dato": "group_rank(decay_linear(((((sale_ttm / noa) - (delay(sale_ttm, 252) / noa_lag4)) + (0 * log(noa))) + "
            "(0 * log(noa_lag4))), 21), grp_ff12)",
    "fscore_hbm": "rank(decay_linear(((rank(((be / me_company) + (0 * log(be)))) > 0.8) ? (fscore - 4.5) : 0), 21))",
    "exch_switch": "rank((-1 * exch_up_365d))",
    # Round 2b (Ruling PM8-6; appended after the round-2 set, same rules)
    "ins_cluster": "rank(ins_cluster_buy)",
}
ROUND_2B = ("ins_cluster",)
CARRIED = ("iv_vol_of_vol", "day_rev_freq", "exch_switch")       # LIB2 (R-12) registrations, lapsed at 0 trials
LIB2_SHA16 = {"iv_vol_of_vol": "c5ecec15fbdb4807", "day_rev_freq": "a5416c4ea13d4422", "exch_switch": "f433b32af008e74c"}
EXPECTED = {  # (bars, slots, nodes, extra fields): what K1 should print
    "peer_mom_1m": (41, 6, 15, ["grp_ff49"]),
    "so_wang_rev": (3, 5, 13, ["ea_window_pre5", "grp_ff49"]),
    "iv_vol_of_vol": (40, 5, 13, ["iv_atm_21d"]),
    "day_rev_freq": (40, 4, 12, ["ret_intraday", "ret_overnight"]),
    "mom_turn": (272, 6, 22, ["shares_out"]),
    "ea_uvol": (211, 5, 26, ["ea_days_since", "shares_out"]),
    "dato": (272, 5, 19, ["grp_ff12", "noa", "noa_lag4", "sale_ttm"]),
    "fscore_hbm": (20, 5, 17, ["be", "fscore", "me_company"]),
    "exch_switch": (0, 3, 4, ["exch_up_365d"]),
    "ins_cluster": (0, 2, 2, ["ins_cluster_buy"]),
}

# ------------------------------------------------------------------------------------------------ registration rows
PY_ARGS_TAIL = "--parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>"
REGISTRATION = {
    "peer_mom_1m": {
        "theme": "price_momentum", "tier": "B-",
        "citation": "Moskowitz and Grinblatt (1999, JF 54(4)) Do industries explain momentum?",
        "source": "Moskowitz-Grinblatt 1999", "form": "R(decay_linear(x, 21))",
        "formula": "equal-weighted mean 21-session return of the stock's FF49 industry peers with the stock itself "
                   "excluded: (n x group_mean(r21) - r21) / (n - 1), n = the industry's names with a finite r21; "
                   "industries that won over the last month continue (industry momentum is strongest at one month)",
        "domain": "NaN where the stock's own 21-session return or its FF49 label is NaN, and for a stock alone in its "
                  "industry (n = 1: 0 / 0)",
        "deviation": "equal-weighted peers, not the paper's value-weighted industry portfolios (the value-weighted "
                     "leave-one-out string needs 8 slots in the compiler mirror, over the house 7; equal weights as "
                     "ind_mom_12_1); the stock is left out of its own industry mean (its own one-month return, which "
                     "reverses, is not in the signal); 49 Fama-French industries, not the paper's 20; rolling 21 "
                     "sessions with the house decay, not monthly portfolios; a sibling share class of the same "
                     "company counts as a peer"},
    "so_wang_rev": {
        "theme": "reversal_seasonality", "tier": "B-",
        "citation": "So and Wang (2014, JFE 114(1)) News-driven return reversals: liquidity provision ahead of earnings "
                    "announcements",
        "source": "So-Wang 2014", "form": "R(x)",
        "formula": "-(3-session return, FF49 industry-demeaned) while the expected earnings announcement is 1..5 "
                   "sessions ahead (ea_window_pre5 = 1), else 0; short-term reversals are about six times larger ahead "
                   "of scheduled earnings news (market makers price the inventory risk of holding through it)",
        "domain": "NaN where the earnings calendar is NaN (no visible primary 8-K 2.02 within 200 days, or overdue by "
                  "more than 63 sessions) or the 3-session return is NaN; 0 outside the window (most names tie at the "
                  "middle rank)",
        "deviation": "rolling 3-session return ending at the decision session while the expected date is 1..5 "
                     "sessions ahead (paper: the return over days a-4..a-2, held a-1..a+1 around the realised date); "
                     "expected date from the house yoy_364 calendar, not the realised date; FF49-demeaned as the house "
                     "reversal members (paper: raw returns); no decay (event-window form); the house fills at t+1"},
    "mom_turn": {
        "theme": "price_momentum", "tier": "C+",
        "citation": "Lee and Swaminathan (2000, JF 55(5)) Price momentum and trading volume",
        "source": "Lee-Swaminathan 2000", "form": "R(decay_linear(x, 21))",
        "formula": "(rank(12-1 return) - 0.5) x (rank(mean daily turnover, volume / shares_out, over the same sessions "
                   "t-251..t-21) - 0.5): the momentum x turnover interaction alone; momentum is larger among "
                   "high-turnover stocks",
        "domain": "NaN where the 12-1 return or any of the 231 daily turnovers is NaN",
        "deviation": "the interaction term only, with centred cross-sectional ranks (no momentum main effect, which "
                     "mom_12_1 carries, and no turnover main effect); continuous, not the paper's 10 x 3 portfolios; "
                     "the house 12-1 return with turnover over the same 231 sessions; daily turnover = raw share "
                     "volume / vendor shares_out (A8 90-day lag, split-restated); house decay"},
    "ea_uvol": {
        "theme": "earnings_momentum", "tier": "C+",
        "citation": "Garfinkel and Sokobin (2006, JAR 44(1)) Volume, opinion divergence, and returns: a study of "
                    "post-earnings announcement drift",
        "source": "Garfinkel-Sokobin 2006", "form": "R(decay_linear(x, 21))",
        "formula": "log(turnover over sessions a-1..a+1 around the reaction session a of the latest earnings "
                   "announcement / (3 x mean daily turnover over sessions a-65..a-6)), set at t = a+1 and held "
                   "(ts_backfill 126) until the next announcement; post-announcement returns rise with abnormal "
                   "announcement volume (opinion divergence)",
        "domain": "NaN without an announcement reaction session in the last 126 sessions with finite turnover windows "
                  "(turnover = volume / shares_out; ea_days_since NaN for non-filers of 8-K 2.02)",
        "deviation": "abnormal volume = log ratio to the stock's own pre-announcement turnover (paper: volume "
                     "unexplained by a regression on prior trading activity); 3-session window a-1..a+1; carried to "
                     "the next announcement with the house ear idiom (ts_backfill 126) and decay 21; unsigned (not "
                     "conditioned on the earnings news)"},
    "dato": {
        "theme": "profitability_quality", "tier": "C+",
        "citation": "Soliman (2008, The Accounting Review 83(3)) The use of DuPont analysis by market participants",
        "source": "Soliman 2008", "form": "R(decay_linear(x, 21))",
        "formula": "change in asset turnover: sale_ttm / noa - (sale_ttm 252 sessions earlier) / noa_lag4, ranked "
                   "within FF12; a rise in asset turnover predicts higher returns",
        "domain": "NaN where net operating assets now or four quarters earlier are not positive (0 x log guard) or "
                  "an input is NaN",
        "deviation": "NOA at the period end, not average NOA; prior-year sales = sale_ttm 252 sessions earlier (the "
                     "filing clock can read a TTM one quarter off for a few sessions around a filing; noa_lag4 is the "
                     "exact four-quarter lag); continuous rank within FF12 (house R1), not deciles; house decay"},
    "fscore_hbm": {
        "theme": "profitability_quality", "tier": "C+",
        "citation": "Piotroski (2000, JAR 38 supplement) Value investing: the use of historical financial statement "
                    "information to separate winners from losers",
        "source": "Piotroski 2000", "form": "R(decay_linear(x, 21))",
        "formula": "fscore - 4.5 for the top book-to-market quintile (cross-sectional rank of be / me_company above "
                   "0.8), 0 elsewhere; inside value stocks high F-score firms beat low F-score firms",
        "domain": "NaN where book equity is not positive or be / me_company is NaN; for value names NaN where fscore "
                  "is NaN",
        "deviation": "continuous F-score centred at 4.5 (the midpoint of 0..9), not the paper's high (8-9) minus low "
                     "(0-1) portfolios; market-wide quintile on the role universe each session, not annual sorts; "
                     "filing-clock items; house decay; the unconditional fscore member stays (this adds weight inside "
                     "value names only)"},
    "ins_cluster": {
        "theme": "ownership_flow", "tier": "C+",
        "citation": "Alldredge and Blank (2019, Journal of Financial Research 42(2)) Do insiders cluster trades with "
                    "colleagues? Evidence from daily insider trading",
        "source": "Alldredge-Blank 2019", "form": "R(x)",
        "formula": "ins_cluster_buy: 1 when at least 3 distinct insiders (directors or officers) made open-market "
                   "purchases with transaction dates in the last 21 sessions, visible at t, else 0; clustered insider "
                   "purchases are followed by higher abnormal returns over the next month than solitary ones",
        "domain": "NaN without a visible insider transaction row of the issuer within 365 days (Section 16 presence; "
                  "foreign private issuers NaN); a binary flag: flagged names tie at the top rank, the rest tie below",
        "deviation": "cluster = at least 3 distinct buyers within 21 sessions (field sec-ins-cluster-buy21-min3-v1), "
                     "not the paper's purchase within two days of a peer insider's purchase; directors and officers "
                     "on original Form 4s (10% owners and joint 10%-owner filings excluded); continuous rank of a flag, "
                     "not an event-time portfolio; no decay (the flag lives 21 sessions from the trade date)"},
}


def lib2_lines() -> dict:
    """The three LIB2 add-alpha lines (task-LIB2-report.md section 4), cut at ' --parent '."""
    text = (HERE / "task-LIB2-report.md").read_text(encoding="utf-8")
    out = {}
    for line in text.splitlines():
        for cid in CARRIED:
            if line.startswith(f'"$PY" scripts/research_cycle.py add-alpha --id {cid} '):
                out[cid] = line.split(" --parent ")[0]
    assert set(out) == set(CARRIED), "LIB2 lines not found"
    return out


def add_alpha_line(cid: str) -> str:
    if cid in CARRIED:
        return f"{lib2_lines()[cid]} {PY_ARGS_TAIL}"
    r = REGISTRATION[cid]
    return (f'"$PY" scripts/research_cycle.py add-alpha --id {cid} --dsl "{CANDIDATES[cid]}" --theme {r["theme"]} '
            f'--tier {r["tier"]} --prior-sign 1 --citation "{r["citation"]}" --origin prior '
            f'--prior-sign-source "{r["source"]}" --form "{r["form"]}" --formula "{r["formula"]}" '
            f'--domain "{r["domain"]}" --deviation "{r["deviation"]}" {PY_ARGS_TAIL}')


# ------------------------------------------------------------------------------------------------ interpreter (house)
# oracle.cpp / lit_ops.hpp: NaN propagates; compare and && are NaN on a NaN operand; Select on a NaN mask is NaN;
# cross-sectional ops over the non-NaN cells of a row (a NaN group label -> NaN); rolling ops need a full window with
# no NaN (sum / mean also refuse +-inf); decay_linear weights 1..d, newest heaviest; ts_backfill = the newest non-NaN
# cell of the last d rows; rank = average-tie percentile (n - 1 denominator, a lone cell 0.5).
def _rank_vals(v):
    n = len(v)
    if n == 1:
        return np.array([0.5])
    less = (v[None, :] < v[:, None]).sum(axis=1)
    eq = (v[None, :] == v[:, None]).sum(axis=1)
    return (less + (eq - 1) / 2.0) / (n - 1)


def _cs(x, g, fn):
    out = np.full(x.shape, np.nan)
    for t in range(x.shape[0]):
        ok = ~np.isnan(x[t])
        if g is None:
            if ok.any():
                out[t, ok] = fn(x[t, ok])
            continue
        lab = g[t]
        for k in np.unique(lab[ok & ~np.isnan(lab)]):
            m = ok & (lab == k)
            out[t, m] = fn(x[t, m])
    return out


def _roll(x, d, fn, finite):
    out = np.full(x.shape, np.nan)
    for t in range(d - 1, x.shape[0]):
        w = x[t - d + 1:t + 1]
        bad = np.isnan(w).any(axis=0)
        if finite:
            bad |= ~np.isfinite(w).all(axis=0)
        with np.errstate(invalid="ignore", over="ignore"):
            out[t] = np.where(bad, np.nan, fn(w))
    return out


def _decay(w):
    acc = np.zeros(w.shape[1:])
    for i in range(w.shape[0]):
        acc = acc + (i + 1) * w[i]
    return acc / (w.shape[0] * (w.shape[0] + 1) / 2.0)


def _backfill(x, d):
    out = np.full(x.shape, np.nan)
    for t in range(x.shape[0]):
        for i in range(min(d, t + 1)):
            row = x[t - i]
            take = np.isnan(out[t]) & ~np.isnan(row)
            out[t, take] = row[take]
    return out


def _nan2(a, b, v):
    return np.where(np.isnan(a) | np.isnan(b), np.nan, v)


def ev(n, env):
    k = n["key"]
    if k[0] == "num":
        return np.float64(k[1])
    if k[0] == "field":
        return env[k[1]]
    a = [ev(c, env) for c in n["kids"]]
    with np.errstate(invalid="ignore", divide="ignore", over="ignore"):
        if k[0] == "bin":
            return {"+": np.add, "-": np.subtract, "*": np.multiply, "/": np.divide}[k[1]](a[0], a[1])
        if k[0] == "cmp":
            op = {"<": np.less, ">": np.greater, "<=": np.less_equal, ">=": np.greater_equal, "==": np.equal,
                  "!=": np.not_equal}[k[1]]
            return _nan2(a[0], a[1], op(a[0], a[1]).astype(float))
        if k[0] == "logic":
            x, y = ((np.asarray(v) != 0) for v in (a[0], a[1]))
            return _nan2(a[0], a[1], (x & y if k[1] == "&&" else x | y).astype(float))
        if k[0] == "sel":
            c = np.broadcast_to(a[0], np.broadcast(a[0], a[1], a[2]).shape)
            return np.where(np.isnan(c), np.nan, np.where(c != 0, a[1], a[2]))
        name = k[1]
        if name == "rank":
            return _cs(a[0], None, _rank_vals)
        if name == "group_rank":
            return _cs(a[0], a[1], _rank_vals)
        if name == "group_neutralize":
            return _cs(a[0], a[1], lambda v: v - v.sum() / len(v))
        if name == "group_mean":
            return _cs(a[0], a[1], lambda v: np.full(len(v), v.sum() / len(v)))
        if name == "group_count":
            return _cs(a[0], a[1], lambda v: np.full(len(v), float(len(v))))
        if name == "delay":
            d, x = int(a[1]), a[0]
            out = np.full(x.shape, np.nan)
            out[d:] = x[:-d]
            return out
        if name == "ts_sum":
            return _roll(a[0], int(a[1]), lambda w: w.sum(axis=0), True)
        if name == "ts_mean":
            return _roll(a[0], int(a[1]), lambda w: w.sum(axis=0) / w.shape[0], True)
        if name == "decay_linear":
            return _roll(a[0], int(a[1]), _decay, False)
        if name == "ts_backfill":
            return _backfill(a[0], int(a[1]))
        if name in ("log", "abs", "sign"):
            return {"log": np.log, "abs": np.abs, "sign": np.sign}[name](a[0])
        if name in ("min", "max"):
            return _nan2(a[0], a[1], (np.minimum if name == "min" else np.maximum)(a[0], a[1]))
    raise NotImplementedError(k)


def run(text, env):
    return np.broadcast_to(ev(xs.parse(text), env), env["close"].shape).astype(float)


# ------------------------------------------------------------------------------------------------ synthetic world
def world(seed=20261002, T=330, N=48):
    rng = np.random.default_rng(seed)
    close = 30.0 * np.exp(np.cumsum(rng.normal(0.0, 0.02, (T, N)), axis=0))
    close[rng.random((T, N)) < 0.004] = np.nan
    shares = np.exp(rng.uniform(np.log(2e7), np.log(5e8), N))
    so = np.repeat(shares[None, :], T, axis=0) * np.where(np.arange(T)[:, None] > 150, rng.choice([1.0, 2.0], N), 1.0)
    so[rng.random((T, N)) < 0.002] = np.nan
    vol = so * np.exp(rng.normal(np.log(0.006), 0.6, (T, N)))
    g49 = np.repeat(rng.integers(1, 8, N).astype(float)[None, :], T, axis=0)
    g49[:, 0] = 99.0                                  # an industry of one
    g49[200:, 1] = np.nan                             # a lost label
    g12 = np.repeat(rng.integers(1, 4, N).astype(float)[None, :], T, axis=0)
    cyc = rng.integers(0, 63, N)
    eds = ((np.arange(T)[:, None] + cyc[None, :]) % 63).astype(float)       # sessions since the latest announcement
    eds[:, :3] = np.nan                                                      # non-filers
    pre5 = np.where(np.isnan(eds), np.nan, ((63 - eds >= 1) & (63 - eds <= 5)).astype(float))
    q = np.repeat(np.arange(T)[:, None] // 63, N, axis=1)
    lvl = np.exp(rng.normal(20.0, 0.5, N))
    grow = rng.normal(0.02, 0.05, (T // 63 + 2, N))
    sale = lvl * np.exp(np.take_along_axis(np.cumsum(grow, axis=0), q, axis=0))
    noa = sale * np.exp(rng.normal(-0.2, 0.4, (T // 63 + 2, N)))[q[:, 0]]
    noa[:, 4:7] *= -1.0                                                      # non-positive NOA
    noa_l4 = np.vstack([np.full((252, N), np.nan), noa[:-252]]) if T > 252 else np.full((T, N), np.nan)
    noa_l4[:252] = noa[:252] * np.exp(rng.normal(0.0, 0.2, (252, N)))
    be = sale * np.exp(rng.normal(-0.5, 0.6, N))
    be[:, 7:9] *= -1.0                                                       # negative book equity
    fs = np.repeat(rng.integers(0, 10, (T // 63 + 2, N)).astype(float), 63, axis=0)[:T]
    fs[rng.random((T, N)) < 0.02] = np.nan
    buys = rng.random((T, N, 6)) < np.where(np.arange(N) % 5 == 0, 0.08, 0.01)[None, :, None]  # 6 insiders a name
    present = np.ones((T, N))
    present[:, 9:11] = np.nan                                                # no Section 16 presence (NaN)
    ever = lambda w: np.array([buys[max(0, t - w):t].any(axis=0).sum(axis=1) for t in range(T)], dtype=float)
    return {"close": close, "volume": vol, "shares_out": so, "me_company": close * shares, "grp_ff49": g49,
            "grp_ff12": g12, "ea_days_since": eds, "ea_window_pre5": pre5, "sale_ttm": sale, "noa": noa,
            "noa_lag4": noa_l4, "be": be, "fscore": fs, "buys": buys,
            "present": present, "ins_cluster_buy": present * (ever(21) >= 3).astype(float),
            "ins_n_buyers": present * ever(126)}


# ------------------------------------------------------------------------------------------------ direct definitions
# Written independently of the interpreter (loops over names and groups, lane XSIG's rank and decay references).
def _ref_shift(x, k):
    out = np.full(x.shape, np.nan)
    out[k:] = x[:-k]
    return out


def ref_rank(x):
    return xs.cs_rank(np.where(np.isnan(x), np.nan, x))


def ref_decay(x, w):
    out = np.full(x.shape, np.nan)
    wts = np.arange(1, w + 1, dtype=float)
    for t in range(w - 1, x.shape[0]):
        blk = x[t - w + 1:t + 1]
        ok = ~np.isnan(blk).any(axis=0)
        out[t, ok] = (wts[:, None] * blk[:, ok]).sum(axis=0) / wts.sum()
    return out


def ref_group_rank(x, g):
    out = np.full(x.shape, np.nan)
    for t in range(x.shape[0]):
        for i in range(x.shape[1]):
            if np.isnan(x[t, i]) or np.isnan(g[t, i]):
                continue
            mem = [j for j in range(x.shape[1]) if not np.isnan(x[t, j]) and g[t, j] == g[t, i]]
            v = np.array([x[t, j] for j in mem])
            out[t, i] = xs.avg_rank_pct(v)[mem.index(i)]
    return out


def def_peer_mom_1m(e):
    r = e["close"] / _ref_shift(e["close"], 21) - 1.0
    v = np.full(r.shape, np.nan)
    for t in range(r.shape[0]):
        for i in range(r.shape[1]):
            if np.isnan(r[t, i]) or np.isnan(e["grp_ff49"][t, i]):
                continue
            peers = [r[t, j] for j in range(r.shape[1]) if j != i and not np.isnan(r[t, j])
                     and e["grp_ff49"][t, j] == e["grp_ff49"][t, i]]
            if peers:
                v[t, i] = float(np.mean(peers))
    return ref_rank(ref_decay(v, 21))


def def_so_wang_rev(e):
    r = e["close"] / _ref_shift(e["close"], 3) - 1.0
    g = e["grp_ff49"]
    v = np.full(r.shape, np.nan)
    for t in range(r.shape[0]):
        for i in range(r.shape[1]):
            if np.isnan(r[t, i]) or np.isnan(g[t, i]) or np.isnan(e["ea_window_pre5"][t, i]):
                continue
            mem = [r[t, j] for j in range(r.shape[1]) if not np.isnan(r[t, j]) and g[t, j] == g[t, i]]
            v[t, i] = e["ea_window_pre5"][t, i] * -(r[t, i] - float(np.mean(mem)))
    return ref_rank(v)


def def_mom_turn(e):
    c = e["close"]
    m = _ref_shift(c, 21) / _ref_shift(c, 252) - 1.0
    tv = e["volume"] / e["shares_out"]
    turn = np.full(c.shape, np.nan)
    for t in range(251, c.shape[0]):
        blk = tv[t - 251:t - 20]                    # sessions t-251..t-21 (231)
        ok = np.isfinite(blk).all(axis=0)
        turn[t, ok] = blk[:, ok].mean(axis=0)
    return ref_rank(ref_decay((ref_rank(m) - 0.5) * (ref_rank(turn) - 0.5), 21))


def def_ea_uvol(e):
    tv = e["volume"] / e["shares_out"]
    eds = e["ea_days_since"]
    u = np.full(tv.shape, np.nan)
    for t in range(66, tv.shape[0]):
        for i in range(tv.shape[1]):
            if eds[t, i] != 1:
                continue
            ev3, base = tv[t - 2:t + 1, i], tv[t - 66:t - 6, i]       # a-1..a+1 and a-65..a-6 (a = t-1)
            if np.isfinite(ev3).all() and np.isfinite(base).all():
                u[t, i] = np.log(ev3.sum() / (3.0 * base.mean()))
    held = np.full(u.shape, np.nan)
    for t in range(u.shape[0]):
        for i in range(u.shape[1]):
            for k in range(min(126, t + 1)):
                if not np.isnan(u[t - k, i]):
                    held[t, i] = u[t - k, i]
                    break
    return ref_rank(ref_decay(held, 21))


def def_dato(e):
    ok = (e["noa"] > 0) & (e["noa_lag4"] > 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        d = np.where(ok, e["sale_ttm"] / e["noa"] - _ref_shift(e["sale_ttm"], 252) / e["noa_lag4"], np.nan)
    return ref_group_rank(ref_decay(d, 21), e["grp_ff12"])


def def_fscore_hbm(e):
    with np.errstate(invalid="ignore", divide="ignore"):
        bm = np.where(e["be"] > 0, e["be"] / e["me_company"], np.nan)
    q = ref_rank(bm)
    v = np.where(np.isnan(q), np.nan, np.where(q > 0.8, e["fscore"] - 4.5, 0.0))
    return ref_rank(ref_decay(v, 21))


def def_ins_cluster(e):
    """Long issuers where at least 3 distinct insiders bought on sessions t-21..t-1 (the field's window), from the
    synthetic trade table; NaN without Section 16 presence."""
    b = e["buys"]
    v = np.full(b.shape[:2], np.nan)
    for t in range(b.shape[0]):
        for i in range(b.shape[1]):
            if np.isnan(e["present"][t, i]):
                continue
            buyers = {k for s in range(max(0, t - 21), t) for k in range(b.shape[2]) if b[s, i, k]}
            v[t, i] = 1.0 if len(buyers) >= 3 else 0.0
    return ref_rank(v)


DEFINITIONS = {"peer_mom_1m": def_peer_mom_1m, "so_wang_rev": def_so_wang_rev, "mom_turn": def_mom_turn,
               "ea_uvol": def_ea_uvol, "dato": def_dato, "fscore_hbm": def_fscore_hbm,
               "ins_cluster": def_ins_cluster}
MUTANTS = {  # plausible mistakes; each must fail the cell-for-cell check
    "peer_mom_1m": [
        f"rank(decay_linear(group_mean({R21}, grp_ff49), 21))",                                   # own return kept
        f"rank(decay_linear((((group_mean({R21}, grp_ff49) * {N49}) - {R21}) / {N49}), 21))",      # n, not n - 1
        CANDIDATES["peer_mom_1m"].replace("grp_ff49", "grp_ff12"),
        CANDIDATES["peer_mom_1m"].replace("delay(close, 21)", "delay(close, 20)")],
    "so_wang_rev": [
        "rank((ea_window_pre5 * (-1 * ((close / delay(close, 3)) - 1))))",                         # not demeaned
        "rank((ea_window_pre5 * group_neutralize(((close / delay(close, 3)) - 1), grp_ff49)))",    # sign
        CANDIDATES["so_wang_rev"].replace("delay(close, 3)", "delay(close, 5)"),
        "rank((-1 * group_neutralize(((close / delay(close, 3)) - 1), grp_ff49)))"],               # no window
    "mom_turn": [
        CANDIDATES["mom_turn"].replace("delay(ts_mean((volume / shares_out), 231), 21)",
                                       "ts_mean((volume / shares_out), 231)"),                     # turnover window
        CANDIDATES["mom_turn"].replace(", 231), 21)) - 0.5)", ", 231), 21)))"),                    # not centred
        CANDIDATES["mom_turn"].replace("(delay(close, 21) / delay(close, 252))",
                                       "(close / delay(close, 252))")],                             # no skip month
    "ea_uvol": [
        CANDIDATES["ea_uvol"].replace("(ea_days_since - 1)", "(ea_days_since - 2)"),
        CANDIDATES["ea_uvol"].replace(", 60), 7)", ", 60), 6)"),
        CANDIDATES["ea_uvol"].replace(", 126), 21))", ", 63), 21))"),
        CANDIDATES["ea_uvol"].replace("ts_sum((volume / shares_out), 3)", "ts_sum(volume, 3)")],
    "dato": [
        CANDIDATES["dato"].replace("(delay(sale_ttm, 252) / noa_lag4)", "(delay(sale_ttm, 252) / noa)"),
        CANDIDATES["dato"].replace("delay(sale_ttm, 252)", "delay(sale_ttm, 189)"),
        CANDIDATES["dato"].replace("grp_ff12)", "grp_ff49)"),
        "rank(decay_linear(((((sale_ttm / noa) - (delay(sale_ttm, 252) / noa_lag4)) + (0 * log(noa))) + "
        "(0 * log(noa_lag4))), 21))"],
    "fscore_hbm": [
        CANDIDATES["fscore_hbm"].replace("> 0.8)", "< 0.2)"),
        CANDIDATES["fscore_hbm"].replace("(fscore - 4.5)", "(fscore - 5)"),
        "rank(decay_linear(((rank((be / me_company)) > 0.8) ? (fscore - 4.5) : 0), 21))"],          # no book guard
    "ins_cluster": [
        "rank((-1 * ins_cluster_buy))",                                                             # sign
        "rank(ins_n_buyers)",                                                                       # 126-session count
        "rank(decay_linear(ins_cluster_buy, 21))"],                                                 # held past the window
}


def same(a, b, tol=1e-9):
    return bool(np.array_equal(np.isnan(a), np.isnan(b)) and np.allclose(a[~np.isnan(a)], b[~np.isnan(b)], rtol=0,
                                                                           atol=tol))


def moved_after(e, t0, seed):
    """Every input row after t0 replaced (labels permuted, flags and counts redrawn, levels rescaled)."""
    rng = np.random.default_rng(seed)
    f = {k: v.copy() for k, v in e.items()}
    for k, v in f.items():
        tail = v[t0 + 1:]
        if k in ("buys", "present"):     # the trade table behind the insider fields (the DSL reads the fields)
            continue
        if k.startswith("grp_"):
            v[t0 + 1:] = rng.permutation(tail.ravel()).reshape(tail.shape)
        elif k in ("ea_window_pre5", "ins_cluster_buy"):
            v[t0 + 1:] = np.where(np.isnan(tail), np.nan, (rng.random(tail.shape) < 0.2).astype(float))
        elif k in ("ea_days_since", "fscore", "ins_n_buyers"):
            v[t0 + 1:] = np.where(np.isnan(tail), np.nan, rng.integers(0, 9, tail.shape).astype(float))
        else:
            v[t0 + 1:] = tail * np.exp(rng.normal(0.0, 0.3, tail.shape))
    return f


def semantic_checks():
    e = world()
    lines, n_mut = [], 0
    t0 = 300
    for cid, ref_fn in DEFINITIONS.items():
        got, ref = run(CANDIDATES[cid], e), ref_fn(e)
        cells = int(np.isfinite(ref).sum())
        assert cells >= 400, f"{cid}: too few finite cells ({cells})"
        assert same(got, ref), f"{cid}: the DSL differs from its registered definition"
        for m in MUTANTS[cid]:
            assert m != CANDIDATES[cid], f"{cid}: a mutant equals the string"
            assert not same(run(m, e), ref), f"{cid}: mutant passes: {m}"
            n_mut += 1
        later = run(CANDIDATES[cid], moved_after(e, t0, 7))
        assert same(later[:t0 + 1], got[:t0 + 1]), f"{cid}: rows <= t0 move when later rows move (look-ahead)"
        assert not same(later[t0 + 1:], got[t0 + 1:]), f"{cid}: the causality probe moved nothing"
        lines.append(f"{cid:12s} == registered definition on {cells} cells; {len(MUTANTS[cid])} mutants fail; "
                     "causal (rows <= t0 fixed when every later row moves)")
    leak = {**e, "close": np.vstack([e["close"][1:], e["close"][-1:]])}       # reads session t+1's close at row t
    leak_moved = moved_after(e, t0, 7)
    leak_moved = {**leak_moved, "close": np.vstack([leak_moved["close"][1:], leak_moved["close"][-1:]])}
    assert not same(def_peer_mom_1m(leak_moved)[:t0 + 1], def_peer_mom_1m(leak)[:t0 + 1]), "probe has no teeth"
    lines.append(f"mutation probe: {n_mut} mutants, every one fails; causality probe fails on a reference that reads "
                 "t+1 (teeth)")
    return lines


def main():
    rd = readers()
    st = {k: xs.static(v) for k, v in rd.items()}
    read = set().union(*(s["fields"] for s in st.values()))
    unread = [f for f in V14 if f not in read]
    assert len(V13) == 75 and SPEC_V13["manifest_sha256"].startswith("e5f7f28c"), "fields v13 list"
    print(f"readers: {len(rd)} strings (52 v8.0, {len(X_LIST)} X-2/X-3/X-4, {len(XWQ_12)} X-7); fields v14 "
          f"{len(V14)}; read {len([f for f in V14 if f in read])}; zero readers {len(unread)}:")
    print("  " + " ".join(unread))
    lapsed_reads = sorted(set().union(*(xs.static(BY_ID[i]["dsl"])["fields"] for i in LAPSED)) & set(unread))
    print(f"  of these, read only by lapsed R-7 strings: {lapsed_reads}")
    canon = {xwq.canonical(xs.parse(v)) for v in rd.values()}
    shas = {s["sha256"] for s in st.values()}
    carried = lib2_lines()
    for cid, text in CANDIDATES.items():
        s = xs.static(text)
        assert (s["bars"], s["slots"], s["nodes"], s["extra_fields"]) == EXPECTED[cid], (cid, s)
        assert s["bars"] <= xs.HOUSE["bars"] and s["slots"] <= xs.HOUSE["slots"], cid
        assert len(s["extra_fields"]) <= xs.HOUSE["extra_fields"] and s["bytes"] <= xs.HOUSE["bytes"], cid
        assert s["sha256"] not in shas and xwq.canonical(xs.parse(text)) not in canon, f"{cid} restates a reader"
        if cid in CARRIED:
            assert s["sha256"][:16] == LIB2_SHA16[cid], f"{cid}: not the LIB2 string"
            assert f'--dsl "{text}"' in carried[cid], f"{cid}: not the LIB2 add-alpha line"
        rows = [f for f in s["extra_fields"] if f not in REG["fields"]]
        new_reads = [f for f in s["extra_fields"] if f in unread or f not in V14]
        print(f"{cid:13s} bars {s['bars']:3d} slots {s['slots']} nodes {s['nodes']:2d} bytes {s['bytes']:3d} "
              f"extra {s['extra_fields']} unread-field reads {new_reads} registry rows needed {rows} "
              f"sha256 {s['sha256']}")
    for line in semantic_checks():
        print(line)
    for cid in CANDIDATES:
        line = add_alpha_line(cid)
        print(f"[{sha(line)[:16]}] {line}")
    print("ysig_check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
