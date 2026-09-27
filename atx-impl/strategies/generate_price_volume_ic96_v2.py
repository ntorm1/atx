"""Deterministic 96-candidate research library v2; no prices, fitting, or random search.

Candidates 1-48 are slow_price_volume_ic48_v1, copied from its generator and
pinned by library SHA256, so ids, families, DSL bytes and dsl_sha256 values are
unchanged and cached signals are reused. Candidates 49-96 add eight diversifying
price/volume families (3 templates x 2 slow variants each) fixed from published
priors before any measurement; fix round 1 re-selected templates structurally
from review findings, still before any TRAIN measurement. Run with --check to
verify the committed exact JSON bytes. A static validator re-parses every DSL
string (grammar round-trip, registry arity, declared fields, positive integer
windows, prior-bar lookback, DAG slot bound); the native DSL compiler remains
authoritative.
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
# Prior bars each field needs beyond its own session. mkt_ret at d is built from
# closes at d-1 and d (like ret), so it is charged one prior bar; the native
# typecheck charges every field 0, which the validator also reports.
FIELD_PRIOR_BARS = {'close': 0, 'raw_close': 0, 'volume': 0, 'mkt_ret': 1}
MKT_RET_FIELD = dict(name='mkt_ret', basis=(
    'equal-weight mean adjusted close-to-close return (close[d] / close[d-1] - 1) over the previous session\'s '
    '(d-1) as-of members, broadcast to every name present at d; produced upstream, not by the DSL'))
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
    mkt = Expr('mkt_ret', FIELD_PRIOR_BARS['mkt_ret'])  # upstream member market return, all present names
    one, zero, half, five = Expr('1'), Expr('0'), Expr('0.5'), Expr('5')
    ret = binary(binary(close, '/', window('delay', close, 1)), '-', one)
    dollars = binary(raw, '*', volume)
    adj_volume = binary(dollars, '/', close)  # split-continuous share volume (volume / cumulative factor)
    amihud = binary(call('abs', ret), '/', dollars)
    excess = binary(ret, '-', mkt)
    ratio = lambda a, b: binary(a, '/', b)
    mul = lambda a, b: binary(a, '*', b)
    corr = lambda a, b, n: pair('correlation', a, b, n)
    week = lambda x: window('ts_sum', x, 5)
    unexplained = lambda n: binary(one, '-', mul(corr(ret, mkt, n), corr(ret, mkt, n)))
    resid_sum = lambda n: binary(window('ts_sum', ret, n), '-', mul(pair('ts_regression', ret, mkt, n), window('ts_sum', mkt, n)))
    resid_sd = lambda n: mul(window('stddev', ret, n), call('signedpower', call('abs', unexplained(n)), half))
    resid_sharpe = lambda n: window('delay', ratio(resid_sum(n), resid_sd(n)), 21)
    idio_var = lambda n: mul(window('ts_var', ret, n), unexplained(n))
    return [
        ('residual_momentum',
         'Standardized market-model residual return (residual Sharpe) over 6-1, 9-1 and 12-1 windows; beta, alpha and residual sd fit on the formation window; one-month skip.',
         'Blitz, Huij and Martens (2011, J. Empirical Finance) standardized residual momentum; Grundy and Martin (2001, RFS) factor-adjusted momentum.', [
             ('resid_sharpe_6_1', resid_sharpe(105)),
             ('resid_sharpe_9_1', resid_sharpe(168)),
             ('resid_sharpe_12_1', resid_sharpe(231))]),
        ('market_beta',
         'Trailing beta on mkt_ret: daily OLS slope, Frazzini-Pedersen three-day correlation times one-year volatility ratio, negative realized semibeta.',
         'Frazzini and Pedersen (2014, JFE) betting against beta; Bollerslev, Li, Patton and Quaedvlieg (2022, JFE) realized semibetas.', [
             ('beta_126', pair('ts_regression', ret, mkt, 126)),
             ('fp_beta_250_252', ratio(mul(corr(window('ts_sum', ret, 3), window('ts_sum', mkt, 3), 250), window('stddev', ret, 252)),
                                       window('stddev', mkt, 252))),
             ('neg_semibeta_189', ratio(window('ts_sum', mul(call('min', ret, zero), call('min', mkt, zero)), 189),
                                        window('ts_sum', mul(mkt, mkt), 189)))]),
        ('idiosyncratic_risk',
         'Changes in market-model residual variance (short vs long, quarter over quarter) and the unexplained variance share; residual-vol levels excluded to avoid re-loading v1 low-vol.',
         'Ang, Hodrick, Xing and Zhang (2006, JF) idiosyncratic volatility; Fu (2009, JFE) expected vs unexpected idiosyncratic volatility; Durnev, Morck, Yeung and Zarowin (2003, JAR).', [
             ('ivol_change_21_252', ratio(idio_var(21), idio_var(252))),
             ('ivol_change_63_qoq', ratio(idio_var(63), window('delay', idio_var(63), 63))),
             ('idio_share_252', unexplained(252))]),
        ('lottery_demand',
         'Volatility-scaled right tail (MAX / sigma at 21 and 63 sessions) and realized return skewness; raw MAX excluded to avoid re-loading v1 low-vol.',
         'Bali, Cakici and Whitelaw (2011, JFE) MAX; Amaya, Christoffersen, Jacobs and Vasquez (2015, JFE) realized skewness.', [
             ('scaled_max_21', ratio(window('ts_max', ret, 21), window('stddev', ret, 21))),
             ('scaled_max_63', ratio(window('ts_max', ret, 63), window('stddev', ret, 63))),
             ('skew_126', window('ts_skew', ret, 126))]),
        ('abnormal_volume',
         'Own-history split-adjusted share-volume shocks, (raw_close * volume) / close: weekly shock, monthly spike, quarterly trend; excludes the price-level change in v1 raw-dollar ratios.',
         'Gervais, Kaniel and Mingelgrin (2001, JF) high-volume return premium; Barber and Odean (2008, RFS) attention.', [
             ('volume_shock_5_63', ratio(window('ts_mean', adj_volume, 5), window('ts_mean', adj_volume, 63))),
             ('volume_spike_21_126', ratio(window('ts_max', adj_volume, 21), window('ts_mean', adj_volume, 126))),
             ('volume_trend_63_252', ratio(window('ts_mean', adj_volume, 63), window('ts_mean', adj_volume, 252)))]),
        ('illiquidity',
         'Illiquidity shock (21/252 Amihud ratio), Roll bid-ask-bounce autocorrelation and volume-signed market-adjusted reversal; Amihud levels excluded to avoid re-loading v1 size/liquidity.',
         'Amihud (2002, J. Financial Markets) unexpected illiquidity; Roll (1984, JF) implied spread; Pastor and Stambaugh (2003, JPE) volume-induced reversal.', [
             ('amihud_shock_21_252', ratio(window('ts_mean', amihud, 21), window('ts_mean', amihud, 252))),
             ('roll_autocorr_126', corr(ret, window('delay', ret, 1), 126)),
             ('signed_volume_reversal_126', corr(excess, window('delay', mul(call('sign', excess), ratio(dollars, window('ts_mean', dollars, 63))), 1), 126))]),
        ('comovement',
         'Down-minus-up correlation asymmetry, weekly correlation and lagged (price-delay) correlation with mkt_ret.',
         'Ang and Chen (2002, JFE) asymmetric correlations; Hou and Moskowitz (2005, RFS) price delay; Barberis, Shleifer and Wurgler (2005, JFE).', [
             ('corr_asymmetry_126', binary(corr(call('min', ret, zero), call('min', mkt, zero), 126), '-',
                                           corr(call('max', ret, zero), call('max', mkt, zero), 126))),
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


# Fix round 1 (review of 2c92d658), applied before any TRAIN measurement.
REVISIONS = [
    dict(finding='I1', change='abnormal_volume uses split-adjusted (raw_close * volume) / close instead of raw share volume'),
    dict(finding='I2', change='vec_avg(ret) replaced by the upstream mkt_ret field (previous-session members, broadcast to all present names)'),
    dict(finding='I3', change='residual_momentum is standardized residual Sharpe at 6-1, 9-1 and 12-1 (resmom_6_1/resmom_9_1 dropped)'),
    dict(finding='I4a', change='illiquidity: amihud_21/63/126 levels replaced by amihud_shock_21_252, roll_autocorr_126, signed_volume_reversal_126'),
    dict(finding='I4b', change='ivol_21/ivol_126 replaced by ivol_change_21_252 and ivol_change_63_qoq; max_ret_21 replaced by scaled_max_21'),
    dict(finding='M1', change='fp_beta volatility ratio uses 252 sessions (fp_beta_250_63 renamed fp_beta_250_252)'),
    dict(finding='M2', change='market_corr_63 replaced by corr_asymmetry_126 (down minus up correlation)'),
    dict(finding='M7', change='residual sd uses signedpower(abs(1 - rho^2), 0.5), keeping the alpha sign if rho rounds past 1'),
]


# ---- static validator ------------------------------------------------------
# Rows of atx-engine/src/alpha/registry.cpp builtin_ops() used by this library:
# name -> (min_arity, max_arity, OpCode). Lookback classes follow typecheck:
# shift ops add d; rolling ops add d - 1; everything else takes max(children).
REGISTRY = {
    'abs': (1, 1, 'Abs'), 'sign': (1, 1, 'Sign'), 'min': (2, 2, 'MinP'), 'max': (2, 2, 'MaxP'),
    'signedpower': (2, 2, 'Spow'), 'rank': (1, 1, 'CsRank'), 'delay': (2, 2, 'TsDelay'),
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
    """Fully parenthesized builder grammar -> (node, shape, prior bars, native prior bars, rendered text)."""
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
            return (('bin', op, left[0], right[0]), shape, max(left[2], right[2]), max(left[3], right[3]),
                    f'({left[4]} {op} {right[4]})')
        if tok == '-':  # native parser folds a negated literal into one literal
            tok = take()
            if not tok[0].isdigit():
                raise ValueError('unary minus only on literals here')
            return (('num', -float(tok)), 'scalar', 0, 0, f'-{tok}')
        if tok[0].isdigit():
            if not math.isfinite(float(tok)):
                raise ValueError('non-finite literal')
            return (('num', float(tok)), 'scalar', 0, 0, tok)
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
            lookback, native = max(a[2] for a in args), max(a[3] for a in args)
            if opcode in SHIFT_OPS | ROLLING_OPS:
                node = args[-1][0]
                if node[0] != 'num' or node[1] < 1 or node[1] != int(node[1]):
                    raise ValueError(f'{tok} window must be a positive integer literal')
                if any(a[1] == 'scalar' for a in args[:-1]):
                    raise ValueError(f'{tok} series operand is scalar')
                extra = int(node[1]) - (0 if opcode in SHIFT_OPS else 1)
                lookback, native = lookback + extra, native + extra
            elif opcode.startswith('Cs') and args[0][1] == 'scalar':
                raise ValueError(f'{tok} primary is scalar')
            rendered = f"{tok}({', '.join(a[4] for a in args)})"
            return (('call', opcode, *(a[0] for a in args)), 'panel', lookback, native, rendered)
        if tok not in FIELD_PRIOR_BARS:
            raise ValueError(f'undeclared field {tok!r}')
        return (('field', tok), 'panel', FIELD_PRIOR_BARS[tok], 0, tok)

    result = primary()
    if i != len(tokens) or result[4] != text:
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

    refs[intern(tree)] += 1
    live = peak = 0
    for children in kids:
        live += 1
        peak = max(peak, live)
        for c in children:
            refs[c] -= 1
            live -= refs[c] == 0
    return len(kids), peak


def validate(dsl: str, expected_lookback: int) -> dict:
    tree, shape, lookback, native, _ = parse(dsl)
    nodes, slots = peak_slots(tree)
    assert shape == 'panel' and lookback == expected_lookback <= MAX_PRIOR_BARS, (dsl, lookback, expected_lookback)
    assert native <= lookback and nodes <= MAX_SLOTS and slots <= MAX_SLOTS and len(dsl.encode()) <= MAX_DSL_BYTES, dsl
    return dict(prior_bars=lookback, native_prior_bars=native, dag_nodes=nodes, estimated_peak_slots=slots)


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
    assert [f['name'] for f in v1_library['fields']] == ['close', 'raw_close', 'volume']
    fields = v1_library['fields'] + [MKT_RET_FIELD]
    candidates, lineage, static = [], [], []
    for candidate, row in zip(v1_library['candidates'], v1_recipe['lineage'], strict=True):
        assert candidate['id'] == row['id'] and candidate['family'] == row['family']
        assert row['dsl_sha256'] == hashlib.sha256(candidate['dsl'].encode()).hexdigest()
        check = validate(candidate['dsl'], row['prior_bars'])
        static.append(check)
        candidates.append(candidate)
        lineage.append(dict(row, native_prior_bars=check['native_prior_bars'], origin='frozen_v1'))
    families = new_families()
    templates = []
    for family, _, _, family_templates in families:
        assert family not in v1_families and len(family_templates) == 3
        for template, base in family_templates:
            templates.append(dict(family=family, template=template, base_dsl=base.text, base_prior_bars=base.lookback))
            for smoothing in (21, 63):
                expression = window('decay_linear', call('rank', base), smoothing)
                assert expression.lookback <= MAX_PRIOR_BARS
                check = validate(expression.text, expression.lookback)
                static.append(check)
                candidate_id = f'{template}_s{smoothing}'
                candidates.append(dict(id=candidate_id, family=family, dsl=expression.text,
                                       horizons=[5, 21, 63], sign_policy='train-rank-ic21'))
                lineage.append(dict(id=candidate_id, family=family, template=template,
                                    smoothing_sessions=smoothing, prior_bars=expression.lookback,
                                    dsl_sha256=hashlib.sha256(expression.text.encode()).hexdigest(),
                                    native_prior_bars=check['native_prior_bars'], origin='new_v2'))
    ids = [c['id'] for c in candidates]
    assert len(candidates) == 96 and len(set(ids)) == 96 and len({c['dsl'] for c in candidates}) == 96
    assert all(re.fullmatch(r'[a-z0-9_]{1,64}', s) for s in ids + [c['family'] for c in candidates])
    assert candidates[:48] == v1_library['candidates']
    assert not any('vec_avg' in c['dsl'] for c in candidates)
    library = dict(schema='atx.dsl-ic-library/v1', id='price_volume_ic96_v2', fields=fields,
                   families=v1_library['families'] + [dict(id=f, description=d) for f, d, _, _ in families],
                   candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    fixed = ('The eight new families were fixed from published economic priors before any v2 measurement. Fix round 1 '
             're-selected templates structurally from an independent code review (split handling, market-return '
             'coverage, overlap with v1 momentum/low-risk/size tilts), still before any TRAIN measurement; no IC, return, '
             'turnover or correlation output was consulted to choose or tune them. TRAIN fits only per-candidate signs; '
             'validation is not used for selection.')
    recipe = dict(schema='atx.dsl-ic-experiment/v1', id='price_volume_ic96_v2_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        generation=dict(rule='frozen-v1-48-plus-eight-by-three-by-two-v2', revision='fix-round-1',
                        candidates=96, families=16, templates_per_family=3,
                        smoothing_sessions=[21, 63], wrapper='decay_linear(rank(base), s)', smoothing_exemptions=[],
                        smoothing_rationale='no family exempted: rank tames heavy-tailed bases (Amihud ratios, volume ratios, '
                                            'skew, volatility ratios) and two decay variants keep the v1 3x2 family budget; '
                                            'already-slow 126-252 session bases only spend lookback, checked <= 314',
                        seed=None, max_prior_bars=MAX_PRIOR_BARS, complete_observations=MAX_PRIOR_BARS + 1,
                        frozen_v1=dict(generator=V1_GENERATOR, library=V1_LIBRARY, library_sha256=V1_LIBRARY_SHA256,
                                       recipe=V1_RECIPE, recipe_sha256=V1_RECIPE_SHA256, candidates=48, families=8,
                                       copy='ids, families, family descriptions and DSL bytes identical; dsl_sha256 unchanged'),
                        new=dict(candidates=48, families=8, templates=24)),
        revisions=REVISIONS,
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False,
                           template_reselection='structural, from code review, before any TRAIN measurement', statement=fixed),
        family_priors=[dict(family=f, origin='frozen_v1', prior=d) for f, d in v1_families.items()] +
                      [dict(family=f, origin='new_v2', prior=p) for f, _, p, _ in families],
        templates=templates,
        lineage=lineage,
        data=dict(v1_recipe['data'], fields=fields, daily_return='ret = (close / delay(close, 1)) - 1',
                  market_return='mkt_ret panel field: ' + MKT_RET_FIELD['basis'],
                  split_adjusted_volume='(raw_close * volume) / close = volume / cumulative factor',
                  prior_bar_accounting='prior_bars charges mkt_ret one prior bar (d-1..d), the same as ret; '
                                       'native_prior_bars follows the native typecheck, which charges fields 0'),
        orientation=v1_recipe['orientation'],
        composition=dict(v1_recipe['composition'], family_weight='1/16', within_family_weight='1/6',
                         template_budget='1/3 of family (two variants each)', candidate_weight='1/96'),
        planned_turnover=v1_recipe['planned_turnover'],
        trials=dict(generated_candidates=96, reused_v1_candidates=48, new_candidates=48, orientation_fits=96,
                    inverse_additional_vm_evaluations=0, family_or_template_selection=False,
                    families_fixed_before_measurement=True, pre_measurement_template_revisions=1,
                    combined_recipes=1, validation_selection=False,
                    reruns='retain attempts; v1 attempts remain prior trials of the reused 48',
                    horizons_and_smoothers_are_independent_trials=False),
        static_validation=dict(rule='python re-parse: canonical round-trip, registry arity, declared fields, '
                                    'positive integer windows, lookback recomputed per typecheck, dag slot bound',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_native_prior_bars=max(s['native_prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               slot_estimate='post-order interning and refcount retirement; native --plan-only authoritative'),
        qualification=dict(native_parse_vm='pending root --plan-only compile check with mkt_ret field support',
                           empirical='unmeasured at source freeze',
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
