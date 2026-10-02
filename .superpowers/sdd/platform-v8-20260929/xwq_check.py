"""Lane XWQ (expansion X, cell X-7; Rulings PM7-33, PM7-34): Kakushadze (2016) "101 Formulaic Alphas" in the DSL.

Run from the repository root (no arguments; synthetic data only, reads no data payload):
    "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/xwq_check.py

1. TABLE: every one of the 101 printed formulas (arXiv:1601.00991 appendix A.1, verbatim), its exactness class in the
   DSL with the fields in house (plus the three bar fields of research_fields_ohlc.py), the missing input for the ones
   that are not exact, its mechanism cluster, and for every exact one the DSL transcription.
2. The compiler mirror of lane XSIG (xsig_check.py: parser, typing, bars, slots, nodes; reused, not rewritten) checks
   every exact transcription, wrapped in its house form (PM7-34: rank(decay_linear(x, 5)) when the longest printed
   window is under 10 sessions, rank(decay_linear(x, 21)) otherwise), against the house budget.
3. SELECTION: the rule of task-XWQ-report.md section 3, coded (eligibility filters, one pick per cluster, fixed
   tie-break); it reads only this table and the mirror's static figures (no return, IC or Sharpe of any window).
K1 (``--plan-only`` through add-alpha) remains the checker of record; this file states what K1 should print.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xsig_check as xs  # noqa: E402  (lane XSIG's compiler mirror, same directory)

NEW_FIELDS = ("open_adj", "high_adj", "low_adj")     # research_fields_ohlc.py (this lane)
xs.FIELDS |= set(NEW_FIELDS)

# ------------------------------------------------------------------------------------------------ the printed formulas
# Appendix A.1 of arXiv:1601.00991v3, verbatim (line breaks of the PDF joined with one space).
PRINTED = {
    1: "(rank(Ts_ArgMax(SignedPower(((returns < 0) ? stddev(returns, 20) : close), 2.), 5)) - 0.5)",
    2: "(-1 * correlation(rank(delta(log(volume), 2)), rank(((close - open) / open)), 6))",
    3: "(-1 * correlation(rank(open), rank(volume), 10))",
    4: "(-1 * Ts_Rank(rank(low), 9))",
    5: "(rank((open - (sum(vwap, 10) / 10))) * (-1 * abs(rank((close - vwap)))))",
    6: "(-1 * correlation(open, volume, 10))",
    7: "((adv20 < volume) ? ((-1 * ts_rank(abs(delta(close, 7)), 60)) * sign(delta(close, 7))) : (-1 * 1))",
    8: "(-1 * rank(((sum(open, 5) * sum(returns, 5)) - delay((sum(open, 5) * sum(returns, 5)), 10))))",
    9: "((0 < ts_min(delta(close, 1), 5)) ? delta(close, 1) : ((ts_max(delta(close, 1), 5) < 0) ? delta(close, 1) : "
       "(-1 * delta(close, 1))))",
    10: "rank(((0 < ts_min(delta(close, 1), 4)) ? delta(close, 1) : ((ts_max(delta(close, 1), 4) < 0) ? delta(close, 1) "
        ": (-1 * delta(close, 1)))))",
    11: "((rank(ts_max((vwap - close), 3)) + rank(ts_min((vwap - close), 3))) * rank(delta(volume, 3)))",
    12: "(sign(delta(volume, 1)) * (-1 * delta(close, 1)))",
    13: "(-1 * rank(covariance(rank(close), rank(volume), 5)))",
    14: "((-1 * rank(delta(returns, 3))) * correlation(open, volume, 10))",
    15: "(-1 * sum(rank(correlation(rank(high), rank(volume), 3)), 3))",
    16: "(-1 * rank(covariance(rank(high), rank(volume), 5)))",
    17: "(((-1 * rank(ts_rank(close, 10))) * rank(delta(delta(close, 1), 1))) * rank(ts_rank((volume / adv20), 5)))",
    18: "(-1 * rank(((stddev(abs((close - open)), 5) + (close - open)) + correlation(close, open, 10))))",
    19: "((-1 * sign(((close - delay(close, 7)) + delta(close, 7)))) * (1 + rank((1 + sum(returns, 250)))))",
    20: "(((-1 * rank((open - delay(high, 1)))) * rank((open - delay(close, 1)))) * rank((open - delay(low, 1))))",
    21: "((((sum(close, 8) / 8) + stddev(close, 8)) < (sum(close, 2) / 2)) ? (-1 * 1) : (((sum(close, 2) / 2) < "
        "((sum(close, 8) / 8) - stddev(close, 8))) ? 1 : (((1 < (volume / adv20)) || ((volume / adv20) == 1)) ? 1 : "
        "(-1 * 1))))",
    22: "(-1 * (delta(correlation(high, volume, 5), 5) * rank(stddev(close, 20))))",
    23: "(((sum(high, 20) / 20) < high) ? (-1 * delta(high, 2)) : 0)",
    24: "((((delta((sum(close, 100) / 100), 100) / delay(close, 100)) < 0.05) || ((delta((sum(close, 100) / 100), 100) "
        "/ delay(close, 100)) == 0.05)) ? (-1 * (close - ts_min(close, 100))) : (-1 * delta(close, 3)))",
    25: "rank(((((-1 * returns) * adv20) * vwap) * (high - close)))",
    26: "(-1 * ts_max(correlation(ts_rank(volume, 5), ts_rank(high, 5), 5), 3))",
    27: "((0.5 < rank((sum(correlation(rank(volume), rank(vwap), 6), 2) / 2.0))) ? (-1 * 1) : 1)",
    28: "scale(((correlation(adv20, low, 5) + ((high + low) / 2)) - close))",
    29: "(min(product(rank(rank(scale(log(sum(ts_min(rank(rank((-1 * rank(delta((close - 1), 5))))), 2), 1))))), 1), 5) "
        "+ ts_rank(delay((-1 * returns), 6), 5))",
    30: "(((1.0 - rank(((sign((close - delay(close, 1))) + sign((delay(close, 1) - delay(close, 2)))) + "
        "sign((delay(close, 2) - delay(close, 3)))))) * sum(volume, 5)) / sum(volume, 20))",
    31: "((rank(rank(rank(decay_linear((-1 * rank(rank(delta(close, 10)))), 10)))) + rank((-1 * delta(close, 3)))) + "
        "sign(scale(correlation(adv20, low, 12))))",
    32: "(scale(((sum(close, 7) / 7) - close)) + (20 * scale(correlation(vwap, delay(close, 5), 230))))",
    33: "rank((-1 * ((1 - (open / close))^1)))",
    34: "rank(((1 - rank((stddev(returns, 2) / stddev(returns, 5)))) + (1 - rank(delta(close, 1)))))",
    35: "((Ts_Rank(volume, 32) * (1 - Ts_Rank(((close + high) - low), 16))) * (1 - Ts_Rank(returns, 32)))",
    36: "(((((2.21 * rank(correlation((close - open), delay(volume, 1), 15))) + (0.7 * rank((open - close)))) + (0.73 * "
        "rank(Ts_Rank(delay((-1 * returns), 6), 5)))) + rank(abs(correlation(vwap, adv20, 6)))) + (0.6 * "
        "rank((((sum(close, 200) / 200) - open) * (close - open)))))",
    37: "(rank(correlation(delay((open - close), 1), close, 200)) + rank((open - close)))",
    38: "((-1 * rank(Ts_Rank(close, 10))) * rank((close / open)))",
    39: "((-1 * rank((delta(close, 7) * (1 - rank(decay_linear((volume / adv20), 9)))))) * (1 + rank(sum(returns, "
        "250))))",
    40: "((-1 * rank(stddev(high, 10))) * correlation(high, volume, 10))",
    41: "(((high * low)^0.5) - vwap)",
    42: "(rank((vwap - close)) / rank((vwap + close)))",
    43: "(ts_rank((volume / adv20), 20) * ts_rank((-1 * delta(close, 7)), 8))",
    44: "(-1 * correlation(high, rank(volume), 5))",
    45: "(-1 * ((rank((sum(delay(close, 5), 20) / 20)) * correlation(close, volume, 2)) * rank(correlation(sum(close, 5), "
        "sum(close, 20), 2))))",
    46: "((0.25 < (((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10))) ? (-1 * 1) : "
        "(((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10)) < 0) ? 1 : ((-1 * 1) * "
        "(close - delay(close, 1)))))",
    47: "((((rank((1 / close)) * volume) / adv20) * ((high * rank((high - close))) / (sum(high, 5) / 5))) - "
        "rank((vwap - delay(vwap, 5))))",
    48: "(indneutralize(((correlation(delta(close, 1), delta(delay(close, 1), 1), 250) * delta(close, 1)) / close), "
        "IndClass.subindustry) / sum(((delta(close, 1) / delay(close, 1))^2), 250))",
    49: "(((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10)) < (-1 * 0.1)) ? 1 : ((-1 * "
        "1) * (close - delay(close, 1))))",
    50: "(-1 * ts_max(rank(correlation(rank(volume), rank(vwap), 5)), 5))",
    51: "(((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10)) < (-1 * 0.05)) ? 1 : ((-1 * "
        "1) * (close - delay(close, 1))))",
    52: "((((-1 * ts_min(low, 5)) + delay(ts_min(low, 5), 5)) * rank(((sum(returns, 240) - sum(returns, 20)) / 220))) * "
        "ts_rank(volume, 5))",
    53: "(-1 * delta((((close - low) - (high - close)) / (close - low)), 9))",
    54: "((-1 * ((low - close) * (open^5))) / ((low - high) * (close^5)))",
    55: "(-1 * correlation(rank(((close - ts_min(low, 12)) / (ts_max(high, 12) - ts_min(low, 12)))), rank(volume), 6))",
    56: "(0 - (1 * (rank((sum(returns, 10) / sum(sum(returns, 2), 3))) * rank((returns * cap)))))",
    57: "(0 - (1 * ((close - vwap) / decay_linear(rank(ts_argmax(close, 30)), 2))))",
    58: "(-1 * Ts_Rank(decay_linear(correlation(IndNeutralize(vwap, IndClass.sector), volume, 3.92795), 7.89291), "
        "5.50322))",
    59: "(-1 * Ts_Rank(decay_linear(correlation(IndNeutralize(((vwap * 0.728317) + (vwap * (1 - 0.728317))), "
        "IndClass.industry), volume, 4.25197), 16.2289), 8.19648))",
    60: "(0 - (1 * ((2 * scale(rank(((((close - low) - (high - close)) / (high - low)) * volume)))) - "
        "scale(rank(ts_argmax(close, 10))))))",
    61: "(rank((vwap - ts_min(vwap, 16.1219))) < rank(correlation(vwap, adv180, 17.9282)))",
    62: "((rank(correlation(vwap, sum(adv20, 22.4101), 9.91009)) < rank(((rank(open) + rank(open)) < (rank(((high + low) "
        "/ 2)) + rank(high))))) * -1)",
    63: "((rank(decay_linear(delta(IndNeutralize(close, IndClass.industry), 2.25164), 8.22237)) - "
        "rank(decay_linear(correlation(((vwap * 0.318108) + (open * (1 - 0.318108))), sum(adv180, 37.2467), 13.557), "
        "12.2883))) * -1)",
    64: "((rank(correlation(sum(((open * 0.178404) + (low * (1 - 0.178404))), 12.7054), sum(adv120, 12.7054), "
        "16.6208)) < rank(delta(((((high + low) / 2) * 0.178404) + (vwap * (1 - 0.178404))), 3.69741))) * -1)",
    65: "((rank(correlation(((open * 0.00817205) + (vwap * (1 - 0.00817205))), sum(adv60, 8.6911), 6.40374)) < "
        "rank((open - ts_min(open, 13.635)))) * -1)",
    66: "((rank(decay_linear(delta(vwap, 3.51013), 7.23052)) + Ts_Rank(decay_linear(((((low * 0.96633) + (low * (1 - "
        "0.96633))) - vwap) / (open - ((high + low) / 2))), 11.4157), 6.72611)) * -1)",
    67: "((rank((high - ts_min(high, 2.14593)))^rank(correlation(IndNeutralize(vwap, IndClass.sector), "
        "IndNeutralize(adv20, IndClass.subindustry), 6.02936))) * -1)",
    68: "((Ts_Rank(correlation(rank(high), rank(adv15), 8.91644), 13.9333) < rank(delta(((close * 0.518371) + (low * (1 "
        "- 0.518371))), 1.06157))) * -1)",
    69: "((rank(ts_max(delta(IndNeutralize(vwap, IndClass.industry), 2.72412), 4.79344))^Ts_Rank(correlation(((close * "
        "0.490655) + (vwap * (1 - 0.490655))), adv20, 4.92416), 9.0615)) * -1)",
    70: "((rank(delta(vwap, 1.29456))^Ts_Rank(correlation(IndNeutralize(close, IndClass.industry), adv50, 17.8256), "
        "17.9171)) * -1)",
    71: "max(Ts_Rank(decay_linear(correlation(Ts_Rank(close, 3.43976), Ts_Rank(adv180, 12.0647), 18.0175), 4.20501), "
        "15.6948), Ts_Rank(decay_linear((rank(((low + open) - (vwap + vwap)))^2), 16.4662), 4.4388))",
    72: "(rank(decay_linear(correlation(((high + low) / 2), adv40, 8.93345), 10.1519)) / "
        "rank(decay_linear(correlation(Ts_Rank(vwap, 3.72469), Ts_Rank(volume, 18.5188), 6.86671), 2.95011)))",
    73: "(max(rank(decay_linear(delta(vwap, 4.72775), 2.91864)), Ts_Rank(decay_linear(((delta(((open * 0.147155) + (low "
        "* (1 - 0.147155))), 2.03608) / ((open * 0.147155) + (low * (1 - 0.147155)))) * -1), 3.33829), 16.7411)) * -1)",
    74: "((rank(correlation(close, sum(adv30, 37.4843), 15.1365)) < rank(correlation(rank(((high * 0.0261661) + (vwap * "
        "(1 - 0.0261661)))), rank(volume), 11.4791))) * -1)",
    75: "(rank(correlation(vwap, volume, 4.24304)) < rank(correlation(rank(low), rank(adv50), 12.4413)))",
    76: "(max(rank(decay_linear(delta(vwap, 1.24383), 11.8259)), Ts_Rank(decay_linear(Ts_Rank(correlation(IndNeutralize("
        "low, IndClass.sector), adv81, 8.14941), 19.569), 17.1543), 19.383)) * -1)",
    77: "min(rank(decay_linear(((((high + low) / 2) + high) - (vwap + high)), 20.0451)), "
        "rank(decay_linear(correlation(((high + low) / 2), adv40, 3.1614), 5.64125)))",
    78: "(rank(correlation(sum(((low * 0.352233) + (vwap * (1 - 0.352233))), 19.7428), sum(adv40, 19.7428), "
        "6.83313))^rank(correlation(rank(vwap), rank(volume), 5.77492)))",
    79: "(rank(delta(IndNeutralize(((close * 0.60733) + (open * (1 - 0.60733))), IndClass.sector), 1.23438)) < "
        "rank(correlation(Ts_Rank(vwap, 3.60973), Ts_Rank(adv150, 9.18637), 14.6644)))",
    80: "((rank(Sign(delta(IndNeutralize(((open * 0.868128) + (high * (1 - 0.868128))), IndClass.industry), "
        "4.04545)))^Ts_Rank(correlation(high, adv10, 5.11456), 5.53756)) * -1)",
    81: "((rank(Log(product(rank((rank(correlation(vwap, sum(adv10, 49.6054), 8.47743))^4)), 14.9655))) < "
        "rank(correlation(rank(vwap), rank(volume), 5.07914))) * -1)",
    82: "(min(rank(decay_linear(delta(open, 1.46063), 14.8717)), Ts_Rank(decay_linear(correlation(IndNeutralize(volume, "
        "IndClass.sector), ((open * 0.634196) + (open * (1 - 0.634196))), 17.4842), 6.92131), 13.4283)) * -1)",
    83: "((rank(delay(((high - low) / (sum(close, 5) / 5)), 2)) * rank(rank(volume))) / (((high - low) / (sum(close, 5) "
        "/ 5)) / (vwap - close)))",
    84: "SignedPower(Ts_Rank((vwap - ts_max(vwap, 15.3217)), 20.7127), delta(close, 4.96796))",
    85: "(rank(correlation(((high * 0.876703) + (close * (1 - 0.876703))), adv30, 9.61331))^rank(correlation(Ts_Rank(((high "
        "+ low) / 2), 3.70596), Ts_Rank(volume, 10.1595), 7.11408)))",
    86: "((Ts_Rank(correlation(close, sum(adv20, 14.7444), 6.00049), 20.4195) < rank(((open + close) - (vwap + open)))) "
        "* -1)",
    87: "(max(rank(decay_linear(delta(((close * 0.369701) + (vwap * (1 - 0.369701))), 1.91233), 2.65461)), "
        "Ts_Rank(decay_linear(abs(correlation(IndNeutralize(adv81, IndClass.industry), close, 13.4132)), 4.89768), "
        "14.4535)) * -1)",
    88: "min(rank(decay_linear(((rank(open) + rank(low)) - (rank(high) + rank(close))), 8.06882)), "
        "Ts_Rank(decay_linear(correlation(Ts_Rank(close, 8.44728), Ts_Rank(adv60, 20.6966), 8.01266), 6.65053), "
        "2.61957))",
    89: "(Ts_Rank(decay_linear(correlation(((low * 0.967285) + (low * (1 - 0.967285))), adv10, 6.94279), 5.51607), "
        "3.79744) - Ts_Rank(decay_linear(delta(IndNeutralize(vwap, IndClass.industry), 3.48158), 10.1466), 15.3012))",
    90: "((rank((close - ts_max(close, 4.66719)))^Ts_Rank(correlation(IndNeutralize(adv40, IndClass.subindustry), low, "
        "5.38375), 3.21856)) * -1)",
    91: "((Ts_Rank(decay_linear(decay_linear(correlation(IndNeutralize(close, IndClass.industry), volume, 9.74928), "
        "16.398), 3.83219), 4.8667) - rank(decay_linear(correlation(vwap, adv30, 4.01303), 2.6809))) * -1)",
    92: "min(Ts_Rank(decay_linear(((((high + low) / 2) + close) < (low + open)), 14.7221), 18.8683), "
        "Ts_Rank(decay_linear(correlation(rank(low), rank(adv30), 7.58555), 6.94024), 6.80584))",
    93: "(Ts_Rank(decay_linear(correlation(IndNeutralize(vwap, IndClass.industry), adv81, 17.4193), 19.848), 7.54455) / "
        "rank(decay_linear(delta(((close * 0.524434) + (vwap * (1 - 0.524434))), 2.77377), 16.2664)))",
    94: "((rank((vwap - ts_min(vwap, 11.5783)))^Ts_Rank(correlation(Ts_Rank(vwap, 19.6462), Ts_Rank(adv60, 4.02992), "
        "18.0926), 2.70756)) * -1)",
    95: "(rank((open - ts_min(open, 12.4105))) < Ts_Rank((rank(correlation(sum(((high + low) / 2), 19.1351), sum(adv40, "
        "19.1351), 12.8742))^5), 11.7584))",
    96: "(max(Ts_Rank(decay_linear(correlation(rank(vwap), rank(volume), 3.83878), 4.16783), 8.38151), "
        "Ts_Rank(decay_linear(Ts_ArgMax(correlation(Ts_Rank(close, 7.45404), Ts_Rank(adv60, 4.13242), 3.65459), "
        "12.6556), 14.0365), 13.4143)) * -1)",
    97: "((rank(decay_linear(delta(IndNeutralize(((low * 0.721001) + (vwap * (1 - 0.721001))), IndClass.industry), "
        "3.3705), 20.4523)) - Ts_Rank(decay_linear(Ts_Rank(correlation(Ts_Rank(low, 7.87871), Ts_Rank(adv60, 17.255), "
        "4.97547), 18.5925), 15.7152), 6.71659)) * -1)",
    98: "(rank(decay_linear(correlation(vwap, sum(adv5, 26.4719), 4.58418), 7.18088)) - "
        "rank(decay_linear(Ts_Rank(Ts_ArgMin(correlation(rank(open), rank(adv15), 20.8187), 8.62571), 6.95668), "
        "8.07206)))",
    99: "((rank(correlation(sum(((high + low) / 2), 19.8975), sum(adv60, 19.8975), 8.8136)) < rank(correlation(low, "
        "volume, 6.28259))) * -1)",
    100: "(0 - (1 * (((1.5 * scale(indneutralize(indneutralize(rank(((((close - low) - (high - close)) / (high - low)) * "
         "volume)), IndClass.subindustry), IndClass.subindustry))) - scale(indneutralize((correlation(close, "
         "rank(adv20), 5) - rank(ts_argmin(close, 30))), IndClass.subindustry))) * (volume / adv20))))",
    101: "((close - open) / ((high - low) + .001))",
}

# ------------------------------------------------------------------------------------------------ transcription macros
# Readings (task-XWQ-report.md section 1): returns = adjusted close-to-close; adv{d} = mean of raw close x share volume
# over the last d sessions (the house dollar ADV, atx-impl equity_baseline_views kEquityDollarAdvDsl); K = raw_close /
# close = 1 / F(t) rebases a level on the house basis to prices adjusted as of the evaluation row (class L);
# IndClass.sector / industry / subindustry = grp_ff12 / grp_ff49 / grp_sic2; fractional windows floored (paper A.2).
MACROS = {"R": "((close / delay(close, 1)) - 1)", "K": "(raw_close / close)", "O": "open_adj", "H": "high_adj",
          "L": "low_adj", **{f"ADV{d}": f"ts_mean((raw_close * volume), {d})" for d in (20, 30, 40, 60)}}
A46 = "((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10)) * {K})"


def _t(template: str) -> str:
    return template.replace("{A}", A46).format(**MACROS)


VWAP = "vwap: the daily volume-weighted average price needs intraday trade prices; no source in house carries it and " \
       "no daily-bar proxy meets the definition exactly"


def nested(why: str) -> str:
    return ("price level inside a time-series op (" + why + "): under the paper's adjustment (prices adjusted as of the "
            "evaluation day) every past cross-section is re-evaluated on that day's adjusted prices; the DSL evaluates "
            "a past cross-section once, so the formula is not exactly expressible (a 2-D re-evaluation, not an "
            "operator or a field)")


# n: (class, cluster, DSL template or None, flags, note). Classes: S = scale-free (invariant to any positive per-line
# rescaling of the price history: exact on the house adjusted basis); L = level-dependent, every level term at the
# evaluation row, rebased by K (exact); N = not exact (nested level; note); V = needs vwap.
# Clusters (mechanism, by the input that sets the sign): ctc_rev = 1-10 session close-to-close reversal, alone or
# scaled by momentum, size, volume or volatility; pv_vol = time-series correlation of a price or range series with share
# volume; pv_liq = correlation of a price series with dollar ADV; vol_ret = correlation of volume change with return;
# range_vol = position in the multi-day or daily range combined with volume; bar = same-day open / high / low / close
# structure; gap = overnight gap against the previous day; vol_rev = reversal confirmed by abnormal volume in own-history
# ranks; ar1 = own return autocorrelation.
TABLE = {
    1: ("N", None, None, set(), nested("close and stddev(returns) compared inside ts_argmax")),
    2: ("S", "vol_ret", "(-1 * correlation(rank(delta(log(volume), 2)), rank(((close - {O}) / {O})), 6))", set(), ""),
    3: ("N", None, None, set(), nested("rank(open) inside correlation")),
    4: ("N", None, None, set(), nested("rank(low) inside ts_rank")),
    5: ("V", None, None, set(), VWAP),
    6: ("S", "pv_vol", "(-1 * correlation({O}, volume, 10))", set(), ""),
    7: ("S", "ctc_rev", "(({ADV20} < volume) ? ((-1 * ts_rank(abs(delta(close, 7)), 60)) * sign(delta(close, 7))) : "
                        "(-1 * 1))", {"degenerate"},
        "compares adv20 (dollars) with volume (shares) as printed: false unless price < volume / mean volume, so the "
        "value is -1 almost everywhere above a $1 price"),
    8: ("L", "ctc_rev", "(-1 * rank((((ts_sum({O}, 5) * ts_sum({R}, 5)) - delay((ts_sum({O}, 5) * ts_sum({R}, 5)), "
                        "10)) * {K})))", set(), ""),
    9: ("L", "ctc_rev", "((0 < ts_min(delta(close, 1), 5)) ? (delta(close, 1) * {K}) : ((ts_max(delta(close, 1), 5) < "
                        "0) ? (delta(close, 1) * {K}) : (-1 * (delta(close, 1) * {K}))))", set(), ""),
    10: ("L", "ctc_rev", "rank(((0 < ts_min(delta(close, 1), 4)) ? (delta(close, 1) * {K}) : ((ts_max(delta(close, 1), "
                         "4) < 0) ? (delta(close, 1) * {K}) : (-1 * (delta(close, 1) * {K})))))", set(), ""),
    11: ("V", None, None, set(), VWAP),
    12: ("L", "ctc_rev", "(sign(delta(volume, 1)) * (-1 * (delta(close, 1) * {K})))", set(), ""),
    13: ("N", None, None, set(), nested("rank(close) inside covariance")),
    14: ("S", "pv_vol", "((-1 * rank(delta({R}, 3))) * correlation({O}, volume, 10))", set(), ""),
    15: ("N", None, None, set(), nested("rank(high) inside correlation")),
    16: ("N", None, None, set(), nested("rank(high) inside covariance")),
    17: ("L", "vol_rev", "(((-1 * rank(ts_rank(close, 10))) * rank((delta(delta(close, 1), 1) * {K}))) * "
                         "rank(ts_rank((volume / {ADV20}), 5)))", set(), ""),
    18: ("L", "bar", "(-1 * rank((((stddev(abs((close - {O})), 5) + (close - {O})) * {K}) + correlation(close, {O}, "
                     "10))))", set(), ""),
    19: ("S", "ctc_rev", "((-1 * sign(((close - delay(close, 7)) + delta(close, 7)))) * (1 + rank((1 + ts_sum({R}, "
                         "250)))))", set(), ""),
    20: ("L", "gap", "(((-1 * rank((({O} - delay({H}, 1)) * {K}))) * rank((({O} - delay(close, 1)) * {K}))) * "
                     "rank((({O} - delay({L}, 1)) * {K})))", set(), ""),
    21: ("S", "ctc_rev", "((((ts_sum(close, 8) / 8) + stddev(close, 8)) < (ts_sum(close, 2) / 2)) ? (-1 * 1) : "
                         "(((ts_sum(close, 2) / 2) < ((ts_sum(close, 8) / 8) - stddev(close, 8))) ? 1 : (((1 < (volume "
                         "/ {ADV20})) || ((volume / {ADV20}) == 1)) ? 1 : (-1 * 1))))", {"degenerate"},
         "volume / adv20 (shares over dollars) >= 1 only below a $1 price: the inner branch is -1 almost everywhere"),
    22: ("L", "pv_vol", "(-1 * (delta(correlation({H}, volume, 5), 5) * rank((stddev(close, 20) * {K}))))", set(), ""),
    23: ("L", "ctc_rev", "(((ts_sum({H}, 20) / 20) < {H}) ? (-1 * (delta({H}, 2) * {K})) : 0)", set(), ""),
    24: ("L", "ctc_rev", "((((delta((ts_sum(close, 100) / 100), 100) / delay(close, 100)) < 0.05) || "
                         "((delta((ts_sum(close, 100) / 100), 100) / delay(close, 100)) == 0.05)) ? (-1 * ((close - "
                         "ts_min(close, 100)) * {K})) : (-1 * (delta(close, 3) * {K})))", set(), ""),
    25: ("V", None, None, set(), VWAP),
    26: ("S", "pv_vol", "(-1 * ts_max(correlation(ts_rank(volume, 5), ts_rank({H}, 5), 5), 3))", set(), ""),
    27: ("V", None, None, set(), VWAP),
    28: ("L", "bar", "scale(((correlation({ADV20}, {L}, 5) + ((({H} + {L}) / 2) * {K})) - (close * {K})))", set(), ""),
    29: ("N", None, None, set(), nested("rank(delta(close, 5)) in dollars inside ts_min; also log of a minimum rank of "
                                         "0")),
    30: ("S", "vol_rev", "(((1.0 - rank(((sign((close - delay(close, 1))) + sign((delay(close, 1) - delay(close, 2)))) + "
                         "sign((delay(close, 2) - delay(close, 3)))))) * ts_sum(volume, 5)) / ts_sum(volume, 20))",
         set(), ""),
    31: ("N", None, None, set(), nested("rank(delta(close, 10)) in dollars inside decay_linear")),
    32: ("V", None, None, set(), VWAP),
    33: ("S", "bar", "rank((-1 * power((1 - ({O} / close)), 1)))", set(), ""),
    34: ("L", "ctc_rev", "rank(((1 - rank((stddev({R}, 2) / stddev({R}, 5)))) + (1 - rank((delta(close, 1) * {K})))))",
         set(), ""),
    35: ("S", "vol_rev", "((ts_rank(volume, 32) * (1 - ts_rank(((close + {H}) - {L}), 16))) * (1 - ts_rank({R}, 32)))",
         set(), ""),
    36: ("V", None, None, set(), VWAP),
    37: ("L", "bar", "(rank(correlation(delay(({O} - close), 1), close, 200)) + rank((({O} - close) * {K})))", set(), ""),
    38: ("S", "bar", "((-1 * rank(ts_rank(close, 10))) * rank((close / {O})))", set(), ""),
    39: ("L", "ctc_rev", "((-1 * rank(((delta(close, 7) * {K}) * (1 - rank(decay_linear((volume / {ADV20}), 9)))))) * "
                         "(1 + rank(ts_sum({R}, 250))))", set(), ""),
    40: ("L", "pv_vol", "((-1 * rank((stddev({H}, 10) * {K}))) * correlation({H}, volume, 10))", set(), ""),
    41: ("V", None, None, set(), VWAP),
    42: ("V", None, None, {"delay0"}, VWAP),
    43: ("S", "vol_rev", "(ts_rank((volume / {ADV20}), 20) * ts_rank((-1 * delta(close, 7)), 8))", set(), ""),
    44: ("S", "pv_vol", "(-1 * correlation({H}, rank(volume), 5))", set(), ""),
    45: ("L", "ctc_rev", "(-1 * ((rank(((ts_sum(delay(close, 5), 20) / 20) * {K})) * correlation(close, volume, 2)) * "
                         "rank(correlation(ts_sum(close, 5), ts_sum(close, 20), 2))))", {"degenerate"},
         "both correlations span 2 sessions: always +-1 (or undefined)"),
    46: ("L", "ctc_rev", "((0.25 < {A}) ? (-1 * 1) : (({A} < 0) ? 1 : ((-1 * 1) * ((close - delay(close, 1)) * {K}))))",
         set(), ""),
    47: ("V", None, None, set(), VWAP),
    48: ("S", "ar1", "(indneutralize(((correlation(delta(close, 1), delta(delay(close, 1), 1), 250) * delta(close, 1)) "
                     "/ close), grp_sic2) / ts_sum(power((delta(close, 1) / delay(close, 1)), 2), 250))", {"delay0"},
         ""),
    49: ("L", "ctc_rev", "(({A} < (-1 * 0.1)) ? 1 : ((-1 * 1) * ((close - delay(close, 1)) * {K})))", set(), ""),
    50: ("V", None, None, set(), VWAP),
    51: ("L", "ctc_rev", "(({A} < (-1 * 0.05)) ? 1 : ((-1 * 1) * ((close - delay(close, 1)) * {K})))", set(), ""),
    52: ("L", "ctc_rev", "(((((-1 * ts_min({L}, 5)) + delay(ts_min({L}, 5), 5)) * {K}) * rank(((ts_sum({R}, 240) - "
                         "ts_sum({R}, 20)) / 220))) * ts_rank(volume, 5))", set(), ""),
    53: ("S", "bar", "(-1 * delta((((close - {L}) - ({H} - close)) / (close - {L})), 9))", {"delay0"}, ""),
    54: ("S", "bar", "((-1 * (({L} - close) * power({O}, 5))) / (({L} - {H}) * power(close, 5)))", {"delay0"}, ""),
    55: ("S", "range_vol", "(-1 * correlation(rank(((close - ts_min({L}, 12)) / (ts_max({H}, 12) - ts_min({L}, 12)))), "
                           "rank(volume), 6))", set(), ""),
    56: ("S", "ctc_rev", "(0 - (1 * (rank((ts_sum({R}, 10) / ts_sum(ts_sum({R}, 2), 3))) * rank(({R} * me_company)))))",
         set(), ""),
    57: ("V", None, None, set(), VWAP),
    58: ("V", None, None, set(), VWAP),
    59: ("V", None, None, set(), VWAP),
    60: ("S", "range_vol", "(0 - (1 * ((2 * scale(rank(((((close - {L}) - ({H} - close)) / ({H} - {L})) * volume)))) - "
                           "scale(rank(ts_argmax(close, 10))))))", {"argmax"}, ""),
    **{n: ("V", None, None, set(), VWAP) for n in (61, 62, 63, 64, 65, 66, 67, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
                                                    79, 81, 83, 84, 86, 87, 89, 91, 93, 94, 96, 97, 98)},
    68: ("N", None, None, set(), nested("rank(high) inside correlation")),
    80: ("N", None, None, set(), nested("indneutralize of an open / high price level inside delta")),
    82: ("L", "pv_vol", "(-1 * min(rank((decay_linear(delta({O}, 1), 14) * {K})), "
                        "ts_rank(decay_linear(correlation(indneutralize(volume, grp_ff12), (({O} * 0.634196) + ({O} * "
                        "(1 - 0.634196))), 17), 6), 13)))", set(), ""),
    85: ("S", "pv_liq", "power(rank(correlation((({H} * 0.876703) + (close * (1 - 0.876703))), {ADV30}, 9)), "
                        "rank(correlation(ts_rank((({H} + {L}) / 2), 3), ts_rank(volume, 10), 7)))", set(), ""),
    88: ("N", None, None, set(), nested("rank(open), rank(low), rank(high), rank(close) inside decay_linear")),
    90: ("L", "ctc_rev", "(-1 * power(rank(((close - ts_max(close, 4)) * {K})), ts_rank(correlation(indneutralize("
                         "{ADV40}, grp_sic2), {L}, 5), 3)))", set(), ""),
    92: ("N", None, None, set(), nested("rank(low) inside correlation")),
    95: ("L", "pv_liq", "((rank((({O} - ts_min({O}, 12)) * {K})) < ts_rank(power(rank(correlation(ts_sum((({H} + {L}) / "
                        "2), 19), ts_sum({ADV40}, 19), 12)), 5), 11)) ? 1 : 0)", set(), ""),
    99: ("S", "pv_liq", "(-1 * ((rank(correlation(ts_sum((({H} + {L}) / 2), 19), ts_sum({ADV60}, 19), 8)) < "
                        "rank(correlation({L}, volume, 6))) ? 1 : 0))", set(), ""),
    100: ("S", "range_vol", "(0 - (1 * (((1.5 * scale(indneutralize(indneutralize(rank(((((close - {L}) - ({H} - close)) "
                            "/ ({H} - {L})) * volume)), grp_sic2), grp_sic2))) - scale(indneutralize((correlation(close, "
                            "rank({ADV20}), 5) - rank(ts_argmin(close, 30))), grp_sic2))) * (volume / {ADV20}))))",
          {"argmax"}, ""),
    101: ("L", "bar", "(((close - {O}) * {K}) / ((({H} - {L}) * {K}) + 0.001))", {"stated"}, ""),
}
DELAY0 = {42, 48, 53, 54}           # paper footnote 11: traded at the close of the computing day
assert set(TABLE) == set(PRINTED) == set(range(1, 102)), "the table covers the 101 printed formulas"
assert {n for n, r in TABLE.items() if r[0] == "V"} == {n for n, p in PRINTED.items() if "vwap" in p}, "vwap rows"
assert {n for n, r in TABLE.items() if "delay0" in r[3]} == DELAY0


# Budget order (E5): an over-budget transcription may be replaced by the same formula with the operands of a
# commutative operation or of a comparison swapped (a < b written b > a): same semantics (canonical() below proves the
# two parse trees equal modulo those swaps), fewer peak slots. As written, #99 needs 8 slots; this order needs 7.
ALT_ORDER = {
    99: "(-1 * ((rank(correlation({L}, volume, 6)) > rank(correlation(ts_sum((({H} + {L}) / 2), 19), ts_sum({ADV60}, "
        "19), 8))) ? 1 : 0))",
}
COMMUTATIVE_CALLS = {"correlation", "covariance", "min", "max"}
FLIP = {">": "<", ">=": "<="}


def canonical(node) -> str:
    """A parse tree's text modulo the swaps ALT_ORDER may use (commutative operands sorted, > / >= flipped)."""
    k, kids = node["key"], [canonical(c) for c in node["kids"]]
    if k[0] == "cmp" and k[1] in FLIP:
        k, kids = ("cmp", FLIP[k[1]]), kids[::-1]
    elif k[0] == "bin" and k[1] in ("+", "*"):
        kids = sorted(kids)
    elif k[0] == "call" and k[1] in COMMUTATIVE_CALLS:
        kids = sorted(kids[:2]) + kids[2:]
    return f"{k}[{','.join(kids)}]"


def dsl(n: int) -> str:
    """The registered transcription: the budget order when there is one, else the transcription as written."""
    return _t(ALT_ORDER.get(n, TABLE[n][2]))


# ------------------------------------------------------------------------------------------------ house forms (PM7-34)
def windows(node, out):
    """Every printed window of a parsed DSL tree: the window literal of each windowed time-series op."""
    k = node["key"]
    if k[0] == "call":
        row = xs.OPS[k[1]]
        if row["shape"] == "shape_panel" and row["opcode"] not in xs.NO_WINDOW_PANEL:
            out.append(int(node["kids"][-1]["num"]))
    for c in node["kids"]:
        windows(c, out)
    return out


def longest_window(text: str) -> int:
    return max(windows(xs.parse(text), []), default=0)


def house_form(text: str) -> tuple[str, int]:
    """PM7-34: decay_linear 5 when the longest printed window is under 10 sessions, 21 otherwise; plain rank."""
    d = 5 if longest_window(text) < 10 else 21
    return f"rank(decay_linear({text}, {d}))", d


def statics() -> dict:
    out = {}
    for n, row in TABLE.items():
        if row[0] not in ("S", "L"):
            continue
        text = dsl(n)
        wrapped, d = house_form(text)
        s = xs.static(wrapped)
        s.update(form=d, window=longest_window(text), alpha_nodes=xs.static(text)["nodes"],
                 budget=(s["bars"] <= xs.HOUSE["bars"] and s["slots"] <= xs.HOUSE["slots"]
                         and len(s["extra_fields"]) <= xs.HOUSE["extra_fields"] and s["bytes"] <= xs.HOUSE["bytes"]))
        out[n] = s
    return out


# ------------------------------------------------------------------------------------------------ the selection rule
EXCLUDED_CLUSTERS = {
    "ctc_rev": "the roster holds close-to-close short-term reversal (ind_adj_rev_5 / ind_adj_rev_5_nx); a 1-10 session "
               "reversal variant is a restatement of it (PM7-34)",
    "gap": "the overnight-return sign is contested in the literature and the house withdrew night_day on that ground "
           "(library-v8-draft R7-5)",
}


def select(st: dict) -> tuple[list, dict]:
    """The rule of task-XWQ-report.md section 3. Returns (picks in rank order, {n: why not picked})."""
    why, eligible = {}, []
    for n, (cls, cluster, _, flags, _) in TABLE.items():
        if cls == "V":
            why[n] = "E1 not exact: vwap"
        elif cls == "N":
            why[n] = "E1 not exact: nested price level"
        elif "argmax" in flags:
            why[n] = "E2 sign depends on the ts_argmax / ts_argmin direction reading"
        elif "degenerate" in flags:
            why[n] = "E3 degenerate as printed"
        elif "delay0" in flags:
            why[n] = "E4 delay-0 alpha (traded at the computing close; the house fills one session later)"
        elif not st[n]["budget"]:
            why[n] = "E5 over the house budget"
        elif cluster in EXCLUDED_CLUSTERS:
            why[n] = f"E6 cluster {cluster}"
        else:
            eligible.append(n)
    by_cluster: dict = {}
    for n in eligible:
        by_cluster.setdefault(TABLE[n][1], []).append(n)

    def key(n):   # T0 stated mechanism, T1 S before L, T2 longest window, T3 fewer nodes, T4 paper number
        return ("stated" not in TABLE[n][3], TABLE[n][0] != "S", -st[n]["window"], st[n]["alpha_nodes"], n)

    picks = []
    for cluster, members in by_cluster.items():
        members.sort(key=key)
        picks.append(members[0])
        for n in members[1:]:
            why[n] = f"one pick per cluster ({cluster}: #{members[0]} ranks first)"
    picks.sort(key=lambda n: (-st[n]["window"], n))
    return picks, why


def main():
    for n, alt in ALT_ORDER.items():
        assert canonical(xs.parse(_t(alt))) == canonical(xs.parse(_t(TABLE[n][2]))), f"#{n}: budget order changes it"
        assert xs.static(house_form(_t(alt))[0])["slots"] < xs.static(house_form(_t(TABLE[n][2]))[0])["slots"]
    st = statics()
    exact = sorted(st)
    counts = {c: sum(1 for r in TABLE.values() if r[0] == c) for c in "SLNV"}
    print(f"table: 101 formulas; exact {len(exact)} (S {counts['S']}, L {counts['L']}); not exact "
          f"{counts['N'] + counts['V']} (vwap {counts['V']}, nested level {counts['N']})")
    for n in exact:
        s = st[n]
        print(f"#{n:3d} {TABLE[n][0]} {TABLE[n][1]:9s} win {s['window']:3d} form {s['form']:2d} bars {s['bars']:3d} "
              f"slots {s['slots']} nodes {s['nodes']:2d} bytes {s['bytes']:4d} extra {s['extra_fields']} "
              f"{'ok' if s['budget'] else 'OVER BUDGET'} sha256 {s['sha256'][:16]}")
    picks, why = select(st)
    print(f"selection rule: {len(picks)} picks {picks}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
