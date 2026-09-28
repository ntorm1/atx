"""Deterministic 38-candidate research library v6 = library v5.1 revised by prereg v6 V6-L; no data read.

Library v6 (task V6-L) implements the v4 pre-registration section "## v6 revision", item V6-L, as specified by
the task brief (.superpowers/sdd/mega-alpha-20260926/task-V6L-brief.md): library v5.1 (fund_industry_ic_v5.json,
38 candidates, pinned by SHA-256 and re-derived from its own generator) with literature-driven member upgrades,
one variant per hypothesis, every candidate prior-signed (sign embedded in the DSL, prior_sign +1; the raw
direction and its author-year source are in PRIOR_SIGN_SOURCES below). Changes against v5.1:

  added     value_composite  value                  mean within-FF12 rank of B/M, E/P, CF/P (ebit_ev: budget)
  added     res_mom_12_1     price_momentum         Blitz, Huij and Martens (2011) residual momentum, 12-1
  replaced  cfoa -> cbop     profitability_quality  Ball, Gerakos, Linnainmaa and Nikolaev (2016) cash-based OP
  replaced  low_beta -> bac  low_risk               Asness, Frazzini, Gormsen and Pedersen (2020) betting against
                                                    correlation
  replaced  low_max -> smax  low_risk               scaled MAX (Asness et al. 2020; Bali, Cakici, Whitelaw 2011)
  removed   low_ivol         low_risk               replaced by bac / smax (prereg V6-L)
  removed   lowvol_ind       low_risk               spanned by the price-risk-v1 vol63 regressor (LOWVOL_IND_ARGUMENT)

Smoothing (brief, controller ruling; the v6 code review I3 / I5):
  * fast sleeves (standalone TRAIN tau >= 0.08 in build-equity/mega-weights-v51-ew/admission.json, a turnover
    statistic, never a return statistic): the 21-session decay is removed, R(x); construction theta smooths.
  * otherwise, decay_linear(R(x), 21) becomes R(decay_linear(x, 21)) ONLY where the 21-session blackout argument
    applies: R is a member-masked cross-sectional op (vm.hpp set_cross_section_mask: every Cs op emits NaN for
    non-members) and ts ops are full-window any-NaN -> NaN (ts_ops.hpp), so decay_linear(R(x), 21) is NaN for 21
    sessions after any membership gap. Moving the decay inside R removes the blackout iff x itself holds no Cs op;
    members whose x holds one (ind_mom_12_1: group_mean; within_ind_mom: group_neutralize) keep the blackout under
    either order and are left byte-identical.
  * additions take the fixed form R(decay_linear(x, 21)) (the s21 default; they have no measured tau).

Not expressible with the fields-v6 manifest, the 314-bar lookback bound and the IC-runner plan budget (<= 5
extra fields, <= 7 slots per candidate): see NEEDS_NEW_FIELD (Heston-Sadka lags 24/36, FINRA daily short volume,
SG&A for intangible value, 5-year composite issuance, XFIN, ebit_ev in the composite). EAR-centred earnings
momentum already exists as `ear` (the [-1, +1] return around the vendor reaction session, carried): kept.

The static validator is v4's (a private copy of the pinned v4 generator) with two registry rows added
(power, ts_count_nans), each cross-checked against registry.cpp and the typecheck lookback
classes. The frozen v4, v4.2 and v5.1 generators and files are not modified. No TRAIN return statistic and no
2023+ data were read to choose any definition, sign, window or roster position.

Run with --check to verify the committed exact JSON bytes.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V4_GENERATOR = 'generate_fund_ic_v4.py'
V5_GENERATOR = 'generate_fund_ic_v5.py'
V5_LIBRARY = 'fund_industry_ic_v5.json'
V5_RECIPE = 'fund_industry_ic_v5.recipe.json'
V5_LIBRARY_SHA256 = '9e5ea08cb3c9a802f72e23dd069499b59294fba5708e0873d9dfc57f8a7458e0'
V5_RECIPE_SHA256 = 'a26670b0f7b6d1681a4ea8a4758da83841163ceaea8af5a4256ee718ad7c3aea'
V5_CANDIDATES = 38
LIBRARY = 'fund_industry_ic_v6.json'
RECIPE = 'fund_industry_ic_v6.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v6'
PREREG = ('.superpowers/sdd/mega-alpha-20260926/v4-prereg.md section "## v6 revision" item V6-L (declared 2026-09-27 '
          'after the Phase A reviews, before any v6 TRAIN read); task brief task-V6L-brief.md')
MAX_ROSTER = 48               # brief: roster size <= 48
MAX_EXTRAS_PER_CANDIDATE = 5  # IC runner plan budget (controller ruling T27; kept by v5.1 R-b)
MAX_SLOTS_PER_CANDIDATE = 7   # the v4/v5.1 library maximum (max_compiled_slots does not grow)
SMOOTHING = 21
FAST_TAU = 0.08               # brief: fast sleeve = standalone TRAIN tau >= 0.08 (v51-ew admission)
ADMISSION_V51 = 'build-equity/mega-weights-v51-ew/admission.json'
ADMISSION_V51_SHA256 = '4f06bd18537a3c2ff040d62a43708c8f6fd1144b7e94bc354237b9ae9f0bd37c'
# Standalone TRAIN one-way daily turnover (tau) of the v5.1 members at or above FAST_TAU, transcribed from
# ADMISSION_V51 (id and tau only were read; tau is a turnover statistic). Every other v5.1 member has tau < 0.08.
FAST_SLEEVE_TAU = {'ind_adj_rev_5': 0.1808, 'seasonality_same_month': 0.0938, 'si_change': 0.0915,
                   'iv_rv_spread': 0.0898, 'low_max': 0.0887}
RES_MOM_WINDOW = 231          # 12-1: returns t-251..t-21 (as v4.2 res_mom_12_1)
BAC_WINDOW = 250              # correlation of overlapping 3-day returns (Frazzini-Pedersen estimator; bound 314)
MAX_WINDOW = 21               # one month of daily returns (Bali-Cakici-Whitelaw)
VOL_WINDOW = 252              # one year of daily volatility (SMAX scaling)
V6_REGISTRY_ROWS = {'power': (2, 2, 'Pow'), 'ts_count_nans': (2, 2, 'TsCountNans')}
V6_ROLLING_OPS = {'TsCountNans'}

FORM_RANK_OF_DECAY = 'R(decay_linear(x, 21))'
FORM_DECAY_OF_RANK = 'decay_linear(R(x), 21)'
FORM_RANK = 'R(x)'
FORM_COMPOSITE = 'mean_k R(decay_linear(x_k, 21))'

# Roster edits (literature-driven; positions never chosen by a TRAIN statistic). A replacement takes the slot of
# the member it replaces; an addition that the literature ranks as the preferred construction of a hypothesis
# cluster is inserted directly before the incumbent it upgrades, so it precedes it in the (tier, roster order)
# redundancy pass (prereg R3): value_composite before bm (Israel-Laursen-Richardson 2021: composites average out
# ratio noise; v6-literature 3.9), res_mom_12_1 before mom_12_1 (Blitz-Huij-Martens 2011; v6-literature 3.5).
INSERT_BEFORE = {'bm': 'value_composite', 'mom_12_1': 'res_mom_12_1'}
REPLACE = {'cfoa': 'cbop', 'low_beta': 'bac', 'low_max': 'smax'}
REMOVE = {'low_ivol': 'replaced by bac and smax (prereg V6-L: replace low_beta / low_ivol / low_max)',
          'lowvol_ind': 'spanned by the price-risk-v1 vol63 regressor (see LOWVOL_IND_ARGUMENT)'}

# Structural (return-free) argument for dropping lowvol_ind (brief: keep only if not spanned by vol63).
LOWVOL_IND_ARGUMENT = (
    'lowvol_ind = -(v - m) with v = sd(ret, 252) and m its FF12 mean. price-risk-v1 regresses the target on z(vol63) '
    '(and z(beta252), ladv63) and keeps the residual; cross-sectionally vol252 and vol63 measure the same level, so '
    'write v = m + w (between- and within-industry parts, w orthogonal to m). The residual of s = -w on v is '
    '-(var(m) / var(v)) w + (var(w) / var(v)) m. Within-industry dispersion is the larger part of cross-sectional '
    'volatility, so the prior-consistent -w part is mostly projected out and the surviving residual loads POSITIVELY '
    'on the industry-mean volatility m: a long-high-volatility-industry bet, the opposite of the low-risk prior '
    '(Asness, Frazzini and Pedersen 2014 find low risk works within and across industries). The window mismatch '
    '(252 vs 63) adds estimation noise, not a prior. Hence spanned: removed. Uses no return statistic.')

# Prior sign sources: id -> (raw prior direction, author-year of the sign). base = direction * raw quantity, so a
# higher DSL value is the predicted long side and every candidate carries prior_sign +1.
PRIOR_SIGN_SOURCES = {
    # ---- value: cheap (high fundamental / price) outperforms
    'value_composite': (1, 'Fama-French 1992; Lakonishok-Shleifer-Vishny 1994; Israel-Laursen-Richardson 2021'),
    'bm': (1, 'Rosenberg-Reid-Lanstein 1985; Fama-French 1992'),
    'ep': (1, 'Basu 1977'),
    'cfp': (1, 'Lakonishok-Shleifer-Vishny 1994'),
    'fcfp': (1, 'Lakonishok-Shleifer-Vishny 1994; Hackel-Livnat-Rai 1994'),
    'ebit_ev': (1, 'Loughran-Wellman 2011'),
    'net_payout': (1, 'Boudoukh-Michaely-Richardson-Roberts 2007'),
    'sp': (1, 'Barbee-Mukherji-Raines 1996'),
    'rd_me': (1, 'Chan-Lakonishok-Sougiannis 2001'),
    # ---- profitability_quality: profitable / high-quality outperforms; accruals negative
    'gpa': (1, 'Novy-Marx 2013'),
    'opbe': (1, 'Fama-French 2015'),
    'cbop': (1, 'Ball-Gerakos-Linnainmaa-Nikolaev 2016'),
    'roe_q': (1, 'Hou-Xue-Zhang 2015'),
    'roa': (1, 'Balakrishnan-Bartov-Faurel 2010'),
    'accruals': (-1, 'Sloan 1996'),
    'fscore': (1, 'Piotroski 2000'),
    'opex_at': (1, 'Novy-Marx 2011'),
    # ---- investment_issuance: high investment / issuance underperforms
    'asset_growth': (-1, 'Cooper-Gulen-Schill 2008'),
    'noa': (-1, 'Hirshleifer-Hou-Teoh-Zhang 2004'),
    'issuance_xbrl': (-1, 'Pontiff-Woodgate 2008'),
    'issuance_vendor': (-1, 'Daniel-Titman 2006'),
    # ---- earnings_momentum: good earnings news continues
    'sue': (1, 'Bernard-Thomas 1989'),
    'droe': (1, 'Hou-Mo-Xue-Zhang 2021'),
    'chtax': (1, 'Thomas-Zhang 2011'),
    'ear': (1, 'Chan-Jegadeesh-Lakonishok 1996; Brandt-Kishore-Santa-Clara-Venkatachalam 2008'),
    # ---- price_momentum: winners continue
    'res_mom_12_1': (1, 'Blitz-Huij-Martens 2011'),
    'mom_12_1': (1, 'Jegadeesh-Titman 1993'),
    'ind_mom_12_1': (1, 'Moskowitz-Grinblatt 1999'),
    'within_ind_mom': (1, 'Asness-Porter-Stevens 2000'),
    'high_52w': (1, 'George-Hwang 2004'),
    # ---- low_risk: high correlation / lottery-like stocks underperform
    'bac': (-1, 'Asness-Frazzini-Gormsen-Pedersen 2020'),
    'smax': (-1, 'Asness-Frazzini-Gormsen-Pedersen 2020; Bali-Cakici-Whitelaw 2011'),
    # ---- short_interest: heavily / increasingly shorted stocks underperform
    'si_ratio': (-1, 'Asquith-Pathak-Ritter 2005'),
    'dtc': (-1, 'Hong-Li-Ni-Scheinkman-Yan 2015'),
    'si_change': (-1, 'Rapach-Ringgenberg-Zhou 2016 (direction)'),
    # ---- reversal_seasonality: last week's industry-relative losers rebound; same-month winners repeat
    'ind_adj_rev_5': (-1, 'Da-Liu-Schaumburg 2014; Hameed-Mian 2015'),
    'seasonality_same_month': (1, 'Heston-Sadka 2008'),
    # ---- options_implied: implied above realized volatility predicts higher returns
    'iv_rv_spread': (1, 'Bali-Hovakimian 2009'),
}

THEME_DESCRIPTIONS = {
    'value': 'Price-scaled fundamentals (book, earnings, cash flow, free cash flow, EBIT/EV, net payout, sales, R&D) and '
             'a composite of the book, earnings and cash-flow yields, ranked within FF12 industry; high value predicts '
             'higher returns.',
    'profitability_quality': 'Profitability and quality (gross, operating, cash-based operating, ROE, ROA, low '
                             'accruals, F-score) and operating leverage (operating costs to assets), ranked within '
                             'FF12 industry; high quality and high operating leverage predict higher returns.',
    'price_momentum': '12-1 momentum, 12-1 residual momentum, industry 12-1 momentum, within-industry momentum and '
                      'nearness to the 52-week high; winners continue.',
    'low_risk': 'Betting against correlation (low correlation with the market) and low scaled MAX (largest daily '
                'return per unit of volatility); low risk predicts higher risk-adjusted returns.',
}
THEME_TURNOVER_V6 = {  # theme-level expected turnover where v6 changes it (else v5.1's text)
    'low_risk': 'low to moderate: 250-session correlation and 21-session MAX over 252-session volatility; s21 decay',
    'short_interest': 'very low for si_ratio / dtc (s21 decay); si_change unsmoothed (fast sleeve, theta smooths)',
    'reversal_seasonality': 'high (the fastest theme; cost-bound): weekly and one-month windows, unsmoothed (fast '
                            'sleeves; construction theta smooths)',
    'options_implied': 'high: daily IV and 21-session realized volatility, unsmoothed (fast sleeve; theta smooths)',
}
EXPECTED_TURNOVER_V6 = {
    'value_composite': 'low (as cfp, ~0.02/day): price-scaled ratios, s21 decay inside the rank',
    'cbop': 'very low (< 0.02/day, as gpa): accounting ranks step at filings; s21 decay',
    'res_mom_12_1': 'low (~0.03/day): 231-session standardized residual return, s21 decay',
    'bac': 'low (as low_beta, ~0.05/day or less): 250-session correlation, s21 decay',
    'smax': 'moderate (21-session MAX over 252-session volatility), s21 decay; measured at admission',
}

# Hypotheses of the brief that fields-v6, the 314-bar bound or the plan budget cannot express (no field invented).
NEEDS_NEW_FIELD = [
    dict(theme='reversal_seasonality', hypothesis='Heston-Sadka (2008) multi-lag seasonality: mean same-calendar-month '
         'return at annual lags 12, 24, 36 months',
         blocker='lags 24 and 36 need 504 and 756 prior sessions: above the 314-bar DSL lookback bound (the IC runner '
                 'admits lookback <= score_begin - 63 = 336 on TRAIN role v2) and before the role\'s first session '
                 '(2018-06-01) for the first scoring dates',
         needs='a producer field, e.g. seas_same_month_y2_y5 = mean of the same-calendar-month return at annual lags '
               '2..5 (point in time, from a long adjusted-close history), or a role with >= 820 warm-up sessions',
         v6='seasonality_same_month (lag 12) kept; no second seasonality variant'),
    dict(theme='short_interest', hypothesis='long-window FINRA shorting flow (Wang-Yan-Zheng 2020): 63-126 session '
         'mean of daily short-sale volume / volume',
         blocker='fields-v6 has FINRA short INTEREST (si_shares, si_dtc) only; no daily Reg SHO short-sale volume',
         needs='finra_short_volume: daily consolidated short-sale volume (FINRA TRF + exchange files), known after the '
               'session\'s publication, same share basis as volume; then -ts_mean(finra_short_volume / volume, 126)',
         v6='no candidate'),
    dict(theme='value', hypothesis='intangible-adjusted value (Eisfeldt-Kim-Papanikolaou 2022): (BE + intangible '
         'capital) / ME',
         blocker='no SG&A field (xsga_ttm) for organization capital; R&D knowledge capital needs a perpetual inventory '
                 'over ~10 years of R&D, beyond the 314-bar bound',
         needs='xsga_ttm and a producer intangible-capital field (R&D at 15% and 30% of SG&A at 20% depreciation)',
         v6='no candidate; rd_me kept'),
    dict(theme='investment_issuance', hypothesis='5-year composite issuance (Daniel-Titman 2006)',
         blocker='log(ME_t / ME_t-60m) - log total return needs 1260 prior sessions (bound 314; TRAIN data start '
                 '2018-06-01)',
         needs='a producer field ci_5y (or a 5-year shares-outstanding history)',
         v6='fallback "1-year net share issuance" already present as issuance_xbrl (Pontiff-Woodgate) and '
            'issuance_vendor (1-year Daniel-Titman composite issuance): no duplicate added'),
    dict(theme='investment_issuance', hypothesis='net external financing XFIN (Bradshaw-Richardson-Sloan 2006)',
         blocker='expressible only as group_rank(decay_linear(-((sstk_ttm - prstkc_ttm - dvc_ttm + debt - '
                 'delay(debt, 252)) / at), 21), grp_ff12) = 6 extra fields, above the plan budget of 5; debt '
                 'issuance / retirement flows are absent (balance-change proxy)',
         needs='a budget ruling (<= 6 extras) or producer fields dltis_ttm / dltr_ttm or xfin_ttm',
         v6='no candidate'),
    dict(theme='value', hypothesis='composite value including EBIT/EV (brief: mean rank of bm, ep, cfp, ebit_ev)',
         blocker='the four ratios need be, ni_ttm, cfo_ttm, oi_ttm, me_company, debt, che, grp_ff12 = 8 extra fields '
                 '(budget 5)',
         needs='a budget ruling or a producer enterprise-value field (ev_company)',
         v6='value_composite uses bm, ep, cfp (5 extras)'),
    dict(theme='profitability_quality', hypothesis='cash-based operating profitability, income-statement form (Ball-'
         'Gerakos-Linnainmaa-Nikolaev 2016)',
         blocker='no cogs_ttm, xsga_ttm or working-capital items (receivables, inventory, prepaid, deferred revenue, '
                 'payables, accrued expenses); no interest expense for the cash-flow-statement add-back',
         needs='cogs_ttm, xsga_ttm, the six working-capital balances (and lags), xint_ttm',
         v6='cbop uses the cash-flow-statement approximation (operating cash flow + R&D) / assets'),
    dict(theme='low_risk', hypothesis='SMAX with MAX5 (mean of the five largest daily returns)',
         blocker='no top-k order-statistic operator in the DSL (ts_max, med only); an engine operator, not a field',
         needs='an engine op (e.g. ts_topk_mean) or a producer max5_21d field', v6='smax uses MAX1'),
]


@dataclass(frozen=True)
class Member:
    id: str
    theme: str
    tier: str
    base: object              # grammar().Expr: prior-positive base (the full expression for the composite)
    ranking: str              # within_industry_grp_ff12 | cross_section
    citation: str
    formula: str
    raw_prior_direction: int
    form: str                 # FORM_*
    change: str               # vs v5.1: unchanged | smoothing | added | replaces <id>
    change_detail: str
    domain: str = ''
    deviation: str = ''


# ---- pinned parents ------------------------------------------------------------------
def _load(name: str, file: str) -> ModuleType:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    assert spec is not None and spec.loader is not None, file
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


@functools.cache
def pinned_v5() -> tuple[ModuleType, bytes, bytes]:
    """The v5.1 generator, its library and recipe bytes re-derived and checked against the pins and the files."""
    module = _load('_frozen_generate_fund_ic_v5_for_v6', V5_GENERATOR)
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V5_LIBRARY], docs[V5_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V5_LIBRARY_SHA256, 'frozen v5.1 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V5_RECIPE_SHA256, 'frozen v5.1 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return module, library_bytes, recipe_bytes


@functools.cache
def v4() -> ModuleType:
    """A private copy of the v4 generator whose validator tables carry the two v6 registry rows."""
    pinned_v5()  # the v4 and v5.1 pins are verified on separate, unmodified copies first
    module = _load('_generate_fund_ic_v4_for_v6', V4_GENERATOR)
    module.REGISTRY = dict(module.REGISTRY, **V6_REGISTRY_ROWS)
    module.ROLLING_OPS = set(module.ROLLING_OPS) | V6_ROLLING_OPS
    return module


def parse(text: str):
    return v4().parse(text)


def registry_crosscheck(engine_root: Path | None = None) -> str:
    return v4().registry_crosscheck(engine_root)


def has_cs_op(node) -> bool:
    """True if a parsed tree holds a cross-sectional (member-masked) op."""
    return any(op.startswith('Cs') for op in v4().opcodes_of(node))


# ---- members ---------------------------------------------------------------------------
def v51_specs() -> dict:
    """id -> v4.Spec (built by the private v4 copy) for the 38 v5.1 members, checked against the v5.1 DSL bytes."""
    m, v5 = v4(), pinned_v5()[0]
    specs = {s.id: s for s in m.specs()}
    g = m.grammar()
    for s in v5.new_specs():  # opex_at, re-typed onto this module's Expr
        specs[s.id] = m.Spec(**dict(s.__dict__, base=g.Expr(s.base.text, s.base.lookback)))
    library = json.loads(pinned_v5()[1])
    assert [c['id'] for c in library['candidates']] == list(specs) and len(specs) == V5_CANDIDATES
    for cand in library['candidates']:
        assert m.expression(specs[cand['id']]).text == cand['dsl'], cand['id']
    return specs


def new_members() -> dict:
    """The five v6 additions / replacements (prior sign embedded)."""
    m = v4()
    g = m.grammar()
    Expr, binary, call, window, pair = g.Expr, g.binary, g.call, g.window, g.pair
    F = {f.name: Expr(f.name, f.prior_bars) for f in m.FIELDS}
    one, zero, half, three, neg1 = Expr('1'), Expr('0'), Expr('0.5'), Expr('3'), Expr('-1')
    div = lambda a, b: binary(a, '/', b)
    sub = lambda a, b: binary(a, '-', b)
    add = lambda a, b: binary(a, '+', b)
    mul = lambda a, b: binary(a, '*', b)
    neg = lambda x: mul(neg1, x)

    def positive(x, *domains):  # v4 house guard: x + 0 * log(d) is NaN unless every d > 0
        for d in domains:
            x = add(x, mul(zero, call('log', d)))
        return x

    close, mkt, me, at = F['close'], F['mkt_ret'], F['me_company'], F['at']
    ret = sub(div(close, window('delay', close, 1)), one)  # v2/v4 spelling, byte-identical
    ff12 = F[m.WITHIN_INDUSTRY]
    W, X = 'within_industry_grp_ff12', 'cross_section'

    # value_composite: mean of within-FF12 ranks of the decayed B/M, E/P and CF/P (numerators keep their sign)
    comps = [div(F['be'], me), div(F['ni_ttm'], me), div(F['cfo_ttm'], me)]
    ranked = [call('group_rank', window('decay_linear', c, SMOOTHING), ff12) for c in comps]
    composite = div(add(add(ranked[0], ranked[1]), ranked[2]), three)

    # cbop: (operating cash flow + R&D) / total assets; unreported R&D (NaN) counts as 0:
    # power(x, 1 - n) - n with n = ts_count_nans(x, 1) in {0, 1} is x when x is finite and pow(NaN, 0) - 1 = 0
    # when x is NaN (C99 Annex F: pow(x, +-0) = 1 for any x, even NaN; vm.hpp Pow = std::pow).
    nan_xrd = window('ts_count_nans', F['xrd_ttm'], 1)
    xrd0 = sub(call('power', F['xrd_ttm'], sub(one, nan_xrd)), nan_xrd)
    cbop = positive(div(add(F['cfo_ttm'], xrd0), at), at)

    # res_mom_12_1: standardized market-model residual return over the 12-1 window. v4.2's base is
    # (S_r - b S_m) / (sd_r sqrt(1 - rho^2)) with b = ts_regression(ret, mkt_ret) = rho sd_r / sd_m (sample cov over
    # sample var; the Pearson rho is normalisation-free), i.e. (S_r / sd_r - rho S_m / sd_m) / sqrt(1 - rho^2): the
    # same quantity in a DAG of 7 estimated peak slots (v4.2's needs 8, above the library maximum of 7).
    n = RES_MOM_WINDOW
    rho = pair('correlation', ret, mkt, n)
    z_ret = div(window('ts_sum', ret, n), window('stddev', ret, n))
    z_mkt = div(window('ts_sum', mkt, n), window('stddev', mkt, n))
    res_mom = window('delay', div(sub(z_ret, mul(rho, z_mkt)),
                                  call('signedpower', call('abs', sub(one, mul(rho, rho))), half)), 21)

    # bac: -(correlation of overlapping 3-day returns with the member market over 250 sessions)
    bac = neg(pair('correlation', window('ts_sum', ret, 3), window('ts_sum', mkt, 3), BAC_WINDOW))

    # smax: -(largest daily return over 21 sessions / daily volatility over 252 sessions)
    vol = window('stddev', ret, VOL_WINDOW)
    smax = neg(positive(div(window('ts_max', ret, MAX_WINDOW), vol), vol))

    return {
        'value_composite': Member(
            'value_composite', 'value', 'B+', composite, W,
            'Fama and French (1992, JF); Lakonishok, Shleifer and Vishny (1994, JF); Israel, Laursen and Richardson '
            '(2021, JPM) composite value; Asness and Frazzini (2013, JPM) current price',
            'mean over k in {be, ni_ttm, cfo_ttm} of group_rank(decay_linear(k / me_company, 21), grp_ff12)', 1,
            FORM_COMPOSITE, 'added', 'composite value (brief: mean rank of bm, ep, cfp, ebit_ev)',
            'numerators keep their sign (negative book equity, losses and cash burn rank as expensive, the '
            'literature\'s negative-E flag); NaN when any of the three ratios is NaN',
            'three yields, not four: adding ebit_ev (oi_ttm / (me_company + debt - che)) needs 8 extra fields, above '
            'the IC-runner plan budget of 5 (NEEDS_NEW_FIELD); equal-weight mean of within-FF12 ranks (the '
            'literature proposal averages within-FF12 z-scores and adds S/EV and intangible book); each ratio is '
            'decayed before its rank (v6 smoothing form, no 21-session blackout)'),
        'cbop': Member(
            'cbop', 'profitability_quality', 'A-', cbop, W,
            'Ball, Gerakos, Linnainmaa and Nikolaev (2016, JFE) cash-based operating profitability (subsumes accruals); '
            'operating profitability excludes R&D: Ball, Gerakos, Linnainmaa and Nikolaev (2015, JFE)',
            '(cfo_ttm + xrd_ttm) / at, unreported R&D = 0', 1, FORM_RANK_OF_DECAY, 'replaces cfoa',
            'cash-based operating profitability replaces cfoa (cfo_ttm / average assets, which cited the same paper): '
            'one variant per hypothesis',
            'non-positive total assets -> NaN (house guard); NaN xrd_ttm (not reported) -> 0 via power(x, 1 - n) - n, '
            'n = ts_count_nans(x, 1)',
            'cash-flow-statement approximation: operating cash flow plus R&D expense over total assets. The paper\'s '
            'income-statement form (revenue - COGS - SG&A excl. R&D - changes in receivables, inventory, prepaid '
            'expenses + changes in deferred revenue, payables, accrued expenses) needs fields absent from fields-v6; '
            'CFO still deducts interest and taxes, which CbOP excludes (no interest-expense field). Relies on '
            'pow(NaN, 0) = 1 (C99 Annex F) in the VM\'s std::pow'),
        'res_mom_12_1': Member(
            'res_mom_12_1', 'price_momentum', 'B+', res_mom, X,
            'Blitz, Huij and Martens (2011, JEF) residual momentum; Blitz, Hanauer and Vidojevic (2020, JEF) '
            'idiosyncratic momentum; Grundy and Martin (2001, RFS)',
            'sum over t-251..t-21 of (ret - beta * mkt_ret) / sd of the market-model residual over the same window; '
            'beta = OLS slope of ret on mkt_ret over that window', 1, FORM_RANK_OF_DECAY, 'added',
            'residual momentum (brief: 12-1 momentum residual to the price-risk exposures expressible in the DSL)',
            '',
            'closest expressible proxy: residual to the market only. Of the price-risk-v1 exposures (beta252, vol63, '
            'ladv63) only the market is a return factor available in the DSL (mkt_ret); vol63 / ladv63 are '
            'characteristics, not return series, and FF49 industry returns (group_mean) are member-masked Cs ops that '
            'would blank the 231-session window after any membership gap. The paper uses Fama-French three-factor '
            'residuals estimated over the prior 36 months (beyond the 314-bar bound); with an in-window fit the '
            'intercept is kept (sum of ret - beta * mkt = n * alpha), so the score is the standardized formation-window '
            'alpha. Base algebraically identical to v4.2 res_mom_12_1 (beta = rho sd_r / sd_m), respelled to fit 7 '
            'slots; v4.2 TRAIN statistics were not read here'),
        'bac': Member(
            'bac', 'low_risk', 'B+', bac, X,
            'Asness, Frazzini, Gormsen and Pedersen (2020, JFE) betting against correlation; correlation of overlapping '
            '3-day returns: Frazzini and Pedersen (2014, JFE)',
            'corr(3-day ret, 3-day mkt_ret, 250)', -1, FORM_RANK_OF_DECAY, 'replaces low_beta',
            'betting against correlation replaces low_beta (prereg V6-L)', '',
            'window 250 sessions (paper: 5 years; lookback bound 314); the paper ranks correlation within volatility '
            'quintiles, which is not expressible (group operators need a Group classifier; quantile() yields F64), so '
            'volatility matching is left to the price-risk-v1 vol63 regressor that neutralizes the book and the '
            'admission factor; equal-weight member market'),
        'smax': Member(
            'smax', 'low_risk', 'B+', smax, X,
            'Asness, Frazzini, Gormsen and Pedersen (2020, JFE) scaled MAX (SMAX); Bali, Cakici and Whitelaw (2011, '
            'JFE) MAX', 'max(ret over 21 sessions) / sd(ret, 252)', -1, FORM_RANK_OF_DECAY, 'replaces low_max',
            'scaled MAX replaces low_max (prereg V6-L)',
            'non-positive 252-session volatility -> NaN (house guard)',
            'MAX1 (the single largest daily return) instead of the mean of the five largest: the DSL has no top-k '
            'order statistic (NEEDS_NEW_FIELD); volatility over 252 sessions. Scaling by volatility removes the '
            'first-order volatility content that price-risk-v1 projects out'),
    }


def roster() -> list[Member]:
    """v6 members in roster order (v5.1 order with the declared edits)."""
    m = v4()
    g = m.grammar()
    specs, added = v51_specs(), new_members()
    close, one = g.Expr('close'), g.Expr('1')
    # Fast sleeve seasonality without the decay: the window returns to the undecayed same-month-last-year window
    # [t-252, t-231] (v4 shifted it to delay 224 / 245 only to offset the s21 decay's 6.7-session centre lag).
    seasonality = g.binary(g.binary(g.window('delay', close, 231), '/', g.window('delay', close, 252)), '-', one)
    out = []
    for vid, spec in specs.items():
        if vid in INSERT_BEFORE:
            out.append(added[INSERT_BEFORE[vid]])
        if vid in REMOVE:
            continue
        if vid in REPLACE:
            out.append(added[REPLACE[vid]])
            continue
        base = spec.base
        base_tree = parse(base.text)[0]
        common = dict(id=vid, theme=spec.theme, tier=spec.tier, ranking=spec.ranking, citation=spec.citation,
                      formula=spec.formula, raw_prior_direction=spec.raw_prior_direction, domain=spec.domain,
                      deviation=spec.deviation)
        if vid in FAST_SLEEVE_TAU:
            assert FAST_SLEEVE_TAU[vid] >= FAST_TAU
            detail = f'fast sleeve (tau {FAST_SLEEVE_TAU[vid]:.4f} >= {FAST_TAU}): 21-session decay removed'
            if vid == 'seasonality_same_month':
                base = seasonality
                detail += '; window re-centred to [t-252, t-231] (the delay 224 / 245 shift only offset the decay)'
                common.update(formula='close[t-231] / close[t-252] - 1: the same 21 sessions one year before the next 21',
                              deviation='annual lag 12 only (lags 24+ exceed the 314-bar bound; NEEDS_NEW_FIELD); '
                                        'unsmoothed, so the window is the undecayed [t-252, t-231]')
            if vid == 'ind_adj_rev_5':
                common.update(deviation='weekly industry-adjusted return, unsmoothed (fast sleeve; construction theta '
                                        'smooths); the papers use one month')
            out.append(Member(base=base, form=FORM_RANK, change='smoothing', change_detail=detail, **common))
        elif has_cs_op(base_tree):
            out.append(Member(base=base, form=FORM_DECAY_OF_RANK, change='unchanged',
                              change_detail='x holds a member-masked Cs op, so the blackout binds under either order: '
                                            'left byte-identical', **common))
        else:
            out.append(Member(base=base, form=FORM_RANK_OF_DECAY, change='smoothing',
                              change_detail='decay_linear(R(x), 21) -> R(decay_linear(x, 21)): x holds no Cs op, so '
                                            'the 21-session membership blackout is removed', **common))
    return out


def expression(member: Member):
    g = v4().grammar()
    within = member.ranking.startswith('within_industry')
    R = (lambda x: g.call('group_rank', x, g.Expr(v4().WITHIN_INDUSTRY))) if within else (lambda x: g.call('rank', x))
    if member.form == FORM_COMPOSITE:
        return member.base
    if member.form == FORM_RANK_OF_DECAY:
        return R(g.window('decay_linear', member.base, SMOOTHING))
    if member.form == FORM_DECAY_OF_RANK:
        return g.window('decay_linear', R(member.base), SMOOTHING)
    assert member.form == FORM_RANK, member.form
    return R(member.base)


def smoothing_sessions(member: Member) -> int:
    return 1 if member.form == FORM_RANK else SMOOTHING


def documents() -> dict[str, bytes]:
    m = v4()
    _, library_v5_bytes, recipe_v5_bytes = pinned_v5()
    library_v5, recipe_v5 = json.loads(library_v5_bytes), json.loads(recipe_v5_bytes)
    v5_dsl = {c['id']: c['dsl'] for c in library_v5['candidates']}
    members = roster()
    added = new_members()
    ids = [x.id for x in members]
    assert len(ids) == len(set(ids)) == V5_CANDIDATES <= MAX_ROSTER, ids
    assert set(ids) == (set(v5_dsl) - set(REMOVE) - set(REPLACE)) | set(added)
    assert set(PRIOR_SIGN_SOURCES) == set(ids)

    candidates, lineage, templates, static, changes = [], [], [], [], []
    theme_members = {t: [] for t in m.THEME_IDS}
    for order, x in enumerate(members, start=1):
        assert x.theme in theme_members and x.tier in m.TIER_RANK
        assert PRIOR_SIGN_SOURCES[x.id][0] == x.raw_prior_direction in (-1, 1), x.id
        in_block = x.theme in m.WITHIN_INDUSTRY_THEMES and x.id not in m.WITHIN_INDUSTRY_EXCEPTIONS
        assert x.ranking.startswith('within_industry') == in_block, x.id  # prereg R1 (themes 1-3 within FF12)
        assert parse(x.base.text)[3] == x.base.lookback, x.id
        expr = expression(x)
        check = m.validate(expr.text, expr.lookback)
        assert len(check['extra_fields']) <= MAX_EXTRAS_PER_CANDIDATE, (x.id, check['extra_fields'])
        assert check['estimated_peak_slots'] <= MAX_SLOTS_PER_CANDIDATE, (x.id, check['estimated_peak_slots'])
        tree = parse(expr.text)[0]
        if x.change == 'unchanged':
            assert expr.text == v5_dsl[x.id], x.id
        elif x.id in v5_dsl:
            assert expr.text != v5_dsl[x.id], x.id
        if x.form == FORM_RANK_OF_DECAY:  # exactly one masked op, the outermost: no ts op above a Cs op
            assert tree[1] in ('CsRank', 'CsRankG') and tree[2][:2] == ('call', 'TsDecayLinear'), x.id
            assert not has_cs_op(tree[2]), x.id
        if x.form == FORM_RANK:
            assert tree[1] in ('CsRank', 'CsRankG') and 'TsDecayLinear' not in m.opcodes_of(tree), x.id
        if x.form == FORM_COMPOSITE:
            assert all(r[:2] == ('call', 'CsRankG') and r[2][:2] == ('call', 'TsDecayLinear') and not has_cs_op(r[2])
                       for r in (tree[2][2][2], tree[2][2][3], tree[2][3])), x.id
        static.append(check)
        theme_members[x.theme].append(x.id)
        labels = dict(theme=x.theme, tier=x.tier, tier_rank=m.TIER_RANK[x.tier], prior_sign=1, citation=x.citation)
        candidates.append(dict(id=x.id, family=x.theme, dsl=expr.text, horizons=[5, 21, 63],
                               sign_policy='train-rank-ic21', **labels))
        lineage.append(dict(id=x.id, family=x.theme, roster_order=order, **labels,
                            raw_prior_direction=x.raw_prior_direction, prior_sign_source=PRIOR_SIGN_SOURCES[x.id][1],
                            ranking=x.ranking, smoothing_form=x.form, smoothing_sessions=smoothing_sessions(x),
                            prior_bars=expr.lookback, native_prior_bars=check['native_prior_bars'],
                            dsl_sha256=hashlib.sha256(expr.text.encode()).hexdigest(), fields=check['fields'],
                            v51_change=x.change))
        templates.append(dict(id=x.id, theme=x.theme, formula=x.formula,
                              base_dsl=None if x.form == FORM_COMPOSITE else x.base.text,
                              base_prior_bars=None if x.form == FORM_COMPOSITE else x.base.lookback,
                              raw_prior_direction=x.raw_prior_direction, domain=x.domain or None,
                              deviation=x.deviation or None, smoothing_form=x.form,
                              smoothing_sessions=smoothing_sessions(x)))
        changes.append(dict(id=x.id, change=x.change, detail=x.change_detail,
                            v51_dsl_sha256=hashlib.sha256(v5_dsl[x.id].encode()).hexdigest() if x.id in v5_dsl
                            else None))
    for vid, reason in REMOVE.items():
        changes.append(dict(id=vid, change='removed', detail=reason, v51_dsl_sha256=hashlib.sha256(
            v5_dsl[vid].encode()).hexdigest()))
    for vid, new in REPLACE.items():
        changes.append(dict(id=vid, change=f'replaced by {new}', detail=added[new].change_detail,
                            v51_dsl_sha256=hashlib.sha256(v5_dsl[vid].encode()).hexdigest()))
    assert len({c['dsl'] for c in candidates}) == len(candidates)
    assert all(theme_members.values())

    referenced = set().union(*(set(s['fields']) for s in static))
    fields = [f for f in library_v5['fields'] if f['name'] in referenced or f['name'] in m.BASE_FIELDS]
    declared = {f['name'] for f in fields}
    assert referenced <= declared and declared - referenced <= set(m.BASE_FIELDS)
    families = [dict(f, description=THEME_DESCRIPTIONS.get(f['id'], f['description'])) for f in library_v5['families']]
    library = dict(schema='atx.dsl-ic-library/v1', id=LIBRARY_ID, fields=fields, families=families,
                   candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)

    extras = sorted(declared - set(m.BASE_FIELDS))
    fast = {x.id: FAST_SLEEVE_TAU[x.id] for x in members if x.id in FAST_SLEEVE_TAU}
    hygiene = ('Every change is prereg v6 V6-L as specified by the task brief, decided from the literature (v6-literature '
               'sections 2-4), the v6 signal code review (I2, I3, I5), the DSL/VM semantics and field metadata. The '
               'implementer read no TRAIN return statistic to choose any definition, sign, window or roster position and '
               'no data dated 2023 or later. Disclosed: the scorecard alpha table (per-candidate TRAIN HAC t) was in the '
               'brief\'s reading list and was seen; it was not used. The fast-sleeve rule reads standalone TRAIN tau '
               '(turnover, a second moment) from the v5.1 ew admission, as the brief specifies. res_mom_12_1\'s base is '
               'algebraically identical to the v4.2 candidate, whose v4.2 TRAIN statistics were not read here.')
    recipe = dict(
        schema='atx.dsl-ic-experiment/v1', id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(rule='v51-revised-v6l', revision='initial', candidates=len(candidates),
                        families=len(m.THEME_IDS), family_is_theme=True, one_variant_per_hypothesis=True,
                        max_roster=MAX_ROSTER,
                        wrapper=('R(decay_linear(x, 21)) where x holds no Cs op; decay_linear(R(x), 21) kept where x '
                                 'holds a Cs op; R(x) for fast sleeves (tau >= 0.08); composite: mean_k '
                                 'R(decay_linear(x_k, 21)); R = group_rank(., grp_ff12) for themes 1-3, rank(.) '
                                 'otherwise'),
                        smoothing_policy=dict(
                            blackout=('vm.hpp set_cross_section_mask: every Cs op emits NaN for non-members '
                                      '(strategy_ic_runner.cpp sets decision_member); ts_ops.hpp: min_periods = full '
                                      'window, any NaN -> NaN; so decay_linear(R(x), 21) is NaN for 21 sessions after '
                                      'any membership gap. Field loads keep observed history for non-members, so '
                                      'R(decay_linear(x, 21)) with a Cs-free x has one masked op, the last'),
                            fast_sleeves=dict(rule=f'standalone TRAIN tau >= {FAST_TAU}: decay removed (construction '
                                                   'theta smooths)', source=ADMISSION_V51,
                                              source_sha256=ADMISSION_V51_SHA256, tau=fast),
                            additions='R(decay_linear(x, 21)) (s21 default, no measured tau)'),
                        max_prior_bars=m.MAX_PRIOR_BARS, complete_observations=m.MAX_PRIOR_BARS + 1,
                        grammar_helpers=recipe_v5['generation']['grammar_helpers'],
                        parent_v51=dict(generator=V5_GENERATOR, library=V5_LIBRARY, library_sha256=V5_LIBRARY_SHA256,
                                        recipe=V5_RECIPE, recipe_sha256=V5_RECIPE_SHA256, candidates=V5_CANDIDATES),
                        lineage_v42='res_mom_12_1 = v4.2 res_mom_12_1 base (fund_industry_ic_v42.recipe.json '
                                    '396a1d0d) with beta = rho sd_r / sd_m substituted (algebraically identical)',
                        registry_rows=V6_REGISTRY_ROWS,
                        roster_edits=dict(insert_before=INSERT_BEFORE, replace=REPLACE, remove=REMOVE,
                                          rule='a replacement takes the replaced member\'s slot; an addition the '
                                               'literature ranks as the preferred construction of a hypothesis '
                                               'cluster precedes the incumbent it upgrades (redundancy order '
                                               '(tier, roster order), prereg R3); no TRAIN statistic'),
                        generator_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest(),
                        v5_generator_sha256=recipe_v5['generation']['generator_sha256']),
        orientation=recipe_v5['orientation'],
        labels=dict(recipe_v5['labels'],
                    per_candidate=['theme', 'tier', 'tier_rank', 'prior_sign', 'citation'],
                    prior_sign_sources='lineage prior_sign_source: author-year of the raw direction',
                    tier_basis_v6=('v6 additions take the v6-literature evidence grades: residual momentum B+, cash-based '
                                   'operating profitability A-, BAC / SMAX B+; the composite value takes cfp\'s B+')),
        themes=[dict(theme=t, index=k + 1,
                     description=THEME_DESCRIPTIONS.get(t, next(f['description'] for f in library_v5['families']
                                                                if f['id'] == t)),
                     members=theme_members[t],
                     expected_turnover=THEME_TURNOVER_V6.get(t, next(r['expected_turnover'] for r in recipe_v5['themes']
                                                                    if r['theme'] == t)),
                     expected_turnover_v6={i: EXPECTED_TURNOVER_V6[i] for i in theme_members[t]
                                           if i in EXPECTED_TURNOVER_V6} or None)
                for k, t in enumerate(m.THEME_IDS)],
        within_industry=dict(recipe_v5['within_industry'],
                             members=[x.id for x in members if x.ranking.startswith('within_industry')]),
        industry_level_price_members=dict(group_fields={m.FINE_INDUSTRY: ['ind_mom_12_1', 'within_ind_mom',
                                                                          'ind_adj_rev_5']},
                                          source=recipe_v5['industry_level_price_members']['source']),
        v6_changes=changes,
        lowvol_ind_argument=LOWVOL_IND_ARGUMENT,
        ear_centred_earnings_momentum=('kept as ear: the event-date field earn_recent exists and ear is already the '
                                       '[-1, +1] market-adjusted return around the vendor reaction session, carried to '
                                       'the next announcement (<= 126 sessions); a second EAR variant would break one '
                                       'variant per hypothesis. Industry adjustment and event-time decay (v6-'
                                       'literature 3.1) are not adopted'),
        needs_new_field=NEEDS_NEW_FIELD,
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene,
                           v51_statement=recipe_v5['family_fixing']['statement']),
        templates=templates,
        lineage=lineage,
        data=dict(recipe_v5['data'], fields=fields,
                  rd_zero_fill='cbop: power(xrd_ttm, 1 - ts_count_nans(xrd_ttm, 1)) - ts_count_nans(xrd_ttm, 1) is '
                               'xrd_ttm when reported and 0 when NaN (C99 pow(NaN, 0) = 1)',
                  composite='value_composite: numerators keep their sign; one NaN ratio makes the member NaN'),
        admission=dict(policy='v4-prior-v1 (T23; prereg R3; prereg v6 V6-L: admission trials = roster size)',
                       trials=len(candidates), v51_policy=recipe_v5['admission']['policy']),
        composition=dict(policy='prereg v6 V6-L: 1 composition (the V6-W rule, fit on this library)',
                         themes='unchanged 9 families (the fitter\'s V4_THEMES); theme merging / dropping is V6-W'),
        trials=dict(generated_candidates=len(candidates), admission_trials=len(candidates),
                    new_candidates=sum(x.change == 'added' or x.change.startswith('replaces') for x in members),
                    changed_candidates=sum(x.change == 'smoothing' for x in members),
                    unchanged_candidates=sum(x.change == 'unchanged' for x in members),
                    removed_v51_candidates=len(REMOVE) + len(REPLACE), orientation_fits=0,
                    composition_recipes=1, construction_recipes=1, variants_per_hypothesis=1,
                    family_or_template_selection=False, validation_selection=False),
        static_validation=dict(rule=recipe_v5['static_validation']['rule'] + '; v6 adds registry rows power, '
                                    'ts_count_nans (cross-checked against registry.cpp)',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_native_prior_bars=max(s['native_prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               max_estimated_peak_slots_limit=MAX_SLOTS_PER_CANDIDATE,
                               extra_field_capacity=max(len(s['extra_fields']) for s in static),
                               max_extras_per_candidate_limit=MAX_EXTRAS_PER_CANDIDATE,
                               declared_extra_fields=len(extras),
                               extra_field_users={f: [c['id'] for c, s in zip(candidates, static) if f in s['fields']]
                                                  for f in extras}),
        qualification=dict(native_parse_vm='pending: root --plan-only compile against fields-v6 (u phase)',
                           empirical='unmeasured at source freeze',
                           prior_phase='v1-v5.1 and v4.2 files and generators are unchanged by v6'))
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--engine-root', type=Path, default=None,
                        help='repository root whose atx-engine sources the registry cross-check reads (default: this tree)')
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
    library, recipe = json.loads(docs[LIBRARY]), json.loads(docs[RECIPE])
    sv, tr = recipe['static_validation'], recipe['trials']
    print(f"candidates {len(library['candidates'])} (<= {MAX_ROSTER}): new {tr['new_candidates']}, smoothing-changed "
          f"{tr['changed_candidates']}, unchanged {tr['unchanged_candidates']}, v5.1 removed "
          f"{tr['removed_v51_candidates']}; max prior bars {sv['max_prior_bars']}; max dag nodes {sv['max_dag_nodes']}; "
          f"max peak slots {sv['max_estimated_peak_slots']} (limit {MAX_SLOTS_PER_CANDIDATE}); extra-field capacity "
          f"{sv['extra_field_capacity']} (limit {MAX_EXTRAS_PER_CANDIDATE})")
    for theme in recipe['themes']:
        print(f"  {theme['index']} {theme['theme']}: {', '.join(theme['members'])}")
    print(registry_crosscheck(args.engine_root))


if __name__ == '__main__':
    main()
