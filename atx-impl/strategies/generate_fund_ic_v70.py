"""Deterministic 44-candidate research library v7.0 = library v6.1 + wave 1 (five literature members, W2 DSL ops).

Library v7.0 implements the platform-v7 pre-registration section "Library v7.0 = library v6.1 + wave 1" (declared
2026-09-28 before any IC pass; .superpowers/sdd/platform-20260928/v7-prereg.md), which registers by reference
library-v7-draft.md sections 0 (conventions) and 1 (wave 1) as committed at f43e54d9. Library v6.1
(fund_industry_ic_v61.json, pinned by SHA-256 and re-derived from its own generator) keeps its 39 candidate entries
byte-identical and in the same order; the five wave-1 members are appended in the draft's order:

  qmj_safety   low_risk             B   mean of three ranks: -beta252, -leverage, -ROE volatility (AFP 2019 safety)
  nincr        earnings_momentum    C+  streak of year-on-year quarterly earnings increases, capped at 5
  q5_eg        investment_issuance  B   HMXZ expected investment growth, published slopes, ranked within FF12
  smax5        low_risk             B+  MAX5 / one-month volatility (AFGP 2020 scaled MAX)
  res_mom_ind  price_momentum       B   standardized 12-1 FF49-industry-residual momentum (min-periods)

Each DSL string is the draft's text verbatim, except q5_eg's three slopes: the draft transcribes the NBER working
paper w24709 (Table 1 Panel D, tau = 1: -0.031, 0.530, 0.802) and registers that the Review of Finance 2021 values
replace them when they differ (not a variant). RoF 2021 Table I Panel D (tau = 1, July 1963-December 2018) prints
-0.029 (t -5.63), 0.516 (t 12.75) and 0.771 (t 7.62); those are the slopes used (Q5_SLOPES).

Rulings applied (v7-prereg.md): q5_eg reads six fields, one over the per-candidate budget of five (ruling 3.5a and the
Correction: all six are fields-v7 fields, no new field); qmj_safety's verbatim string needs 8 estimated peak slots,
one over the budget of 7, and Ruling 7.0-b allows 8 (declared before any run; otherwise draft 0 would withdraw it);
max() and sign() enter the checker as POLICY_OPS (Correction); q5 slopes per Ruling 7.0-b; the roster cap rises
48 -> 56; theme ownership_flow goes to the fitter's theme list, not to this library (no wave-1 member carries it).
Every field the five members read is a fields-v7 field the v6.1 library already declares, so the declared field list
is unchanged and no fields v8 is built.

The static validator extends the pinned v4 grammar (the v6.1 generator's private copy) with six registry rows:
sign and max (element-wise, nincr's indicator max(sign(x), 0)) and the W2 literature ops ts_beta_on (one regressor,
window last), ts_topk_mean, ts_mean_mp and ts_std_mp ((x, w, n): window second, the count n peeled). Lookback follows
typecheck.cpp analyze_lit_call: (w - 1) + child lookback. The extended parser reproduces the v4 parse of all 39 v6.1
strings exactly (asserted), and every row is cross-checked against registry.cpp / registry.hpp when present.

No TRAIN return statistic and no 2023+ data were read. Run with --check to verify the committed exact JSON bytes.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import json
import math
import re
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V61_GENERATOR = 'generate_fund_ic_v61.py'
V61_LIBRARY = 'fund_industry_ic_v61.json'
V61_RECIPE = 'fund_industry_ic_v61.recipe.json'
V61_LIBRARY_SHA256 = 'db35c2769f6c13a8d9d5b2897a9e6968855bffbe6214ff96a12f6869cb59f9b5'
V61_RECIPE_SHA256 = '9bf278a6f3dfa78edd506ab3e499b2e8dd1ce34899752fe81df573e65550f547'
V61_CANDIDATES = 39
LIBRARY = 'fund_industry_ic_v70.json'
RECIPE = 'fund_industry_ic_v70.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v70'
PREREG = ('.superpowers/sdd/platform-20260928/v7-prereg.md section "Library v7.0 = library v6.1 + wave 1" (declared '
          '2026-09-28 before any IC pass), registering library-v7-draft.md sections 0-1 as committed at f43e54d9; '
          'with its Correction and Ruling 7.0-b (2026-09-28, before any run); task briefs task-L7-brief.md, '
          'task-L7b-brief.md')
DRAFT_COMMIT = 'f43e54d9'
MAX_ROSTER = 56                     # prereg ruling: generator roster cap 48 -> 56
MAX_EXTRAS_PER_CANDIDATE = 5        # IC-runner plan budget (T27, kept by v5.1 / v6 / v6.1)
EXTRAS_EXCEPTIONS = {'q5_eg': 6}    # draft 3.5a + v7-prereg Correction: q5_eg reads six fields-v7 fields
MAX_SLOTS_PER_CANDIDATE = 7         # the v4-v6.1 library maximum (estimated peak slots, v2 peak_slots)
SLOTS_EXCEPTIONS = {'qmj_safety': 8}  # v7-prereg.md Ruling 7.0-b: qmj_safety at 8 slots (recorded exception)
SMOOTHING = 21

# Registry rows added to the pinned v4 grammar: name -> (min_arity, max_arity, OpCode) of this library's use.
# Classes: sign / max element-wise; LIT_TS_LAST window = last operand (ts_beta_on, one regressor); LIT_TS_COUNT
# (x, w, n) with n peeled into imm[0] (registry row n_hparams 1), so the window is the second operand and n an
# integer in [1, w] (typecheck lit_ts_rails).
V70_REGISTRY_ROWS = {'sign': (1, 1, 'Sign'), 'max': (2, 2, 'MaxP'), 'ts_beta_on': (3, 3, 'TsBetaOn'),
                     'ts_topk_mean': (3, 3, 'TsTopkMean'), 'ts_mean_mp': (3, 3, 'TsMeanMp'),
                     'ts_std_mp': (3, 3, 'TsStdMp')}
LIT_TS_LAST = {'TsBetaOn'}
LIT_TS_COUNT = {'TsTopkMean', 'TsMeanMp', 'TsStdMp'}
LIT_OPS = LIT_TS_LAST | LIT_TS_COUNT
W2_MERGE = 'ce0ab3a6'  # W2 ops merged (pinned golden digests); the IC exe must be built at or after this commit

R = '((close / delay(close, 1)) - 1)'  # draft 0: R written out in full in every string
# Draft section 1 strings, verbatim at f43e54d9 (tests compare them with the draft file when it is present).
DRAFT_DSL = {
    'qmj_safety': f'(((rank(decay_linear((-1 * ts_beta_on({R}, mkt_ret, 252)), 21)) + rank(decay_linear(((-1 * '
                  '(debt / at)) + (0 * log(at))), 21))) + rank(decay_linear((-1 * stddev(((ni_q / be_lag1q) + (0 * '
                  'log(be_lag1q))), 252)), 21))) / 3)',
    'nincr': 'rank(decay_linear((max(sign((ni_q - ni_q_lag4)), 0) * (1 + (delay(max(sign((ni_q - ni_q_lag4)), 0), 63) '
             '* (1 + (delay(max(sign((ni_q - ni_q_lag4)), 0), 126) * (1 + (delay(max(sign((ni_q - ni_q_lag4)), 0), '
             '189) * (1 + delay(max(sign((ni_q - ni_q_lag4)), 0), 252))))))))), 21))',
    'q5_eg': 'group_rank(decay_linear(((((-0.031 * log((me_company / at))) + (0.53 * (cfo_ttm / at))) + (0.802 * '
             '((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))), 21), grp_ff12)',
    'smax5': f'rank(decay_linear((-1 * ((ts_topk_mean({R}, 21, 5) / stddev({R}, 21)) + (0 * log(stddev({R}, 21))))), '
             '21))',
    'res_mom_ind': f'rank(decay_linear(delay((ts_mean_mp(({R} - group_mean({R}, grp_ff49)), 231, 116) / '
                   f'ts_std_mp(({R} - group_mean({R}, grp_ff49)), 231, 116)), 21), 21))',
}
# q5_eg slopes: (term, draft/NBER literal, RoF 2021 literal); the RoF values are used (prereg root check).
Q5_SLOPES = (('log(q)', '(-0.031 * log(', '(-0.029 * log('),
             ('Cop', '(0.53 * (cfo_ttm', '(0.516 * (cfo_ttm'),
             ('dRoe', '(0.802 * (', '(0.771 * ('))
Q5_SLOPE_SOURCES = dict(
    registered=('Hou, Mo, Xue and Zhang, NBER WP w24709 (2018), Table 1 Panel D (log(q), Cop and dRoe), tau = 1, '
                'July 1963-December 2016: log(q) -0.031 (t -5.86), Cop 0.530 (t 12.82), dRoe 0.802 (t 7.75); '
                'the draft DSL spells 0.530 as 0.53'),
    used=('Hou, Mo, Xue and Zhang (2021), An augmented q-factor model with expected growth, Review of Finance 25(1), '
          '1-41, Table I Panel D (log(q), Cop and dRoe), tau = 1, July 1963-December 2018: log(q) -0.029 (t -5.63), '
          'Cop 0.516 (t 12.75), dRoe 0.771 (t 7.62); text: "the Cop slope remains large and significant, 0.52, the '
          'log(q) slope becomes weakly negative, -0.03, and the dRoe slope stays significant at 0.77"'),
    rule=('prereg root check: q5 slopes against the RoF 2021 version (the draft cites the NBER WP); a differing slope '
          'is replaced by the RoF value and disclosed; the rewrite is not a variant (nothing estimated on TRAIN); '
          'v7-prereg.md Ruling 7.0-b: the RoF 2021 Table I Panel D values are used, the NBER WP values disclosed'),
    differ=True)

MEMBERS = [
    dict(id='qmj_safety', theme='low_risk', tier='B', ranking='cross_section', raw_prior_direction=1,
         citation='Asness, Frazzini and Pedersen (2019, RAS) quality minus junk, safety component (BAB, LEV, EVOL); '
                  'Frazzini and Pedersen (2014, JFE) betting against beta',
         prior_sign_source='Asness-Frazzini-Pedersen 2019',
         formula='mean over k of rank(decay_linear(x_k, 21)), x_k in {-beta (252-session OLS slope of ret on '
                 'mkt_ret, ts_beta_on), -(debt / at), -sd(ni_q / be_lag1q, 252)}: the AFP safety z-score in rank '
                 'form (value_composite idiom)',
         form='mean_k R(decay_linear(x_k, 21))', smoothing=SMOOTHING,
         domain='non-positive total assets (LEV leg) or non-positive opening book equity (EVOL leg) -> NaN (house '
                'log guard); a flat or short beta window -> NaN (ts_beta_on); NaN when any of the three legs is NaN',
         deviation='beta is a 252-session daily OLS slope (not Frazzini-Pedersen\'s 5-year 3-day correlation x 1-year '
                   'vol ratio); LEV = debt / at (AFP add minority interest and preferred); EVOL = sd of the as-of '
                   'quarterly ROE over 252 sessions (4-5 quarters under the bound); IVOL omitted (low_ivol removed in '
                   'v6; price-risk-v1 projects out vol63); O-score and Z-score omitted (no current assets or '
                   'liabilities, working capital or retained earnings in fields-v7). Caveat: price-risk-v1 (beta252) '
                   'largely projects out the beta leg, as it did low_beta; LEV and EVOL carry the rest',
         prior='AFP (2019 RAS; 2013 WP Table VI): US safety factor 1956-2012 0.23%/mo excess (t 2.06), 0.57%/mo '
               '4-factor alpha (t 7.97); QMJ 4-factor alpha 0.66%/mo (t 10.20)',
         expected_turnover='low (.02-.03/day [est]); max expected abs(rho) .35-.45 with bac [est]'),
    dict(id='nincr', theme='earnings_momentum', tier='C+', ranking='cross_section', raw_prior_direction=1,
         citation='Barth, Elliott and Finn (1999, JAR) consecutive earnings increases; Green, Hand and Zhang (2017, '
                  'RFS) nincr; counter-evidence Han, He, Rapach and Zhou (2018 WP)',
         prior_sign_source='Barth-Elliott-Finn 1999',
         formula='I_k = max(sign(ni_q - ni_q_lag4), 0) delayed 63k sessions (k = 0..4); nincr = I0 (1 + I1 (1 + I2 '
                 '(1 + I3 (1 + I4)))) in 0..5',
         form='R(decay_linear(x, 21))', smoothing=SMOOTHING,
         domain='NaN when ni_q or ni_q_lag4 is NaN at any of the five lags (sign(NaN) propagates); a zero change '
                'counts as no increase',
         deviation='capped at 5 quarters: Barth-Elliott-Finn allow 8, and a delay of 441 exceeds the 314 bound; net '
                   'income replaces EPS (Chen-Zimmermann use IBQ); the 63-session delays approximate fiscal quarters '
                   '(filing-lag jitter can repeat a quarter); W2 ts_count_increases is not used (on an as-of forward-'
                   'filled series it counts session-to-session increases, while nincr counts positive year-on-year '
                   'changes)',
         prior='Barth-Elliott-Finn (1999 JAR): increase streaks earn a valuation premium; Green-Hand-Zhang (2017 '
               'RFS) non-microcaps 1980-2014: independent after 2003, post-2003 t ~ 1.8 (secondary report); Han-He-'
               'Rapach-Zhou (2018 WP): univariate value-weighted slope insignificant, hence C+',
         expected_turnover='very low (.01-.02/day [est]); max expected abs(rho) .30-.40 with droe, sue [est]'),
    dict(id='q5_eg', theme='investment_issuance', tier='B', ranking='within_industry_grp_ff12', raw_prior_direction=1,
         citation='Hou, Mo, Xue and Zhang (2021, RF) q5 expected investment growth, Table I Panel D slopes (tau = 1)',
         prior_sign_source='Hou-Mo-Xue-Zhang 2021',
         formula='E[d1 I/A] = -0.029 log(me_company / at) + 0.516 cfo_ttm / at + 0.771 (ROE - ROE 252 sessions ago), '
                 'ROE = ni_q / be_lag1q: published time-series average Fama-MacBeth slopes (RoF 2021 Table I Panel '
                 'D, tau = 1), nothing estimated on TRAIN',
         form='R(decay_linear(x, 21))', smoothing=SMOOTHING,
         domain='non-positive opening book equity (log guard) -> NaN; non-positive me_company / at -> NaN (log); a '
                'negative book equity a year ago is not guarded',
         deviation='q = ME / AT (HMXZ add DLTT + DLC; debt dropped for the field budget; log(q) has the smallest '
                   'slope); Cop = CFO / AT (HMXZ: income-statement Ball et al. form; fields-v7 has no COGS, SG&A or '
                   'working capital); dRoe = as-of ROE minus its value 252 sessions ago (HMXZ: four-quarter lag); no '
                   '1-99% winsorization (the DSL winsorize is member-masked cross-sectional and would bring back the '
                   '21-session blackout inside the decay; the final FF12 rank bounds influence); financials kept, '
                   'ranked within FF12 Money; slopes: RoF 2021 values replace the draft\'s NBER w24709 values',
         prior='HMXZ (2021 RoF): Eg factor 0.84%/mo, t 10.27, 1967-2018; high-minus-low E[d1 I/A] decile 1.07%/mo '
               '(t 6.48) (w24709: 0.82%, t 9.81, 1967-2016; decile 1.06%, t 6.25); Cop dominates',
         expected_turnover='low (~.02/day [est]); max expected abs(rho) .60-.70 with cbop (Cop term) [est]'),
    dict(id='smax5', theme='low_risk', tier='B+', ranking='cross_section', raw_prior_direction=-1,
         citation='Asness, Frazzini, Gormsen and Pedersen (2020, JFE) scaled MAX with MAX5; Bali, Cakici and Whitelaw '
                  '(2011, JFE) MAX',
         prior_sign_source='Asness-Frazzini-Gormsen-Pedersen 2020; Bali-Cakici-Whitelaw 2011',
         formula='mean of the 5 largest daily returns over 21 sessions (ts_topk_mean) / 21-session volatility',
         form='R(decay_linear(x, 21))', smoothing=SMOOTHING,
         domain='non-positive 21-session volatility -> NaN (house guard); any NaN return in the 21-session window -> '
                'NaN (ts_topk_mean full window)',
         deviation='none against AFGP (MAX5 over the last month / volatility over the last month); smax (MAX1 / '
                   '252-session vol) stays byte-identical; with a 21-session denominator smax5 is a shape (lottery) '
                   'measure, not a volatility level',
         prior='AFGP (2020 JFE): SMAX returns positive and robust to BAB (US); Bali-Cakici-Whitelaw (2011 JFE): '
               'high-minus-low MAX deciles differ by more than 1%/mo, 1962-2005',
         expected_turnover='moderate (.06-.09/day [est]); max expected abs(rho) .55-.65 with smax [est]'),
    dict(id='res_mom_ind', theme='price_momentum', tier='B', ranking='cross_section', raw_prior_direction=1,
         citation='Blitz, Huij and Martens (2011, JEF) residual momentum; Blitz, Hanauer and Vidojevic (2020, JEF) '
                  'idiosyncratic momentum; FF49 industry residual (loading 1)',
         prior_sign_source='Blitz-Huij-Martens 2011',
         formula='mean / sd of the FF49 industry-excess daily return ret - group_mean(ret, grp_ff49) over t-251..'
                 't-21 (ts_mean_mp / ts_std_mp, window 231, min periods 116)',
         form='R(decay_linear(x, 21))', smoothing=SMOOTHING,
         domain='NaN below 116 finite industry-excess returns in the 231-session window (ts_mean_mp / ts_std_mp '
                'max(m, 2)); a flat window gives sd 0 (ts_std_mp) and a non-finite ratio (the registered string has '
                'no guard; unreachable in practice over >= 116 daily returns); NaN FF49 label -> NaN excess return',
         deviation='BHM use FF3 residuals with 36-month betas, beyond the bound; W2 ts_resid_on fits an in-window '
                   'intercept, which absorbs the formation drift, so it cannot express BHM within 314 bars; here the '
                   'residual of a one-factor industry model with loading 1; min periods m = 116 = ceil(w / 2), the '
                   'house convention of sv_ratio126 (63 of 126), so a membership gap does not blank the window',
         prior='Blitz-Huij-Martens (2011 JEF), 1930-2009: about 2x the risk-adjusted profit of total-return momentum; '
               'Blitz-Hanauer-Vidojevic (2020) confirm it for idiosyncratic momentum',
         expected_turnover='low (~.03/day [est]); max expected abs(rho) .65-.80 with res_mom_12_1, within_ind_mom '
                           '[est]'),
]
THEME_DESCRIPTIONS = {
    'investment_issuance': ('Low asset growth, low net operating assets and low share issuance (XBRL and split-neutral '
                            'vendor), and high expected investment growth (Hou-Mo-Xue-Zhang Eg: published slopes on '
                            'log q, cash profitability and change in ROE), ranked within FF12 industry; low investment '
                            'and issuance and high expected growth predict higher returns.'),
    'earnings_momentum': ('Earnings news: SUE, change in ROE, change in tax expense, the 3-day earnings announcement '
                          'return and the streak of year-on-year quarterly earnings increases; good news predicts '
                          'continuation.'),
    'price_momentum': ('12-1 momentum, 12-1 residual momentum, industry 12-1 momentum, within-industry momentum, '
                       'nearness to the 52-week high and standardized 12-1 FF49-industry-residual momentum; winners '
                       'continue.'),
    'low_risk': ('Betting against correlation (low correlation with the market), low scaled MAX (largest daily return '
                 'per unit of volatility), low MAX5 per unit of one-month volatility and QMJ safety (low beta, low '
                 'leverage, low ROE volatility); low risk predicts higher risk-adjusted returns.'),
}
PROMOTION_TESTS = [
    'P1 admission v4-prior-v1 (unchanged) on the restricted TRAIN role lo1: prior sign +1, reject below 250 finite '
    'TRAIN days or tau above .70, veto on HAC t below -2.0 (NW lag 5), redundancy greedy at |rho| <= .90 by '
    '(tier_rank, roster_order) against every admitted member, the 39 v6.1 members included (their status unchanged)',
    'P2 book: library v7.0 x ew-theme-v1 refit on lo1 x the v6.1 final construction (aim-partial-v5 theta .05 dust .1 '
    'fixed, delta orders, exit .05, locate-in-aim, liquidity cache, price-risk-v1) with L FIXED at 1.247; paired S2 '
    'net dSR vs mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 must be > 0 (sign-only inside one SE; Memmel '
    'SE and LW CBB reported)',
    'P3 R6\' mechanics on that cell (all-rows gross in [.90, 1.05], |mean net| <= .02, tau mean <= .20 / p95 <= .30) '
    'and S2 net >= 1.0; freeze still needs cell-count DSR >= .95 (N 34), effective-N DSR (ONC) reported beside it',
    'All-or-nothing: otherwise wave 1 is rejected whole, the draft\'s ranking is not re-tuned on TRAIN, and admitted-'
    'but-losing members are disclosed, not dropped one by one',
]


def _load(name: str, file: str) -> ModuleType:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    assert spec is not None and spec.loader is not None, file
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


@functools.cache
def pinned_v61() -> tuple[ModuleType, bytes, bytes]:
    """The v6.1 generator, its library and recipe bytes re-derived and checked against the pins and the files."""
    module = _load('_frozen_generate_fund_ic_v61_for_v70', V61_GENERATOR)
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V61_LIBRARY], docs[V61_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V61_LIBRARY_SHA256, 'frozen v6.1 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V61_RECIPE_SHA256, 'frozen v6.1 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return module, library_bytes, recipe_bytes


def v4() -> ModuleType:
    """The pinned v4 validator copy the v6.1 generator uses (registry rows power / ts_count_nans); not modified."""
    return pinned_v61()[0].v4()


def field_table() -> dict:
    """The v6.1 field table (v4 fields plus sv_ratio126); wave 1 adds no field."""
    return pinned_v61()[0].field_table()


def registry() -> dict:
    return dict(v4().REGISTRY, **V70_REGISTRY_ROWS)


def parse(text: str, fields: dict | None = None):
    """The v4 grammar + dtype rule extended by V70_REGISTRY_ROWS -> (node, shape, dtype, prior bars, native prior
    bars, rendered text), the v4 parse tuple. Old rows keep the v4 rules verbatim (asserted on every v6.1 string)."""
    m = v4()
    table = field_table() if fields is None else fields
    reg = registry()
    shift, rolling = m.SHIFT_OPS, m.ROLLING_OPS
    tokens, i = m.grammar().tokenize(text), 0

    def take(expected: str | None = None) -> str:
        nonlocal i
        if i >= len(tokens) or (expected is not None and tokens[i] != expected):
            raise ValueError(f'expected {expected!r} at token {i}: {text!r}')
        i += 1
        return tokens[i - 1]

    def numeric(arg, what: str) -> None:
        if arg[2] != 'f64':
            raise ValueError(f'{what} requires a numeric (f64) operand, got a Group classifier: {text!r}')

    def int_literal(arg, what: str) -> int:
        node = arg[0]
        if node[0] != 'num' or node[1] < 1 or node[1] != int(node[1]):
            raise ValueError(f'{what} must be a positive integer literal: {text!r}')
        return int(node[1])

    def primary():
        tok = take()
        if tok == '(':
            left = primary()
            op = take()
            if op not in '+-*/':
                raise ValueError(f'bad operator {op!r}')
            right = primary()
            take(')')
            numeric(left, 'arithmetic')
            numeric(right, 'arithmetic')
            shape = 'panel' if 'panel' in (left[1], right[1]) else 'scalar'
            return (('bin', op, left[0], right[0]), shape, 'f64', max(left[3], right[3]), max(left[4], right[4]),
                    f'({left[5]} {op} {right[5]})')
        if tok == '-':  # native parser folds a negated literal into one literal
            tok = take()
            if not tok[0].isdigit():
                raise ValueError('unary minus only on literals here')
            return (('num', -float(tok)), 'scalar', 'f64', 0, 0, f'-{tok}')
        if tok[0].isdigit():
            if not math.isfinite(float(tok)):
                raise ValueError('non-finite literal')
            return (('num', float(tok)), 'scalar', 'f64', 0, 0, tok)
        if i < len(tokens) and tokens[i] == '(':
            take('(')
            args = [primary()]
            while i < len(tokens) and tokens[i] == ',':
                take(',')
                args.append(primary())
            take(')')
            if tok not in reg:
                raise ValueError(f'unregistered operator {tok!r}')
            low, high, opcode = reg[tok]
            if not low <= len(args) <= high:
                raise ValueError(f'arity {len(args)} for {tok}')
            lookback, native = max(a[3] for a in args), max(a[4] for a in args)
            if opcode in m.GROUP_OPS:
                numeric(args[0], tok)
                if args[0][1] == 'scalar':
                    raise ValueError(f'{tok} primary is scalar')
                if args[1][2] != 'group':
                    raise ValueError(f'{tok} requires a Group classifier 2nd argument: {text!r}')
            else:
                for a in args:
                    numeric(a, tok)
            if opcode in LIT_TS_COUNT:  # (x, w, n): n peeled; window = the 2nd operand; n in [1, w]
                w, n = int_literal(args[1], f'{tok} window'), int_literal(args[2], f'{tok} count')
                if args[0][1] == 'scalar':
                    raise ValueError(f'{tok} series operand is scalar')
                if n > w:
                    raise ValueError(f'{tok} count must be in [1, w]: {text!r}')
                lookback, native = args[0][3] + w - 1, args[0][4] + w - 1
            elif opcode in LIT_TS_LAST:  # (y, x, w), one regressor: w >= k + 2 = 3
                w = int_literal(args[-1], f'{tok} window')
                if any(a[1] == 'scalar' for a in args[:-1]):
                    raise ValueError(f'{tok} series operand is scalar')
                if w < 3:
                    raise ValueError(f'{tok} window must be >= regressor count + 2: {text!r}')
                lookback, native = lookback + w - 1, native + w - 1
            elif opcode in shift | rolling:
                node = args[-1][0]
                if node[0] != 'num' or node[1] < 1 or node[1] != int(node[1]):
                    raise ValueError(f'{tok} window must be a positive integer literal')
                if any(a[1] == 'scalar' for a in args[:-1]):
                    raise ValueError(f'{tok} series operand is scalar')
                extra = int(node[1]) - (0 if opcode in shift else 1)
                lookback, native = lookback + extra, native + extra
            elif opcode.startswith('Cs') and args[0][1] == 'scalar':
                raise ValueError(f'{tok} primary is scalar')
            rendered = f"{tok}({', '.join(a[5] for a in args)})"
            return (('call', opcode, *(a[0] for a in args)), 'panel', 'f64', lookback, native, rendered)
        if tok not in table:
            raise ValueError(f'undeclared field {tok!r}')
        field = table[tok]
        if field.group != m.is_group_field(tok):
            raise ValueError(f'field {tok!r} group declaration disagrees with is_group_field')
        return (('field', tok), 'panel', 'group' if field.group else 'f64', field.prior_bars, 0, tok)

    result = primary()
    if i != len(tokens) or result[5] != text:
        raise ValueError(f'trailing tokens or non-canonical text: {text!r}')
    if result[2] != 'f64':
        raise ValueError(f'a Group classifier cannot be a signal root: {text!r}')
    return result


def validate(cid: str, dsl: str) -> dict:
    """Static budget (draft 0): <= 314 prior bars, <= 7 estimated peak slots (qmj_safety 8), <= 5 extra fields
    (q5_eg 6); both exceptions are root rulings recorded in v7-prereg.md."""
    m = v4()
    tree, shape, _, lookback, native, text = parse(dsl)
    nodes, slots = m.grammar().peak_slots(tree)
    assert text == dsl and shape == 'panel' and lookback <= m.MAX_PRIOR_BARS, (cid, lookback)
    assert native <= lookback and nodes <= m.MAX_SLOTS, (cid, nodes)
    assert slots <= SLOTS_EXCEPTIONS.get(cid, MAX_SLOTS_PER_CANDIDATE), (cid, slots)
    assert len(dsl.encode()) <= m.MAX_DSL_BYTES, cid
    fields = sorted(m.fields_of(tree))
    extras = [f for f in fields if f not in m.BASE_FIELDS]
    assert len(extras) <= EXTRAS_EXCEPTIONS.get(cid, MAX_EXTRAS_PER_CANDIDATE), (cid, extras)
    return dict(prior_bars=lookback, native_prior_bars=native, dag_nodes=nodes, estimated_peak_slots=slots,
                fields=fields, extra_fields=extras)


def registry_crosscheck(engine_root: Path | None = None) -> str:
    """The six added rows against registry.cpp (name, arity, OpCode, peeled count) and registry.hpp (W2 classes)."""
    engine = (HERE.parent.parent if engine_root is None else Path(engine_root)) / 'atx-engine'
    paths = [engine / 'src/alpha/registry.cpp', engine / 'include/atx/engine/alpha/registry.hpp']
    if not all(p.is_file() for p in paths):
        return 'v7.0 registry cross-check skipped (engine sources absent)'
    source, header = (p.read_text(encoding='utf-8') for p in paths)
    rows = {mm[1]: (int(mm[2]), int(mm[3]), mm[4], int(mm[5] or 0)) for mm in re.finditer(
        r'\{"(\w+)",\s*(\d+),\s*(\d+),\s*OpCode::(\w+),[^{}]*?\{[^{}]*\},\s*&shape_\w+(?:,\s*(\d+))?', source)}
    lit_body = re.search(r'is_lit_op\(OpCode op\) noexcept \{(.*?)\n\}', header, re.S)
    mp_body = re.search(r'is_lit_mp_op\(OpCode op\) noexcept \{(.*?)\n\}', header, re.S)
    if lit_body is None or mp_body is None:
        raise SystemExit('registry.hpp is_lit_op / is_lit_mp_op not found')
    lit, mp = set(re.findall(r'OpCode::(\w+)', lit_body[1])), set(re.findall(r'OpCode::(\w+)', mp_body[1]))
    for name, (low, high, opcode) in V70_REGISTRY_ROWS.items():
        row = rows.get(name)
        if row is None or row[2] != opcode or not row[0] <= low <= high <= row[1]:
            raise SystemExit(f'registry row differs for {name}: {row} vs {(low, high, opcode)}')
        peeled = 1 if opcode in LIT_TS_COUNT else 0
        if row[3] != peeled:
            raise SystemExit(f'peeled hyperparameter count differs for {name}: {row[3]} != {peeled}')
        if (opcode in LIT_OPS) != (opcode in lit) or ((opcode in {'TsMeanMp', 'TsStdMp'}) != (opcode in mp)):
            raise SystemExit(f'W2 op class differs for {name}')
    return f'v7.0 registry cross-check ok ({len(V70_REGISTRY_ROWS)} added rows vs registry.cpp/registry.hpp)'


def member_dsl(cid: str) -> str:
    """The draft string, with q5_eg's three slopes respelled to the RoF 2021 values (one literal each)."""
    text = DRAFT_DSL[cid]
    if cid == 'q5_eg':
        for _, old, new in Q5_SLOPES:
            assert text.count(old) == 1, old
            text = text.replace(old, new)
    return text


def wrap(x: dict, base: str) -> str:
    """The house smoothing wrapper R(decay_linear(x, 21)), R = group_rank(., grp_ff12) within FF12 (rule R1)."""
    inner = f'decay_linear({base}, {SMOOTHING})'
    return (f'group_rank({inner}, {v4().WITHIN_INDUSTRY})' if x['ranking'] == 'within_industry_grp_ff12'
            else f'rank({inner})')


def base_of(x: dict, dsl: str) -> str | None:
    """The member's x inside R(decay_linear(x, 21)); None for the three-leg composite (as value_composite)."""
    if x['form'].startswith('mean_k'):
        return None
    head = 'group_rank(decay_linear(' if x['ranking'] == 'within_industry_grp_ff12' else 'rank(decay_linear('
    tail = f', {SMOOTHING}), {v4().WITHIN_INDUSTRY})' if head.startswith('group') else f', {SMOOTHING}))'
    assert dsl.startswith(head) and dsl.endswith(tail), x['id']
    base = dsl[len(head):-len(tail)]
    assert wrap(x, base) == dsl, x['id']
    return base


def documents() -> dict[str, bytes]:
    m = v4()
    _, library_v61_bytes, recipe_v61_bytes = pinned_v61()
    library_v61, recipe_v61 = json.loads(library_v61_bytes), json.loads(recipe_v61_bytes)
    assert len(library_v61['candidates']) == V61_CANDIDATES
    table = field_table()
    for c in library_v61['candidates']:  # the extended parser is the v4 parser on every v6.1 string
        assert parse(c['dsl']) == m.parse(c['dsl'], table), c['id']
    declared = {f['name'] for f in library_v61['fields']}
    template_keys = list(recipe_v61['templates'][0])
    lineage_keys = list(recipe_v61['lineage'][0])
    new_candidates, templates, lineage, checks = [], [], [], {}
    for k, x in enumerate(MEMBERS):
        dsl = member_dsl(x['id'])
        check = validate(x['id'], dsl)
        checks[x['id']] = check
        assert set(check['fields']) <= declared, (x['id'], 'a wave-1 member reads a field v6.1 does not declare')
        assert x['theme'] in m.THEME_IDS and x['tier'] in m.TIER_RANK, x['id']
        within = x['theme'] in m.WITHIN_INDUSTRY_THEMES
        assert within == (x['ranking'] == 'within_industry_grp_ff12'), x['id']  # rule R1
        tree = parse(dsl)[0]
        if x['form'].startswith('mean_k'):  # (((R(.) + R(.)) + R(.)) / 3): three house ranks, no op on top
            assert tree[:2] == ('bin', '/') and tree[3] == ('num', 3.0), x['id']
        else:
            assert tree[:2] == ('call', 'CsRankG' if within else 'CsRank'), x['id']
        base = base_of(x, dsl)
        labels = dict(theme=x['theme'], tier=x['tier'], tier_rank=m.TIER_RANK[x['tier']], prior_sign=1,
                      citation=x['citation'])
        candidate = dict(id=x['id'], family=x['theme'], dsl=dsl, horizons=[5, 21, 63],
                         sign_policy='train-rank-ic21', **labels)
        assert list(candidate) == list(library_v61['candidates'][0]), 'candidate key order differs from v6.1'
        new_candidates.append(candidate)
        base_lookback = parse(base)[3] if base is not None else None
        template = dict(id=x['id'], theme=x['theme'], formula=x['formula'], base_dsl=base,
                        base_prior_bars=base_lookback, raw_prior_direction=x['raw_prior_direction'],
                        domain=x['domain'], deviation=x['deviation'], smoothing_form=x['form'],
                        smoothing_sessions=x['smoothing'])
        assert list(template) == template_keys
        templates.append(template)
        row = dict(id=x['id'], family=x['theme'], roster_order=V61_CANDIDATES + k + 1, **labels,
                   raw_prior_direction=x['raw_prior_direction'], prior_sign_source=x['prior_sign_source'],
                   ranking=x['ranking'], smoothing_form=x['form'], smoothing_sessions=x['smoothing'],
                   prior_bars=check['prior_bars'], native_prior_bars=check['native_prior_bars'],
                   dsl_sha256=hashlib.sha256(dsl.encode()).hexdigest(), fields=check['fields'],
                   v51_change='added in v7.0 (wave 1)')
        assert list(row) == lineage_keys
        lineage.append(row)
    candidates = library_v61['candidates'] + new_candidates
    ids = [c['id'] for c in candidates]
    assert len(ids) == len(set(ids)) == V61_CANDIDATES + len(MEMBERS) <= MAX_ROSTER
    assert len({c['dsl'] for c in candidates}) == len(candidates)
    assert len(library_v61['families']) <= m.MAX_FAMILIES
    families = [dict(f, description=THEME_DESCRIPTIONS[f['id']]) if f['id'] in THEME_DESCRIPTIONS else f
                for f in library_v61['families']]
    assert {x['theme'] for x in MEMBERS} == set(THEME_DESCRIPTIONS)
    library = dict(schema=library_v61['schema'], id=LIBRARY_ID, fields=library_v61['fields'], families=families,
                   candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    # the 39 v6.1 entries are a byte-identical prefix of the v7.0 candidate list (same encoder, same depth)
    region = lambda blob: blob.split(b'"candidates": [\n', 1)[1]
    assert region(library_bytes).startswith(region(library_v61_bytes)[:-len(b'\n  ]\n}\n')] + b',\n')

    new_ids = [x['id'] for x in MEMBERS]
    themes = []
    for t in recipe_v61['themes']:
        added = [x for x in MEMBERS if x['theme'] == t['theme']]
        if added:
            t = dict(t, description=THEME_DESCRIPTIONS[t['theme']], members=t['members'] + [x['id'] for x in added],
                     expected_turnover_v70={x['id']: x['expected_turnover'] for x in added})
        themes.append(t)
    within6 = recipe_v61['within_industry']
    within = dict(within6, members=within6['members'] + [x['id'] for x in MEMBERS
                                                         if x['ranking'] == 'within_industry_grp_ff12'])
    price6 = recipe_v61['industry_level_price_members']
    price = dict(price6, group_fields=dict(price6['group_fields'], grp_ff49=price6['group_fields']['grp_ff49'] +
                                           ['res_mom_ind']),
                 v70='res_mom_ind: FF49 industry-excess return (group_mean) under min-periods ts ops')
    needs = []
    for row in recipe_v61['needs_new_field']:
        if row['hypothesis'].startswith('SMAX with MAX5'):
            row = dict(row, v70='resolved: W2 op ts_topk_mean (platform v7); member smax5; smax (MAX1) kept '
                                'byte-identical')
        needs.append(row)
    assert sum('v70' in r for r in needs) == 1
    sv = recipe_v61['static_validation']
    users = dict(sv['extra_field_users'])
    for x in MEMBERS:
        for f in checks[x['id']]['extra_fields']:
            users[f] = users.get(f, []) + [x['id']]
    static = dict(
        sv, rule=sv['rule'] + '; v7.0 adds registry rows sign, max and the W2 ops ts_beta_on (one regressor), '
                              'ts_topk_mean, ts_mean_mp, ts_std_mp (window second, count peeled; lookback (w - 1) + '
                              'child per typecheck analyze_lit_call), cross-checked against registry.cpp / '
                              'registry.hpp; no field',
        max_prior_bars=max(sv['max_prior_bars'], *(c['prior_bars'] for c in checks.values())),
        max_native_prior_bars=max(sv['max_native_prior_bars'], *(c['native_prior_bars'] for c in checks.values())),
        max_dag_nodes=max(sv['max_dag_nodes'], *(c['dag_nodes'] for c in checks.values())),
        max_estimated_peak_slots=max(sv['max_estimated_peak_slots'],
                                     *(c['estimated_peak_slots'] for c in checks.values())),
        extra_field_capacity=max(sv['extra_field_capacity'], *(len(c['extra_fields']) for c in checks.values())),
        max_extras_per_candidate_exceptions=dict(EXTRAS_EXCEPTIONS),
        max_extras_exception_basis='prereg ruling (library-v7-draft 3.5a; v7-prereg.md "Library v7.0" and its '
                                   'Correction): q5_eg reads six fields-v7 fields, one over the per-candidate budget; '
                                   'no new field',
        max_estimated_peak_slots_exceptions=dict(SLOTS_EXCEPTIONS),
        max_estimated_peak_slots_exception_basis=(
            'v7-prereg.md Ruling 7.0-b (2026-09-28, declared before any run): the verbatim qmj_safety string needs 8 '
            'estimated peak slots against the house budget of 7, which under draft section 0 would withdraw it; '
            'allowed as this recorded exception instead (the top-ranked draft candidate; u-pass cost about +52 MB '
            'per slot per worker, headroom 1,061 MiB peak vs the 1,536 MiB cap); wave 1 stays at 5 candidates / 5 '
            'admission trials'),
        extra_field_users=dict(sorted(users.items())), registry_rows_v70=V70_REGISTRY_ROWS, v70=checks)
    data61 = recipe_v61['data']
    data = dict(data61, requires=dict(data61['requires'], engine_w2=(
        f'an IC / NAV exe built at or after the W2 merge {W2_MERGE} (literature_ops ts_beta_on, ts_topk_mean, '
        'ts_mean_mp, ts_std_mp; dsl_vm_semantics_version unchanged, so v6.1 cache entries stay valid)'),
        fields_v7_only='wave 1 reads fields-v7 only: fields v8 is not built for v7.0 (every field a wave-1 member '
                       'reads is already declared by v6.1)'))
    hygiene = ('v7.0 appends exactly the five pre-registered wave-1 members (v7-prereg.md "Library v7.0", draft '
               f'sections 0-1 at {DRAFT_COMMIT}) implemented literally: DSL strings verbatim, q5_eg slopes respelled '
               'to the RoF 2021 values per the registered root check; the implementer read no TRAIN return '
               'statistic, no validation statistic and no data dated 2023 or later, and did not relate any member to '
               'returns. The 39 v6.1 entries are byte-identical.')
    gen61 = recipe_v61['generation']
    order = {x['id']: V61_CANDIDATES + k + 1 for k, x in enumerate(MEMBERS)}
    recipe = dict(
        schema=recipe_v61['schema'], id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(rule='v61-plus-wave1-v70', revision='initial', candidates=len(candidates),
                        families=gen61['families'], family_is_theme=True, one_variant_per_hypothesis=True,
                        max_roster=MAX_ROSTER,
                        wrapper=('v6.1 members byte-identical; wave 1 takes the v6 fixed form R(decay_linear(x, 21)) '
                                 '(qmj_safety: mean_k R(decay_linear(x_k, 21)), the value_composite idiom), R = '
                                 'group_rank(., grp_ff12) for q5_eg (investment_issuance, rule R1) and rank(.) '
                                 'otherwise; DSL text verbatim from the draft'),
                        appended=new_ids, roster_order=order, v61_wrapper=gen61['wrapper'],
                        max_prior_bars=gen61['max_prior_bars'], complete_observations=gen61['complete_observations'],
                        grammar_helpers=gen61['grammar_helpers'],
                        draft=dict(path='.superpowers/sdd/platform-20260928/library-v7-draft.md', commit=DRAFT_COMMIT,
                                   sections='0 (conventions), 1 (wave 1)'),
                        parent_v61=dict(generator=V61_GENERATOR, library=V61_LIBRARY,
                                        library_sha256=V61_LIBRARY_SHA256, recipe=V61_RECIPE,
                                        recipe_sha256=V61_RECIPE_SHA256, candidates=V61_CANDIDATES,
                                        generator_sha256=gen61['generator_sha256']),
                        generator_sha256=hashlib.sha256(
                            Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest()),
        orientation=recipe_v61['orientation'],
        labels=dict(recipe_v61['labels'], tier_basis_v70='wave-1 tiers from library-v7-draft section 1 (v6-literature '
                                                         'scale): qmj_safety B, nincr C+, q5_eg B, smax5 B+, '
                                                         'res_mom_ind B'),
        themes=themes,
        within_industry=within,
        industry_level_price_members=price,
        industry_demeaned_members=recipe_v61['industry_demeaned_members'],
        v6_changes=recipe_v61['v6_changes'],
        v61_changes=recipe_v61['v61_changes'],
        v70_changes=[dict(id=x['id'], change='added', detail=f"prereg v7.0 wave 1 rank {k + 1}: theme {x['theme']}, "
                                                              f"tier {x['tier']}, raw prior direction "
                                                              f"{x['raw_prior_direction']:+d} (embedded)")
                     for k, x in enumerate(MEMBERS)],
        literature=dict(priors={x['id']: x['prior'] for x in MEMBERS}, q5_eg_slopes=Q5_SLOPE_SOURCES),
        needs_new_field=needs,
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene,
                           v61_statement=recipe_v61['family_fixing']['statement'],
                           v6_statement=recipe_v61['family_fixing']['v6_statement']),
        templates=recipe_v61['templates'] + templates,
        lineage=recipe_v61['lineage'] + lineage,
        data=data,
        admission=dict(policy='v4-prior-v1 (T23; prereg R3; v7-prereg "Library v7.0": unchanged)', trials=len(MEMBERS),
                       trials_basis='v7-prereg: 5 admission trials (only the wave-1 members are new; the 39 v6.1 '
                                    'members are identical definitions on identical data and keep their status)',
                       v61_policy=recipe_v61['admission']['policy']),
        composition=dict(policy='v7-prereg "Library v7.0": ew-theme-v1 refit on lo1 (+1 composition), unchanged rule',
                         themes='the 9 v6.1 families; ownership_flow is appended to the fitter\'s theme list but no '
                                'v7.0 member carries it'),
        promotion_tests=PROMOTION_TESTS,
        trials=dict(generated_candidates=len(candidates), admission_trials=len(MEMBERS), new_candidates=len(MEMBERS),
                    unchanged_candidates=V61_CANDIDATES, orientation_fits=0, composition_recipes=1,
                    construction_recipes=1, variants_per_hypothesis=1, dsr_n=34, family_or_template_selection=False,
                    validation_selection=False),
        static_validation=static,
        qualification=dict(native_parse_vm='pending: root u pass compile against fields-v7 on a W2 exe',
                           empirical='unmeasured at source freeze',
                           prior_phase='v1-v6.1 files and generators are unchanged by v7.0'))
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    docs = documents()
    for name, expected in docs.items():
        path = HERE / name
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f'fixed artifact differs: {name}')
        else:
            path.write_bytes(expected)
        print(f'{name} {hashlib.sha256(expected).hexdigest()} {len(expected)} bytes')
    print(registry_crosscheck())
    library = json.loads(docs[LIBRARY])
    recipe = json.loads(docs[RECIPE])
    print(f"candidates {len(library['candidates'])}: v6.1 {V61_CANDIDATES} byte-identical + "
          f"{[c['id'] for c in library['candidates'][V61_CANDIDATES:]]}; generator sha256 (LF) "
          f"{recipe['generation']['generator_sha256']}")
    for cid, c in recipe['static_validation']['v70'].items():
        print(f"  {cid}: prior bars {c['prior_bars']}, dag nodes {c['dag_nodes']}, peak slots "
              f"{c['estimated_peak_slots']}, extras {len(c['extra_fields'])} {c['extra_fields']}")


if __name__ == '__main__':
    main()
