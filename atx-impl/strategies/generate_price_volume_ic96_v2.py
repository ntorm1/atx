"""Deterministic 96-candidate research library v2; no prices, fitting, or random search.

Candidates 1-48 are slow_price_volume_ic48_v1, copied from its generator and
pinned by library SHA256, so ids, families, DSL bytes and dsl_sha256 values are
unchanged and cached signals are reused. Candidates 49-96 add eight diversifying
price/volume families (3 templates x 2 slow variants each) fixed from published
priors before any measurement. Run with --check to verify the committed exact
JSON bytes. A static validator re-parses every DSL string (grammar round-trip,
registry arity, declared fields, positive integer windows, prior-bar lookback,
DAG slot bound); the native DSL compiler remains authoritative.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
V1_GENERATOR = 'generate_slow_ic_48.py'
V1_LIBRARY = 'slow_price_volume_ic48_v1.json'
V1_RECIPE = 'slow_price_volume_ic48_v1.recipe.json'
V1_LIBRARY_SHA256 = '1ec75242f0328a459ac114256f99f534eb0b8ec2e642794d3f4ad846779a09ff'
V1_RECIPE_SHA256 = '3b6707f7a46e392391281a9f84585e6c6b1a1b5567cd9ff2656931b72481130b'
LIBRARY = 'price_volume_ic96_v2.json'
RECIPE = 'price_volume_ic96_v2.recipe.json'
FIELDS = ('close', 'raw_close', 'volume')
MAX_PRIOR_BARS = 314  # strategy_ic_runner admits lookback <= score_begin - 63; v1 bound kept
MAX_SLOTS = 64        # strategy_ic_runner rejects program.num_slots > 64
MAX_DSL_BYTES = 4096  # strategy_ic_runner rejects longer DSL text


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


def pair(name: str, a: Expr, b: Expr, n: int) -> Expr:
    return Expr(f"{name}({a.text}, {b.text}, {n})", max(a.lookback, b.lookback) + n - 1)


def new_families() -> list[tuple[str, str, str, list[tuple[str, Expr]]]]:
    close, raw, volume = Expr('close'), Expr('raw_close'), Expr('volume')
    one, zero, half, five = Expr('1'), Expr('0'), Expr('0.5'), Expr('5')
    ret = binary(binary(close, '/', window('delay', close, 1)), '-', one)
    mkt = call('vec_avg', ret)  # member-masked cross-sectional mean daily return
    dollars = binary(raw, '*', volume)
    ratio = lambda a, b: binary(a, '/', b)
    mul = lambda a, b: binary(a, '*', b)
    corr = lambda a, b, n: pair('correlation', a, b, n)
    week = lambda x: window('ts_sum', x, 5)
    beta = lambda n: pair('ts_regression', ret, mkt, n)
    unexplained = lambda n: binary(one, '-', mul(corr(ret, mkt, n), corr(ret, mkt, n)))
    resid_sum = lambda n: binary(window('ts_sum', ret, n), '-', mul(beta(n), window('ts_sum', mkt, n)))
    resid_sd = lambda n: mul(window('stddev', ret, n), call('signedpower', unexplained(n), half))
    idio_var = lambda n: mul(window('ts_var', ret, n), unexplained(n))
    return [
        ('residual_momentum',
         'Market-model (beta-adjusted) cumulative return and residual Sharpe, beta fit on the same trailing window, one-month skip.',
         'Blitz, Huij and Martens (2011, J. Empirical Finance) residual momentum; Grundy and Martin (2001, RFS) factor-adjusted momentum.', [
             ('resmom_6_1', window('delay', resid_sum(105), 21)),
             ('resmom_9_1', window('delay', resid_sum(168), 21)),
             ('resid_sharpe_12_1', window('delay', ratio(resid_sum(231), resid_sd(231)), 21))]),
        ('market_beta',
         'Trailing market beta: daily OLS slope, Frazzini-Pedersen three-day correlation times volatility ratio, negative realized semibeta.',
         'Frazzini and Pedersen (2014, JFE) betting against beta; Bollerslev, Li, Patton and Quaedvlieg (2022, JFE) realized semibetas.', [
             ('beta_126', beta(126)),
             ('fp_beta_250_63', ratio(mul(corr(window('ts_sum', ret, 3), window('ts_sum', mkt, 3), 250), window('stddev', ret, 63)),
                                      window('stddev', mkt, 63))),
             ('neg_semibeta_189', ratio(window('ts_sum', mul(call('min', ret, zero), call('min', mkt, zero)), 189),
                                        window('ts_sum', mul(mkt, mkt), 189)))]),
        ('idiosyncratic_risk',
         'Market-model residual variance (rank-equivalent to residual std) and the unexplained share of total variance.',
         'Ang, Hodrick, Xing and Zhang (2006, JF) idiosyncratic volatility; Durnev, Morck, Yeung and Zarowin (2003, JAR) firm-specific variation.', [
             ('ivol_21', idio_var(21)),
             ('ivol_126', idio_var(126)),
             ('idio_share_252', unexplained(252))]),
        ('lottery_demand',
         'Right-tail lottery features: maximum daily return, volatility-scaled maximum, realized return skewness.',
         'Bali, Cakici and Whitelaw (2011, JFE) MAX; Amaya, Christoffersen, Jacobs and Vasquez (2015, JFE) realized skewness.', [
             ('max_ret_21', window('ts_max', ret, 21)),
             ('scaled_max_63', ratio(window('ts_max', ret, 63), window('stddev', ret, 63))),
             ('skew_126', window('ts_skew', ret, 126))]),
        ('abnormal_volume',
         'Own-history share-volume shocks (weekly shock, monthly spike, quarterly trend); share volume, not v1 raw-dollar ratios.',
         'Gervais, Kaniel and Mingelgrin (2001, JF) high-volume return premium; Barber and Odean (2008, RFS) attention.', [
             ('volume_shock_5_63', ratio(window('ts_mean', volume, 5), window('ts_mean', volume, 63))),
             ('volume_spike_21_126', ratio(window('ts_max', volume, 21), window('ts_mean', volume, 126))),
             ('volume_trend_63_252', ratio(window('ts_mean', volume, 63), window('ts_mean', volume, 252)))]),
        ('illiquidity',
         'Amihud price impact abs(ret) / (raw_close * volume) averaged over 21, 63 and 126 sessions.',
         'Amihud (2002, J. Financial Markets) illiquidity premium.', [
             (f'amihud_{n}', window('ts_mean', ratio(call('abs', ret), dollars), n)) for n in (21, 63, 126)]),
        ('comovement',
         'Correlation with the member market return at daily and weekly frequency, and lagged-market (price-delay) correlation.',
         'Hou and Moskowitz (2005, RFS) price delay; Barberis, Shleifer and Wurgler (2005, JFE) comovement.', [
             ('market_corr_63', corr(ret, mkt, 63)),
             ('weekly_market_corr_126', corr(week(ret), week(mkt), 126)),
             ('lagged_market_corr_240', corr(week(ret), window('delay', week(mkt), 5), 240))]),
        ('volatility_dynamics',
         'Volatility of volatility, slow realized-volatility term structure and the five-day variance ratio; not v1 vol_expansion_21_126.',
         'Baltussen, van Bekkum and van der Grient (2018, JFQA) vol-of-vol; Adrian and Rosenberg (2008, JF) short/long volatility components.', [
             ('vol_of_vol_21_126', ratio(window('stddev', window('stddev', ret, 21), 126),
                                         window('ts_mean', window('stddev', ret, 21), 126))),
             ('vol_term_63_252', ratio(window('stddev', ret, 63), window('stddev', ret, 252))),
             ('variance_ratio_5_240', ratio(window('ts_var', week(ret), 240), mul(five, window('ts_var', ret, 240))))]),
    ]


# ---- static validator ------------------------------------------------------
# Rows of atx-engine/src/alpha/registry.cpp builtin_ops() used by this library:
# name -> (min_arity, max_arity, OpCode). Lookback classes follow typecheck:
# shift ops add d; rolling ops add d - 1; everything else takes max(children).
REGISTRY = {
    'abs': (1, 1, 'Abs'), 'sign': (1, 1, 'Sign'), 'min': (2, 2, 'MinP'), 'signedpower': (2, 2, 'Spow'),
    'rank': (1, 1, 'CsRank'), 'vec_avg': (1, 1, 'CsVecAvg'), 'delay': (2, 2, 'TsDelay'),
    'ts_sum': (2, 2, 'TsSum'), 'ts_mean': (2, 2, 'TsMean'), 'stddev': (2, 2, 'TsStd'), 'ts_var': (2, 2, 'TsVar'),
    'ts_min': (2, 2, 'TsMin'), 'ts_max': (2, 2, 'TsMax'), 'ts_skew': (2, 2, 'TsSkew'),
    'decay_linear': (2, 2, 'TsDecayLinear'), 'correlation': (3, 3, 'TsCorr'), 'ts_regression': (3, 3, 'TsRegression'),
}
SHIFT_OPS = {'TsDelay'}
ROLLING_OPS = {'TsSum', 'TsMean', 'TsStd', 'TsVar', 'TsMin', 'TsMax', 'TsSkew', 'TsDecayLinear', 'TsCorr', 'TsRegression'}
TOKEN = re.compile(r'\s*(?:(\d+(?:\.\d+)?)|([a-z_][a-z0-9_]*)|([-+*/(),]))')


def tokenize(text: str) -> list[str]:
    tokens, pos = [], 0
    while pos < len(text):
        match = TOKEN.match(text, pos)
        if not match or match.end() == pos:
            raise ValueError(f'lex error at {pos}: {text!r}')
        tokens.append(next(g for g in match.groups() if g is not None))
        pos = match.end()
    return tokens


def parse(text: str):
    """Fully parenthesized builder grammar -> (node, shape, lookback, rendered text)."""
    tokens, i = tokenize(text), 0

    def take(expected: str | None = None) -> str:
        nonlocal i
        if i >= len(tokens) or (expected is not None and tokens[i] != expected):
            raise ValueError(f'expected {expected!r} at token {i}: {text!r}')
        i += 1
        return tokens[i - 1]

    def primary():
        tok = take()
        if tok == '(':
            left = primary()
            op = take()
            if op not in '+-*/':
                raise ValueError(f'bad operator {op!r}')
            right = primary()
            take(')')
            shape = 'panel' if 'panel' in (left[1], right[1]) else 'scalar'
            return (('bin', op, left[0], right[0]), shape, max(left[2], right[2]), f'({left[3]} {op} {right[3]})')
        if tok == '-':  # native parser folds a negated literal into one literal
            tok = take()
            if not tok[0].isdigit():
                raise ValueError('unary minus only on literals here')
            return (('num', -float(tok)), 'scalar', 0, f'-{tok}')
        if tok[0].isdigit():
            if not math.isfinite(float(tok)):
                raise ValueError('non-finite literal')
            return (('num', float(tok)), 'scalar', 0, tok)
        if i < len(tokens) and tokens[i] == '(':
            take('(')
            args = [primary()]
            while tokens[i] == ',':
                take(',')
                args.append(primary())
            take(')')
            if tok not in REGISTRY:
                raise ValueError(f'unregistered operator {tok!r}')
            low, high, opcode = REGISTRY[tok]
            if not low <= len(args) <= high:
                raise ValueError(f'arity {len(args)} for {tok}')
            lookback = max(a[2] for a in args)
            if opcode in SHIFT_OPS | ROLLING_OPS:
                node = args[-1][0]
                if node[0] != 'num' or node[1] < 1 or node[1] != int(node[1]):
                    raise ValueError(f'{tok} window must be a positive integer literal')
                if any(a[1] == 'scalar' for a in args[:-1]):
                    raise ValueError(f'{tok} series operand is scalar')
                lookback += int(node[1]) - (0 if opcode in SHIFT_OPS else 1)
            elif opcode.startswith('Cs') and args[0][1] == 'scalar':
                raise ValueError(f'{tok} primary is scalar')
            rendered = f"{tok}({', '.join(a[3] for a in args)})"
            return (('call', opcode, *(a[0] for a in args)), 'panel', lookback, rendered)
        if tok not in FIELDS:
            raise ValueError(f'undeclared field {tok!r}')
        return (('field', tok), 'panel', 0, tok)

    result = primary()
    if i != len(tokens) or result[3] != text:
        raise ValueError(f'trailing tokens or non-canonical text: {text!r}')
    return result


def peak_slots(tree) -> tuple[int, int]:
    """Mirror dag.cpp post-order interning and bytecode.cpp slot retirement."""
    keys: dict[tuple, int] = {}
    kids: list[tuple[int, ...]] = []
    refs: list[int] = []

    def intern(node) -> int:
        children = tuple(intern(c) for c in node[2:]) if node[0] in ('call', 'bin') else ()
        key = (node[0], node[1], children)
        if key not in keys:
            keys[key] = len(kids)
            kids.append(children)
            refs.append(0)
            for c in children:
                refs[c] += 1
        return keys[key]

    refs_root = intern(tree)
    refs[refs_root] += 1
    live = peak = 0
    for children in kids:
        live += 1
        peak = max(peak, live)
        for c in children:
            refs[c] -= 1
            live -= refs[c] == 0
    return len(kids), peak


def validate(dsl: str, expected_lookback: int) -> dict:
    tree, shape, lookback, _ = parse(dsl)
    nodes, slots = peak_slots(tree)
    assert shape == 'panel' and lookback == expected_lookback <= MAX_PRIOR_BARS, (dsl, lookback, expected_lookback)
    assert nodes <= MAX_SLOTS and slots <= MAX_SLOTS and len(dsl.encode()) <= MAX_DSL_BYTES, dsl
    return dict(prior_bars=lookback, dag_nodes=nodes, estimated_peak_slots=slots)


def registry_crosscheck() -> str:
    engine = HERE.parent.parent / 'atx-engine'
    paths = [engine / 'src/alpha/registry.cpp', engine / 'src/alpha/typecheck.cpp',
             engine / 'include/atx/engine/alpha/typecheck.hpp']
    if not all(p.is_file() for p in paths):
        return 'registry cross-check skipped (engine sources absent)'
    registry, typecheck, header = (p.read_text(encoding='utf-8') for p in paths)
    rows = {m[1]: (int(m[2]), int(m[3]), m[4])
            for m in re.finditer(r'\{"(\w+)",\s*(\d+),\s*(\d+),\s*OpCode::(\w+)', registry)}
    body = typecheck.split('bool is_rolling_ts(OpCode op) noexcept {', 1)[1].split('return true;', 1)[0]
    rolling = set(re.findall(r'case OpCode::(\w+):', body))
    shift_body = re.search(r'is_shift_ts\(OpCode op\) noexcept \{(.*?)\}', header, re.S)
    if shift_body is None:
        raise SystemExit('typecheck.hpp is_shift_ts not found')
    shift = set(re.findall(r'OpCode::(\w+)', shift_body[1]))
    for name, sig in REGISTRY.items():
        if rows.get(name) != sig:
            raise SystemExit(f'registry row differs for {name}: {rows.get(name)} != {sig}')
        opcode = sig[2]
        if (opcode in ROLLING_OPS) != (opcode in rolling) or (opcode in SHIFT_OPS) != (opcode in shift):
            raise SystemExit(f'lookback class differs for {name}')
    return f'registry cross-check ok ({len(REGISTRY)} operators vs registry.cpp/typecheck)'


# ---- documents ---------------------------------------------------------------
def load_v1() -> tuple[dict, dict]:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('_frozen_generate_slow_ic_48', HERE / V1_GENERATOR)
    assert spec is not None and spec.loader is not None, V1_GENERATOR
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V1_LIBRARY], docs[V1_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V1_LIBRARY_SHA256, 'frozen v1 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V1_RECIPE_SHA256, 'frozen v1 recipe bytes changed'
    return json.loads(library_bytes), json.loads(recipe_bytes)


def documents() -> dict[str, bytes]:
    v1_library, v1_recipe = load_v1()
    v1_families = {f['id']: f['description'] for f in v1_library['families']}
    candidates, lineage, static = [], [], []
    for candidate, row in zip(v1_library['candidates'], v1_recipe['lineage'], strict=True):
        assert candidate['id'] == row['id'] and candidate['family'] == row['family']
        assert row['dsl_sha256'] == hashlib.sha256(candidate['dsl'].encode()).hexdigest()
        static.append(validate(candidate['dsl'], row['prior_bars']))
        candidates.append(candidate)
        lineage.append(dict(row, origin='frozen_v1'))
    families = new_families()
    templates = []
    for family, _, _, family_templates in families:
        assert family not in v1_families and len(family_templates) == 3
        for template, base in family_templates:
            templates.append(dict(family=family, template=template, base_dsl=base.text, base_prior_bars=base.lookback))
            for smoothing in (21, 63):
                expression = window('decay_linear', call('rank', base), smoothing)
                assert expression.lookback <= MAX_PRIOR_BARS
                static.append(validate(expression.text, expression.lookback))
                candidate_id = f'{template}_s{smoothing}'
                candidates.append(dict(id=candidate_id, family=family, dsl=expression.text,
                                       horizons=[5, 21, 63], sign_policy='train-rank-ic21'))
                lineage.append(dict(id=candidate_id, family=family, template=template,
                                    smoothing_sessions=smoothing, prior_bars=expression.lookback,
                                    dsl_sha256=hashlib.sha256(expression.text.encode()).hexdigest(), origin='new_v2'))
    ids = [c['id'] for c in candidates]
    assert len(candidates) == 96 and len(set(ids)) == 96 and len({c['dsl'] for c in candidates}) == 96
    assert all(re.fullmatch(r'[a-z0-9_]{1,64}', s) for s in ids + [c['family'] for c in candidates])
    assert candidates[:48] == v1_library['candidates']
    library = dict(schema='atx.dsl-ic-library/v1', id='price_volume_ic96_v2', fields=v1_library['fields'],
                   families=v1_library['families'] + [dict(id=f, description=d) for f, d, _, _ in families],
                   candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    fixed = ('The eight new families and their 24 templates were fixed from published economic priors before any '
             'v2 measurement; no IC, return, turnover or correlation output was consulted to choose or tune them. '
             'TRAIN fits only per-candidate signs; validation is not used for selection.')
    recipe = dict(schema='atx.dsl-ic-experiment/v1', id='price_volume_ic96_v2_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        generation=dict(rule='frozen-v1-48-plus-eight-by-three-by-two-v2', candidates=96, families=16, templates_per_family=3,
                        smoothing_sessions=[21, 63], wrapper='decay_linear(rank(base), s)', smoothing_exemptions=[],
                        smoothing_rationale='no family exempted: rank tames heavy-tailed bases (Amihud, volume ratios, skew, '
                                            'ratios of volatilities) and two decay variants keep the v1 3x2 family budget; '
                                            'already-slow 126-252 session bases only spend lookback, checked <= 314',
                        seed=None, max_prior_bars=MAX_PRIOR_BARS, complete_observations=MAX_PRIOR_BARS + 1,
                        frozen_v1=dict(generator=V1_GENERATOR, library=V1_LIBRARY, library_sha256=V1_LIBRARY_SHA256,
                                       recipe=V1_RECIPE, recipe_sha256=V1_RECIPE_SHA256, candidates=48, families=8,
                                       copy='ids, families, family descriptions and DSL bytes identical; dsl_sha256 unchanged'),
                        new=dict(candidates=48, families=8, templates=24)),
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=fixed),
        family_priors=[dict(family=f, origin='frozen_v1', prior=d) for f, d in v1_families.items()] +
                      [dict(family=f, origin='new_v2', prior=p) for f, _, p, _ in families],
        templates=templates,
        lineage=lineage,
        data=dict(v1_recipe['data'], daily_return='ret = (close / delay(close, 1)) - 1',
                  market_return='vec_avg(ret): equal-weight mean over as-of members with finite ret; broadcast only to member cells',
                  market_window_support='a market-return template is NaN for a name unless it was an as-of member on every '
                                        'session its trailing windows read (any-NaN window policy); coverage unmeasured'),
        orientation=v1_recipe['orientation'],
        composition=dict(v1_recipe['composition'], family_weight='1/16', within_family_weight='1/6',
                         template_budget='1/3 of family (two variants each)', candidate_weight='1/96'),
        planned_turnover=v1_recipe['planned_turnover'],
        trials=dict(generated_candidates=96, reused_v1_candidates=48, new_candidates=48, orientation_fits=96,
                    inverse_additional_vm_evaluations=0, family_or_template_selection=False,
                    families_fixed_before_measurement=True, combined_recipes=1, validation_selection=False,
                    reruns='retain attempts; v1 attempts remain prior trials of the reused 48',
                    horizons_and_smoothers_are_independent_trials=False),
        static_validation=dict(rule='python re-parse: canonical round-trip, registry arity, declared fields, '
                                    'positive integer windows, lookback recomputed per typecheck, dag slot bound',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               slot_estimate='post-order interning and refcount retirement; native --plan-only authoritative'),
        qualification=dict(native_parse_vm='pending root --plan-only compile check', empirical='unmeasured at source freeze',
                           low_turnover='smoothing intent; planned proxy measured, actual turnover unqualified',
                           prior_phase='slow_price_volume_ic48_v1 files, generator and receipts are unchanged'))
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for name, expected in documents().items():
        path = HERE / name
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f'fixed artifact differs: {name}')
        else:
            path.write_bytes(expected)
        print(f'{name} {hashlib.sha256(expected).hexdigest()} {len(expected)} bytes')
    print(registry_crosscheck())


if __name__ == '__main__':
    main()
