"""Deterministic 40-candidate research library v4.2 = frozen v4 (37) + three v4.2 additions; no data read.

Library v4.2 (T27) implements the v4 pre-registration section "v4.2 revision", R1': the frozen library v4
(fund_industry_ic_v4.json, 37 candidates) is copied unchanged (ids, families, fields, DSL bytes, lineage and
templates; pinned by library and recipe SHA-256 and re-derived from its own generator), then three
prior-signed candidates are appended, one canonical variant each:

  res_mom_12_1        price_momentum     Blitz, Huij and Martens (2011) residual momentum, 12-1
  eap                 earnings_momentum  Frazzini and Lamont (2007); Barber, De George, Lehavy and Trueman (2013)
  low_share_turnover  low_risk           Datar, Naik and Radcliffe (1998) low share turnover

The sign is embedded (prior_sign +1); theme, tier, tier_rank and citation are carried in the library rows
and the recipe lineage exactly as in v4. No v3/v4 per-candidate TRAIN performance output was read.

The static validator is v4's (imported from the pinned generator): a private copy of the v4 module gets
three more registry rows (sign, ts_mean, ts_regression), each cross-checked against registry.cpp and the
typecheck lookback classes. The frozen v4 generator and its files are not modified.

Run with --check to verify the committed exact JSON bytes.
"""
from __future__ import annotations

import argparse
import copy
import functools
import hashlib
import importlib.util
import json
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V4_GENERATOR = 'generate_fund_ic_v4.py'
V4_LIBRARY = 'fund_industry_ic_v4.json'
V4_RECIPE = 'fund_industry_ic_v4.recipe.json'
V4_LIBRARY_SHA256 = 'daa9663e43119102fbb42aba3be9b57961cd34925920c50baee7f340f0e1eddf'
V4_RECIPE_SHA256 = '62b510f12a79cfacd2e74d64e020f20cee202cadb82153653b2e1065ec250c81'
V4_CANDIDATES = 37
LIBRARY = 'fund_industry_ic_v42.json'
RECIPE = 'fund_industry_ic_v42.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v42'
PREREG = ('.superpowers/sdd/mega-alpha-20260926/v4-prereg.md sections R1 (v4, frozen) and "v4.2 revision" R1\' '
          '(declared 2026-09-27 after the v4 gate failed; disclosed)')
MAX_EXTRAS_PER_CANDIDATE = 5  # IC runner plan budget (controller ruling, T27)
EAP_PREDICTED_GAP = 63        # prereg R1': expected next announcement = last announcement + ~63 sessions
EAP_HORIZON = 21              # long when that expected announcement falls within the next 21 sessions
EAP_HISTORY = 126             # a name with no announcement in the last 126 sessions has no schedule: NaN
RES_MOM_WINDOW = 231          # 12-1: returns t-251..t-21 (formation 252 sessions, skip 21)
TURNOVER_WINDOW = 126         # prereg R1': 126-session mean of volume / shares_out
V42_REGISTRY_ROWS = {'sign': (1, 1, 'Sign'), 'ts_mean': (2, 2, 'TsMean'), 'ts_regression': (3, 3, 'TsRegression')}
V42_ROLLING_OPS = {'TsMean', 'TsRegression'}


def _load_v4(name: str) -> ModuleType:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, HERE / V4_GENERATOR)
    assert spec is not None and spec.loader is not None, V4_GENERATOR
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


@functools.cache
def frozen_v4() -> tuple[bytes, bytes]:
    """The v4 library and recipe bytes from an untouched copy of their generator, checked against the pins."""
    docs = _load_v4('_frozen_generate_fund_ic_v4_pin').documents()
    library_bytes, recipe_bytes = docs[V4_LIBRARY], docs[V4_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V4_LIBRARY_SHA256, 'frozen v4 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V4_RECIPE_SHA256, 'frozen v4 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return library_bytes, recipe_bytes


@functools.cache
def v4() -> ModuleType:
    """A private copy of the v4 generator whose validator tables carry the three v4.2 registry rows."""
    frozen_v4()  # pins verified on a separate, unmodified copy first
    module = _load_v4('_generate_fund_ic_v4_for_v42')
    module.REGISTRY = dict(module.REGISTRY, **V42_REGISTRY_ROWS)
    module.ROLLING_OPS = set(module.ROLLING_OPS) | V42_ROLLING_OPS
    return module


def parse(text: str):
    return v4().parse(text)


def registry_crosscheck(engine_root: Path | None = None) -> str:
    return v4().registry_crosscheck(engine_root)


# ---- the three v4.2 additions ----------------------------------------------------
def new_specs() -> list:
    m = v4()
    g = m.grammar()
    Expr, binary, call, window, pair = g.Expr, g.binary, g.call, g.window, g.pair
    F = {f.name: Expr(f.name, f.prior_bars) for f in m.FIELDS}
    one, zero, half, neg1 = Expr('1'), Expr('0'), Expr('0.5'), Expr('-1')
    div = lambda a, b: binary(a, '/', b)
    sub = lambda a, b: binary(a, '-', b)
    add = lambda a, b: binary(a, '+', b)
    mul = lambda a, b: binary(a, '*', b)
    neg = lambda x: mul(neg1, x)
    close, mkt = F['close'], F['mkt_ret']
    ret = sub(div(close, window('delay', close, 1)), one)  # v2/v4 spelling, byte-identical

    # res_mom_12_1: standardized market-model residual return over the 12-1 formation window
    n = RES_MOM_WINDOW
    rho = pair('correlation', ret, mkt, n)
    resid_sum = sub(window('ts_sum', ret, n), mul(pair('ts_regression', ret, mkt, n), window('ts_sum', mkt, n)))
    resid_sd = mul(window('stddev', ret, n), call('signedpower', call('abs', sub(one, mul(rho, rho))), half))
    res_mom = window('delay', div(resid_sum, resid_sd), 21)

    # eap: announcement expected within the next 21 sessions (last announcement + 63), from past reactions only
    earn = F['earn_recent']
    event = mul(earn, window('delay', earn, 1))  # 1 on r+1: the vendor reaction session r and r+1 are flagged
    last_in_window = window('delay', window('ts_sum', event, EAP_HORIZON), EAP_PREDICTED_GAP - EAP_HORIZON - 1)
    none_since = sub(one, call('sign', window('ts_sum', event, EAP_PREDICTED_GAP - EAP_HORIZON - 1)))
    eap = add(mul(call('sign', last_in_window), none_since), mul(zero, call('log', window('ts_sum', event, EAP_HISTORY))))

    # low_share_turnover: 126-session mean of daily share turnover, low turnover long
    shares = F['shares_out']
    daily_turnover = add(div(F['volume'], shares), mul(zero, call('log', shares)))
    low_turnover = neg(window('ts_mean', daily_turnover, TURNOVER_WINDOW))

    X = 'cross_section'
    Spec = m.Spec
    return [
        Spec('res_mom_12_1', 'price_momentum', 'A-', res_mom, X,
             'Blitz, Huij and Martens (2011, J. Empirical Finance) residual momentum; Grundy and Martin (2001, RFS)',
             'sum over t-251..t-21 of (ret - beta * mkt_ret) / sd of the market-model residual over the same window; '
             'beta = OLS slope of ret on mkt_ret over that window', 1, '',
             'market model on the equal-weight member market (the paper: Fama-French three factors) and betas '
             'estimated on the formation window itself (the paper: the prior 36 months, beyond the 314-bar bound); '
             'with an in-window fit the intercept is kept (sum of ret - beta * mkt = n * alpha), so the score is the '
             'standardized formation-window alpha. Byte-identical to the base of the v2 template resid_sharpe_12_1'),
        Spec('eap', 'earnings_momentum', 'B-', eap, X,
             'Frazzini and Lamont (2007, WP) the earnings announcement premium and trading volume; Barber, De George, '
             'Lehavy and Trueman (2013, JFE) the earnings announcement premium around the globe',
             '1 when the last vendor reaction session r satisfies r + 63 in [t+1, t+21] (marker r+1 in [t-61, t-41] and '
             'no later marker), else 0; NaN without an announcement in the last 126 sessions', 1,
             'NaN when no announcement in the last 126 sessions (no schedule to predict from)',
             f'expected date = last announcement + {EAP_PREDICTED_GAP} sessions (prereg R1\'; the papers predict '
             'from the announcement of the same quarter a year earlier, as recalled). Built only from past '
             'earn_recent reaction flags: the vendor\'s own upcoming-event flag (earnFlag -1) is mapped to 0 by the '
             'producer and is not used, so the prediction is causal. The long leg holds through the reaction '
             'session and the day after, then leaves at r_next + 1', 1,
             'the premium accrues in the window up to and around the predicted announcement; the base switches off '
             'at the announcement, and a 21-session linear decay would keep the name long for about 20 sessions '
             'after it, where the cited papers find no premium'),
        Spec('low_share_turnover', 'low_risk', 'C+', low_turnover, X,
             'Datar, Naik and Radcliffe (1998, J. Financial Markets) liquidity and stock returns',
             f'mean over {TURNOVER_WINDOW} sessions of volume / shares_out', -1, 'non-positive shares_out -> NaN',
             'volume and the vendor shares_out are both on the session\'s share basis, so a split cancels; '
             'shares_out lags 90 days (A8); window 126 sessions (prereg R1\'; the paper averages three months of '
             'monthly turnover, as recalled)'),
    ]


THEME_DESCRIPTIONS = {
    'earnings_momentum': 'Earnings news: SUE, change in ROE, change in tax expense, the 3-day earnings announcement '
                         'return and the earnings announcement premium; good news predicts continuation.',
    'price_momentum': '12-1 momentum, industry 12-1 momentum, within-industry momentum, nearness to the 52-week high '
                      'and 12-1 residual momentum; winners continue.',
    'low_risk': 'Low beta, low idiosyncratic volatility, low MAX, within-industry low volatility and low share '
                'turnover; low risk predicts higher risk-adjusted returns.',
}
EXPECTED_TURNOVER_NEW = {
    'res_mom_12_1': 'low (~0.03/day): 231-session standardized residual return ranks with s21 decay',
    'eap': 'moderate (~0.05-0.08/day expected): a two-level signal; about 2/63 of names cross the window each '
           'session, unsmoothed; the v4-prior-v2 cost screen (tau_k > 0.08) decides',
    'low_share_turnover': 'very low: 126-session mean with s21 decay',
}


def documents() -> dict[str, bytes]:
    m = v4()
    library_v4_bytes, recipe_v4_bytes = frozen_v4()
    library_v4, recipe_v4 = json.loads(library_v4_bytes), json.loads(recipe_v4_bytes)
    assert len(library_v4['candidates']) == V4_CANDIDATES
    candidates, lineage, templates, static = [], [], [], []
    for cand, row in zip(library_v4['candidates'], recipe_v4['lineage'], strict=True):
        assert cand['id'] == row['id'] and row['dsl_sha256'] == hashlib.sha256(cand['dsl'].encode()).hexdigest()
        check = m.validate(cand['dsl'], row['prior_bars'])
        assert check['native_prior_bars'] == row['native_prior_bars'] and check['fields'] == row['fields']
        static.append(check)
    candidates, lineage, templates = (copy.deepcopy(library_v4['candidates']), copy.deepcopy(recipe_v4['lineage']),
                                      copy.deepcopy(recipe_v4['templates']))
    added = []
    for order, spec in enumerate(new_specs(), start=V4_CANDIDATES + 1):
        assert spec.theme in m.THEME_IDS and spec.theme not in m.WITHIN_INDUSTRY_THEMES
        assert spec.tier in m.TIER_RANK and spec.raw_prior_direction in (-1, 1) and spec.ranking == 'cross_section'
        assert spec.smoothing == m.SMOOTHING or spec.smoothing_reason, spec.id
        assert parse(spec.base.text)[3] == spec.base.lookback, spec.id
        expr = m.expression(spec)
        check = m.validate(expr.text, expr.lookback)
        static.append(check)
        labels = dict(theme=spec.theme, tier=spec.tier, tier_rank=m.TIER_RANK[spec.tier], prior_sign=1,
                      citation=spec.citation)
        candidates.append(dict(id=spec.id, family=spec.theme, dsl=expr.text, horizons=[5, 21, 63],
                               sign_policy='train-rank-ic21', **labels))
        lineage.append(dict(id=spec.id, family=spec.theme, roster_order=order, **labels,
                            raw_prior_direction=spec.raw_prior_direction, ranking=spec.ranking,
                            smoothing_sessions=spec.smoothing, prior_bars=expr.lookback,
                            native_prior_bars=check['native_prior_bars'],
                            dsl_sha256=hashlib.sha256(expr.text.encode()).hexdigest(), fields=check['fields']))
        templates.append(dict(id=spec.id, theme=spec.theme, formula=spec.formula, base_dsl=spec.base.text,
                              base_prior_bars=spec.base.lookback, raw_prior_direction=spec.raw_prior_direction,
                              domain=spec.domain or None, deviation=spec.deviation or None,
                              smoothing_sessions=spec.smoothing, smoothing_reason=spec.smoothing_reason or None))
        added.append(spec)
    total = len(candidates)
    ids = [c['id'] for c in candidates]
    assert total == 40 and len(set(ids)) == total and len({c['dsl'] for c in candidates}) == total
    assert candidates[:V4_CANDIDATES] == library_v4['candidates'] and lineage[:V4_CANDIDATES] == recipe_v4['lineage']
    assert all(len(s['extra_fields']) <= MAX_EXTRAS_PER_CANDIDATE for s in static), 'IC runner plan budget'
    declared = {f['name'] for f in library_v4['fields']}
    referenced = set().union(*(set(s['fields']) for s in static))
    assert referenced <= declared and declared - referenced <= {'raw_close', 'volume'} - referenced
    families = [dict(f, description=THEME_DESCRIPTIONS.get(f['id'], f['description'])) for f in library_v4['families']]
    library = dict(schema='atx.dsl-ic-library/v1', id=LIBRARY_ID, fields=library_v4['fields'], families=families,
                   candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)

    extras = sorted(declared - set(m.BASE_FIELDS))
    recipe = copy.deepcopy(recipe_v4)
    members = {t['theme']: list(t['members']) for t in recipe['themes']}
    for spec in added:
        members[spec.theme].append(spec.id)
    hygiene = ('The v4 roster (37) is frozen and copied byte-identically. The three additions and their themes are '
               'the v4.2 revision R1\' (declared 2026-09-27 after the v4 gate failed, disclosed in the prereg with the '
               'aggregate v4, v4.1 and T26 book results). Each takes the canonical definition of its cited paper within '
               'the prereg parameters; the implementer read no v3/v4 per-candidate TRAIN performance output '
               '(admission statistics, daily IC files) and no validation data. Disclosed: res_mom_12_1 is byte-identical '
               'to the v2/v3 candidate resid_sharpe_12_1_s21, whose TRAIN statistics the controller may have seen; its '
               'definition follows Blitz-Huij-Martens under the lookback bound, not a v2/v3 result.')
    recipe.update(
        id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(recipe_v4['generation'], rule='frozen-v4-37-plus-three-v42', revision='initial',
                        candidates=total,
                        wrapper=recipe_v4['generation']['wrapper'].replace('no exemption', 'exemption: eap'),
                        smoothing_exemptions={s.id: s.smoothing_reason for s in added if s.smoothing != m.SMOOTHING},
                        frozen_v4=dict(generator=V4_GENERATOR, library=V4_LIBRARY, library_sha256=V4_LIBRARY_SHA256,
                                       recipe=V4_RECIPE, recipe_sha256=V4_RECIPE_SHA256, candidates=V4_CANDIDATES,
                                       copy='ids, families (descriptions of three themes updated), fields, DSL bytes, '
                                            'lineage and templates identical'),
                        new=dict(candidates=len(added), ids=[s.id for s in added], registry_rows=V42_REGISTRY_ROWS),
                        generator_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest(),
                        v4_generator_sha256=recipe_v4['generation']['generator_sha256']),
        themes=[dict(t, members=members[t['theme']], description=THEME_DESCRIPTIONS.get(t['theme'], t['description']),
                     expected_turnover_v42_additions={s.id: EXPECTED_TURNOVER_NEW[s.id] for s in added
                                                      if s.theme == t['theme']} or None)
                for t in recipe_v4['themes']],
        v42_additions=[dict(id=s.id, theme=s.theme, prereg='v4.2 R1\'', eap_parameters=dict(
            predicted_gap_sessions=EAP_PREDICTED_GAP, horizon_sessions=EAP_HORIZON, history_sessions=EAP_HISTORY)
            if s.id == 'eap' else None) for s in added],
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene,
                           v4_statement=recipe_v4['family_fixing']['statement']),
        templates=templates,
        lineage=lineage,
        admission=dict(policy='v4-prior-v2 (T27; prereg v4.2 R3\': v4-prior-v1 plus reject standalone TRAIN tau_k > '
                              '0.08, status reject_turnover_cost)', trials=total,
                       v4_policy=recipe_v4['admission']['policy']),
        trials=dict(recipe_v4['trials'], generated_candidates=total, admission_trials=total, new_candidates=len(added),
                    reused_v4_candidates=V4_CANDIDATES, reruns='v4 admission attempts remain prior trials of the 37'),
        static_validation=dict(recipe_v4['static_validation'],
                               rule=recipe_v4['static_validation']['rule'] + '; v4.2 adds registry rows sign, ts_mean, '
                                    'ts_regression (cross-checked against registry.cpp)',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_native_prior_bars=max(s['native_prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               extra_field_capacity=max(len(s['extra_fields']) for s in static),
                               max_extras_per_candidate_limit=MAX_EXTRAS_PER_CANDIDATE,
                               declared_extra_fields=len(extras),
                               extra_field_users={f: [c['id'] for c, s in zip(candidates, static) if f in s['fields']]
                                                  for f in extras}),
        qualification=dict(recipe_v4['qualification'],
                           native_parse_vm='pending: root --plan-only compile against fields-v6',
                           prior_phase='v1-v4 files and generators are unchanged by v4.2'))
    recipe['data'] = dict(recipe_v4['data'],
                          eap_window='event = earn_recent * delay(earn_recent, 1) marks r+1; long iff a marker in '
                                     '[t-61, t-41] and none in [t-40, t]; NaN iff no marker in [t-125, t]',
                          share_turnover='volume / shares_out per session (same share basis), mean over 126 sessions')
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
    sv = recipe['static_validation']
    print(f"candidates {len(library['candidates'])} (frozen v4 {V4_CANDIDATES} byte-identical, new "
          f"{len(library['candidates']) - V4_CANDIDATES}); themes {len(library['families'])}; max prior bars "
          f"{sv['max_prior_bars']}; max dag nodes {sv['max_dag_nodes']}; max peak slots {sv['max_estimated_peak_slots']}; "
          f"extra-field capacity {sv['extra_field_capacity']} (limit {MAX_EXTRAS_PER_CANDIDATE})")
    for theme in recipe['themes']:
        print(f"  {theme['index']} {theme['theme']}: {', '.join(theme['members'])}")
    print(registry_crosscheck(args.engine_root))


if __name__ == '__main__':
    main()
