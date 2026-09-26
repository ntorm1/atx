"""Deterministic fixed research library; no prices, fitting, or random search.

Run with --check to verify the committed exact JSON bytes. The small expression
builder records static lookbacks; the native DSL compiler remains authoritative.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Expr:
    text: str
    lookback: int = 0


def binary(a: Expr, op: str, b: Expr) -> Expr:
    return Expr(f"({a.text} {op} {b.text})", max(a.lookback, b.lookback))


def call(name: str, *args: Expr) -> Expr:
    return Expr(f"{name}({', '.join(a.text for a in args)})", max(a.lookback for a in args))


def window(name: str, a: Expr, n: int) -> Expr:
    return Expr(f"{name}({a.text}, {n})", a.lookback + (n if name == 'delay' else n - 1))


def corr(a: Expr, b: Expr, n: int) -> Expr:
    return Expr(f"correlation({a.text}, {b.text}, {n})", max(a.lookback, b.lookback) + n - 1)


def documents() -> dict[str, bytes]:
    close, raw, volume = Expr('close'), Expr('raw_close'), Expr('volume')
    one, zero = Expr('1'), Expr('0')
    ret = binary(binary(close, '/', window('delay', close, 1)), '-', one)
    dollars = binary(raw, '*', volume)
    neg = lambda x: binary(Expr('-1'), '*', x)
    ratio = lambda a, b: binary(a, '/', b)
    skipret = lambda end, begin: binary(ratio(window('delay', close, end), window('delay', close, begin)), '-', one)
    families = [
        ('momentum', 'Intermediate return persistence, with the most recent month excluded.', [
            ('mom_12_1', skipret(21, 252)), ('mom_9_1', skipret(21, 189)), ('mom_6_1', skipret(21, 126))]),
        ('trend_quality', 'Path efficiency, return-sign consistency and risk-scaled persistence; related variants share one family budget.', [
            ('efficiency_63', ratio(window('ts_sum', ret, 63), window('ts_sum', call('abs', ret), 63))),
            ('sign_consistency_126', window('ts_mean', call('sign', ret), 126)),
            ('risk_scaled_6_1', ratio(skipret(21, 126), window('stddev', ret, 126)))]),
        ('medium_reversal', 'Smoothed contrarian intermediate returns and distance from a trailing mean.', [
            ('reversal_21_skip5', neg(skipret(5, 21))), ('reversal_63_skip5', neg(skipret(5, 63))),
            ('mean_distance_63', neg(binary(ratio(close, window('ts_mean', close, 63)), '-', one)))]),
        ('defensive_risk', 'Low volatility, downside variability and volatility expansion; factor preferences, not neutralized alpha.', [
            ('low_vol_63', neg(window('stddev', ret, 63))),
            ('low_downside_126', neg(window('ts_mean', binary(call('min', ret, zero), '*', call('min', ret, zero)), 126))),
            ('vol_expansion_21_126', neg(ratio(window('stddev', ret, 21), window('stddev', ret, 126))))]),
        ('liquidity_state', 'Raw-dollar liquidity level, expansion and instability; no adjusted-price/share-volume dollar proxy.', [
            ('dollar_liquidity_63', window('ts_mean', dollars, 63)),
            ('dollar_expansion_21_126', neg(ratio(window('ts_mean', dollars, 21), window('ts_mean', dollars, 126)))),
            ('dollar_instability_63', neg(window('stddev', ratio(dollars, window('ts_mean', dollars, 63)), 63)))]),
        ('flow_confirmation', 'Return/flow co-movement and signed dollar-flow balance, using actual shares times raw close.', [
            ('return_dollar_rank_corr63', corr(ret, call('rank', dollars), 63)),
            ('weekly_return_relative_flow63', corr(window('ts_sum', ret, 5), ratio(dollars, window('ts_mean', dollars, 63)), 63)),
            ('signed_dollar_balance63', ratio(window('ts_sum', binary(call('sign', ret), '*', dollars), 63), window('ts_sum', dollars, 63)))]),
        ('price_location', 'Adjusted-close position within its own trailing range and slow moving-average structure; no candle fields.', [
            ('near_high_126', ratio(close, window('ts_max', close, 126))),
            ('range_position_252', ratio(binary(close, '-', window('ts_min', close, 252)), binary(window('ts_max', close, 252), '-', window('ts_min', close, 252)))),
            ('mean_structure_21_126', ratio(window('ts_mean', close, 21), window('ts_mean', close, 126)))]),
        ('trading_year_seasonality', '252-session-year proxies only; not calendar seasonality or a claim of independent effects.', [
            ('year_forward_month', window('delay', window('ts_sum', ret, 21), 231)),
            ('year_forward_week', window('delay', window('ts_sum', ret, 5), 247)),
            ('year_forward_quarter', skipret(189, 252))]),
    ]
    candidates, lineage = [], []
    for family, _, templates in families:
        for template, base in templates:
            for smoothing in (21, 63):
                expression = window('decay_linear', call('rank', base), smoothing)
                assert expression.lookback <= 314
                candidate_id = f'{template}_s{smoothing}'
                candidates.append(dict(id=candidate_id, family=family, dsl=expression.text,
                                       horizons=[5, 21, 63], sign_policy='train-rank-ic21'))
                lineage.append(dict(id=candidate_id, family=family, template=template,
                                    smoothing_sessions=smoothing, prior_bars=expression.lookback,
                                    dsl_sha256=hashlib.sha256(expression.text.encode()).hexdigest()))
    assert len(candidates) == 48 and len({c['dsl'] for c in candidates}) == 48
    fields = [dict(name='close', basis='f64(source float32 raw close) * vendor cumulReturnFactor; adjusted total-return proxy'),
              dict(name='raw_close', basis='unadjusted source float32 close widened to f64; USD/share'),
              dict(name='volume', basis='unadjusted source share volume widened to f64')]
    library = dict(schema='atx.dsl-ic-library/v1', id='slow_price_volume_ic48_v1', fields=fields,
                   families=[dict(id=f, description=d) for f, d, _ in families], candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    recipe = dict(schema='atx.dsl-ic-experiment/v1', id='slow_price_volume_ic48_v1_initial',
        library=dict(path='slow_price_volume_ic48_v1.json', sha256=hashlib.sha256(library_bytes).hexdigest()),
        generation=dict(rule='fixed-eight-by-three-by-two-v1', candidates=48, families=8, templates_per_family=3,
                        smoothing_sessions=[21, 63], seed=None, max_prior_bars=314, complete_observations=315),
        lineage=lineage,
        data=dict(fields=fields, raw_dollar_liquidity='raw_close * volume', horizons=[5, 21, 63], execution_delay=1,
                  decision_support='as-of member AND present AND finite positive current close',
                  vm_support='full historical as-of membership; temporal raw history retained',
                  forward_support='strict source presence and positive finite endpoints; pair counts and missing fractions reported'),
        orientation=dict(rule='train-rank-ic21', role='train', zero_or_undefined='sign0; KEEP; neutral fixed contribution',
                         freeze_before_validation=True, inverse='multiply centered rank by frozen sign', selection='none'),
        composition=dict(rule='fixed-family-centered-tied-rank-v1', family_weight='1/8', within_family_weight='1/6',
                         template_budget='1/3 of family (two variants each)', candidate_weight='1/48',
                         missing_contribution=0, renormalize_missing=False, learned_weights=False,
                         diversity='structural economic-family/template budgets; no statistical independence claim',
                         storage='one D*N blend plus one incoming candidate; orient then accumulate before discarding candidate (one VM pass)',
                         combined_ic_horizons=[5, 21, 63]),
        planned_turnover=dict(rule='rank-target-cadence5-partial25-no-drift-v1', cadence_sessions=5, trade_fraction=0.25,
                              cadence_anchor='role.score_begin', desired_transform='centered tied rank; dollar-neutral; gross1; no winsorization',
                              membership_exit='force planned zero immediately; do not renormalize remaining names',
                              report=['daily sum(abs(target_change))', 'initial deployment included and separately disclosed', 'planned gross/net', 'candidate contribution coverage'],
                              interpretation='planned weight change only; no execution, price drift, fees, borrow, liquidity caps, Sharpe or capacity claim'),
        trials=dict(generated_candidates=48, orientation_fits=48, inverse_additional_vm_evaluations=0,
                    family_or_template_selection=False, combined_recipes=1, validation_selection=False,
                    reruns='retain attempts; no discount against prior24 or historical trials',
                    horizons_and_smoothers_are_independent_trials=False),
        qualification=dict(native_parse_vm='pending focused fixture', empirical='unmeasured at source freeze',
                           low_turnover='smoothing intent; planned proxy measured, actual turnover unqualified',
                           prior_phase='original slow_price_volume_24_v1 and its receipts are unchanged'))
    return {'slow_price_volume_ic48_v1.json': library_bytes,
            'slow_price_volume_ic48_v1.recipe.json': encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    for name, expected in documents().items():
        path = directory / name
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f'fixed artifact differs: {name}')
        else:
            path.write_bytes(expected)
        print(f'{name} {hashlib.sha256(expected).hexdigest()} {len(expected)} bytes')


if __name__ == '__main__':
    main()
