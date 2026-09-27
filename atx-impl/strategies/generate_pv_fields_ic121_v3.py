"""Deterministic 121-candidate research library v3; no prices, fitting, or random search.

Candidates 1-96 are price_volume_ic96_v2 (fix round 2), loaded from its generator
and pinned by library and recipe SHA256, so ids, families, DSL bytes and
dsl_sha256 values are unchanged and cached signals are reused. Candidates 97-121
add nine families over upstream point-in-time research fields (FINRA short
interest, vendor ATM implied volatility, vendor earnings flags, lagged shares
outstanding), declared from published priors in the T8 brief before any v3
measurement. Every new base is written prior-positive (the prior predicts higher
returns for higher values); TRAIN still fits each candidate's sign.

Run with --check to verify the committed exact JSON bytes. A static validator
re-parses every DSL string (grammar round-trip, registry arity, declared fields,
positive integer windows, prior-bar lookback, DAG slot bound); the native DSL
compiler remains authoritative.
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
V2_GENERATOR = 'generate_price_volume_ic96_v2.py'
V2_LIBRARY = 'price_volume_ic96_v2.json'
V2_RECIPE = 'price_volume_ic96_v2.recipe.json'
V2_LIBRARY_SHA256 = 'fd1e359b316842eeb04551f91135e2f1fb5e13bdc8e888d0450ddac509fe1389'
V2_RECIPE_SHA256 = 'b7bdf06dd68aa26858802b7a7edceebe26fb38b827a89789c83e77992d5b4c21'
LIBRARY = 'pv_fields_ic121_v3.json'
RECIPE = 'pv_fields_ic121_v3.recipe.json'
LIBRARY_ID = 'pv_fields_ic121_v3'
PRODUCER = 'atx-engine/tools/prepare_research_fields.py'
V2_CANDIDATES, NEW_CANDIDATES = 96, 25
MAX_PRIOR_BARS = 314  # strategy_ic_runner admits lookback <= score_begin - 63; v1/v2 bound kept
MAX_SLOTS = 64        # strategy_ic_runner rejects program.num_slots > 64
MAX_DSL_BYTES = 4096  # strategy_ic_runner rejects longer DSL text
MAX_FAMILIES = 32     # strategy_ic_runner rejects more declared families

# IV guard: values outside the open interval (IV_FLOOR, IV_CAP) become NaN through
# x + 0 * log((x - IV_FLOOR) * (IV_CAP - x)) (log of a negative is NaN, log(0) is
# -inf and 0 * -inf is NaN, NaN propagates), then ts_backfill carries the last
# valid guarded value for at most IV_BACKFILL sessions (vendor IV is often null).
IV_FLOOR, IV_CAP, IV_BACKFILL = '0.02', '5', 5
ANNUALIZE = '15.874507866387544'  # sqrt(252): daily sample sd -> annualized decimal vol, IV units
assert float(ANNUALIZE) == math.sqrt(252)

# Upstream research fields (prepare_research_fields.py FIELDS, exact names). Each is
# known at or before the session's close mark, the clock of close, so the DSL charges
# no prior bar; si_* are as-of joins strictly before the session date.
NEW_FIELDS = [
    dict(name='si_shares', basis=(
        'FINRA consolidated shares short (currentShortPositionQuantity); as-of: latest row with available_at (the official '
        'dissemination date) strictly before date(session), NaN when older than 45 calendar days; produced upstream')),
    dict(name='si_dtc', basis=(
        'FINRA days to cover (daysToCoverQuantity, floored at 1.00 by FINRA); same as-of clock and 45-day staleness as si_shares')),
    dict(name='iv_atm_21d', basis=(
        'vendor ATM implied volatility, annualized decimal (0.30 = 30%), 21-session constant maturity, earnings-adjusted clean IV; '
        'same-date end-of-day vendor row, known at the session close mark; null, non-finite or <= 0 -> NaN; vendor garbage '
        'remains (values far above 5), so candidates guard it')),
    dict(name='iv_atm_63d', basis='as iv_atm_21d, 63-session constant maturity'),
    dict(name='iv_atm_126d', basis='as iv_atm_21d, 126-session constant maturity'),
    dict(name='earn_recent', basis=(
        'indicator: 1 on the vendor earnings price-reaction session (earnFlag 0) and the session after (earnFlag 1), 0 otherwise '
        '(N and -1); same-date vendor row, known at the session close mark; NaN when the vendor row is absent')),
    dict(name='shares_out', basis=(
        'vendor shares outstanding (thousands x 1000) of the last line row dated <= date(session) - 90 calendar days (A8 lag), '
        'restated to the session share basis by the cumulReturnFactor ratio; NaN when that row is older than 490 days')),
]
NEW_FIELD_CLOCKS = {
    'si_shares': 'finra-asof: available_at (official dissemination date) < date(session), strict; age > 45 calendar days -> NaN',
    'si_dtc': 'finra-asof: available_at (official dissemination date) < date(session), strict; age > 45 calendar days -> NaN',
    'iv_atm_21d': 'vendor end-of-day row dated the session; known at the session close mark (the close clock)',
    'iv_atm_63d': 'vendor end-of-day row dated the session; known at the session close mark (the close clock)',
    'iv_atm_126d': 'vendor end-of-day row dated the session; known at the session close mark (the close clock)',
    'earn_recent': 'vendor end-of-day row dated the session; flags the reaction session (event public by its close) and the next',
    'shares_out': 'A8: vendor share count dated <= date(session) - 90 calendar days, restated by the session factor',
}


@functools.cache
def load_v2() -> tuple[ModuleType, bytes, bytes]:
    """The v2 generator module and its pinned library and recipe bytes (loaded once; importing this file loads nothing)."""
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('_frozen_generate_price_volume_ic96_v2', HERE / V2_GENERATOR)
    assert spec is not None and spec.loader is not None, V2_GENERATOR
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V2_LIBRARY], docs[V2_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V2_LIBRARY_SHA256, 'frozen v2 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V2_RECIPE_SHA256, 'frozen v2 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return module, library_bytes, recipe_bytes


def v2() -> ModuleType:
    return load_v2()[0]


def tables():
    """Validator tables: the v2 rows plus log and ts_backfill, and every v3 field."""
    m = v2()
    registry = dict(m.REGISTRY, log=(1, 1, 'Log'), ts_backfill=(2, 2, 'TsBackfill'))
    fields = dict(m.FIELD_PRIOR_BARS, **{f['name']: 0 for f in NEW_FIELDS})
    return registry, set(m.SHIFT_OPS), set(m.ROLLING_OPS) | {'TsBackfill'}, fields


# ---- new families --------------------------------------------------------------
def new_families():
    """(family, description, citation, prior sign, smoothing grid, [(template, base Expr, params)])."""
    m = v2()
    Expr, binary, call, window = m.Expr, m.binary, m.call, m.window
    close, raw = Expr('close'), Expr('raw_close')
    mkt = Expr('mkt_ret', m.FIELD_PRIOR_BARS['mkt_ret'])
    si, dtc, shares, earn = Expr('si_shares'), Expr('si_dtc'), Expr('shares_out'), Expr('earn_recent')
    iv = {21: Expr('iv_atm_21d'), 63: Expr('iv_atm_63d'), 126: Expr('iv_atm_126d')}
    one, zero, neg, floor, cap = Expr('1'), Expr('0'), Expr('-1'), Expr(IV_FLOOR), Expr(IV_CAP)
    ret = binary(binary(close, '/', window('delay', close, 1)), '-', one)  # v2 spelling, byte-identical
    excess = binary(ret, '-', mkt)
    flip = lambda x: binary(neg, '*', x)  # prior-positive orientation for a negative prior
    guarded = lambda x: binary(x, '+', binary(zero, '*', call('log', binary(binary(x, '-', floor), '*', binary(cap, '-', x)))))
    ivg = lambda tenor: window('ts_backfill', guarded(iv[tenor]), IV_BACKFILL)
    si_ratio = binary(si, '/', shares)
    realized = binary(window('stddev', ret, 21), '*', Expr(ANNUALIZE))
    car = lambda h: binary(window('ts_sum', binary(earn, '*', excess), h), '/', call('sign', window('ts_sum', earn, h)))
    slow, daily = (1, 21, 63), (21, 63)
    return [
        ('short_interest_ratio',
         'FINRA short interest over lagged shares outstanding, ranked level and smoothed level; high short interest predicts lower returns.',
         'Asquith, Pathak and Ritter (2005, JFE) short interest and institutional ownership; Boehmer, Huszar and Jordan (2010, JFE) the good news in short interest.',
         -1, slow, [('si_ratio', flip(si_ratio), dict())]),
        ('days_to_cover',
         'FINRA days to cover (short interest over average daily volume, floored at 1), ranked level and smoothed level; high days to cover predicts lower returns.',
         'Hong, Li, Ni, Scheinkman and Yan (2015, working paper) days to cover and stock returns.',
         -1, slow, [('dtc', flip(dtc), dict())]),
        ('short_interest_change',
         'Change in the short-interest ratio over 10, 21 and 42 sessions (about one, two and four FINRA cycles), smoothed; rising short interest predicts lower returns.',
         'Change-in-short-interest prior of the T8 brief, on the short-interest literature of Asquith, Pathak and Ritter (2005, JFE) and Boehmer, Huszar and Jordan (2010, JFE).',
         -1, (21,), [(f'si_change_{n}', flip(binary(si_ratio, '-', window('delay', si_ratio, n))), dict(change_sessions=n))
                     for n in (10, 21, 42)]),
        ('implied_vol_level',
         'Guarded 21-session ATM implied volatility level, ranked and smoothed; high implied volatility predicts lower returns.',
         'Ang, Hodrick, Xing and Zhang (2006, JF) volatility anomaly, implied-volatility analogue.',
         -1, daily, [('iv_level_21', flip(ivg(21)), dict(tenor=21))]),
        ('implied_vol_term_slope',
         'Guarded long-over-short ATM implied volatility ratio (63 or 126 over 21 sessions); an inverted term structure predicts lower returns.',
         'Vasquez (2017, JFQA) equity volatility term structures; Jiang and Tian-style implied-volatility slope.',
         1, daily, [(f'iv_term_{t}_21', binary(ivg(t), '/', ivg(21)), dict(long_tenor=t, short_tenor=21)) for t in (63, 126)]),
        ('implied_vol_change',
         'Guarded 21-session ATM implied volatility over its value 5 or 21 sessions earlier, smoothed; a sharp implied-volatility rise predicts lower returns.',
         'An, Ang, Bali and Cakici (2014, JF) the joint cross section of stocks and options (ATM approximation).',
         -1, daily, [(f'iv_change_{n}', flip(binary(ivg(21), '/', window('delay', ivg(21), n))), dict(tenor=21, change_sessions=n))
                     for n in (5, 21)]),
        ('implied_minus_realized_vol',
         'Guarded 21-session ATM implied volatility minus annualized 21-session realized volatility of daily returns, smoothed; a high spread predicts lower returns.',
         'Bali and Hovakimian (2009, Management Science) volatility spreads; Goyal and Saretto (2009, JFE) option returns and volatility.',
         -1, daily, [('iv_rv_21', flip(binary(ivg(21), '-', realized)), dict(tenor=21, realized_sessions=21))]),
        ('earnings_drift',
         'Market-relative return summed over vendor-flagged earnings reaction sessions (reaction day and next) within the last 21, 42 or 63 sessions; NaN without an event in the window; positive drift.',
         'Chan, Jegadeesh and Lakonishok (1996, JF) momentum strategies; Brandt, Kishore, Santa-Clara and Venkatachalam (2008, working paper) earnings announcement returns.',
         1, (1,), [(f'earn_car_{h}', car(h), dict(hold_sessions=h)) for h in (21, 42, 63)]),
        ('size',
         'Negative log market capitalization, lagged shares outstanding times raw close, ranked; small predicts higher returns (weak; overlaps the logADV63 neutralization).',
         'Banz (1981, JFE) the size effect.',
         -1, (21,), [('size_mcap', flip(call('log', binary(shares, '*', raw))), dict())]),
    ]


def wrap(base, smoothing: int):
    """W_s(rank(base)): s == 1 is the plain rank (a one-session decay is the identity)."""
    ranked = v2().call('rank', base)
    return ranked if smoothing == 1 else v2().window('decay_linear', ranked, smoothing)


# ---- static validator --------------------------------------------------------
def parse(text: str):
    """v2 grammar with the v3 tables -> (node, shape, prior bars, native prior bars, rendered text)."""
    registry, shift_ops, rolling_ops, field_bars = tables()
    tokens, i = v2().tokenize(text), 0

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
            if tok not in registry:
                raise ValueError(f'unregistered operator {tok!r}')
            low, high, opcode = registry[tok]
            if not low <= len(args) <= high:
                raise ValueError(f'arity {len(args)} for {tok}')
            lookback, native = max(a[2] for a in args), max(a[3] for a in args)
            if opcode in shift_ops | rolling_ops:
                node = args[-1][0]
                if node[0] != 'num' or node[1] < 1 or node[1] != int(node[1]):
                    raise ValueError(f'{tok} window must be a positive integer literal')
                if any(a[1] == 'scalar' for a in args[:-1]):
                    raise ValueError(f'{tok} series operand is scalar')
                extra = int(node[1]) - (0 if opcode in shift_ops else 1)
                lookback, native = lookback + extra, native + extra
            elif opcode.startswith('Cs') and args[0][1] == 'scalar':
                raise ValueError(f'{tok} primary is scalar')
            rendered = f"{tok}({', '.join(a[4] for a in args)})"
            return (('call', opcode, *(a[0] for a in args)), 'panel', lookback, native, rendered)
        if tok not in field_bars:
            raise ValueError(f'undeclared field {tok!r}')
        return (('field', tok), 'panel', field_bars[tok], 0, tok)

    result = primary()
    if i != len(tokens) or result[4] != text:
        raise ValueError(f'trailing tokens or non-canonical text: {text!r}')
    return result


def fields_of(node) -> set[str]:
    if node[0] == 'field':
        return {node[1]}
    if node[0] in ('call', 'bin'):
        return set().union(*(fields_of(c) for c in node[2:]))
    return set()


def validate(dsl: str, expected_lookback: int) -> dict:
    tree, shape, lookback, native, _ = parse(dsl)
    nodes, slots = v2().peak_slots(tree)
    assert shape == 'panel' and lookback == expected_lookback <= MAX_PRIOR_BARS, (dsl, lookback, expected_lookback)
    assert native <= lookback and nodes <= MAX_SLOTS and slots <= MAX_SLOTS and len(dsl.encode()) <= MAX_DSL_BYTES, dsl
    return dict(prior_bars=lookback, native_prior_bars=native, dag_nodes=nodes, estimated_peak_slots=slots,
                fields=sorted(fields_of(tree)))


def registry_crosscheck() -> str:
    registry, shift_ops, rolling_ops, _ = tables()
    engine = HERE.parent.parent / 'atx-engine'
    paths = [engine / 'src/alpha/registry.cpp', engine / 'src/alpha/typecheck.cpp',
             engine / 'include/atx/engine/alpha/typecheck.hpp']
    if not all(p.is_file() for p in paths):
        return 'registry cross-check skipped (engine sources absent)'
    source, typecheck, header = (p.read_text(encoding='utf-8') for p in paths)
    rows = {m[1]: (int(m[2]), int(m[3]), m[4])
            for m in re.finditer(r'\{"(\w+)",\s*(\d+),\s*(\d+),\s*OpCode::(\w+)', source)}
    body = typecheck.split('bool is_rolling_ts(OpCode op) noexcept {', 1)[1].split('return true;', 1)[0]
    rolling = set(re.findall(r'case OpCode::(\w+):', body))
    shift_body = re.search(r'is_shift_ts\(OpCode op\) noexcept \{(.*?)\}', header, re.S)
    if shift_body is None:
        raise SystemExit('typecheck.hpp is_shift_ts not found')
    shift = set(re.findall(r'OpCode::(\w+)', shift_body[1]))
    for name, sig in registry.items():
        if rows.get(name) != sig:
            raise SystemExit(f'registry row differs for {name}: {rows.get(name)} != {sig}')
        opcode = sig[2]
        if (opcode in rolling_ops) != (opcode in rolling) or (opcode in shift_ops) != (opcode in shift):
            raise SystemExit(f'lookback class differs for {name}')
    return f'registry cross-check ok ({len(registry)} operators vs registry.cpp/typecheck)'


# ---- documents ---------------------------------------------------------------
EXPECTED_TURNOVER = {
    'short_interest_ratio': 'very low: inputs move at FINRA dissemination (about every 10.5 sessions); the s1 plain rank steps on those sessions',
    'days_to_cover': 'very low: FINRA cycle updates only; the floor at 1 ties about 40% of rows',
    'short_interest_change': 'low: the change refreshes each FINRA cycle; s21 decay spreads each step over a month',
    'implied_vol_level': 'low: persistent level, s21/s63 decay',
    'implied_vol_term_slope': 'low to moderate: daily ratio of two persistent tenors, s21/s63 decay',
    'implied_vol_change': 'moderate, the fastest v3 family: iv_change_5_s21 is the fastest candidate; decay_linear 21 bounds a '
                          'white-noise base at lag-1 autocorrelation ~0.93 (Gaussian one-way tau ~0.37), below 0.70',
    'implied_minus_realized_vol': 'moderate: daily IV and 21-session realized vol, s21/s63 decay',
    'earnings_drift': 'low: the base is constant between events; names enter on the reaction session and leave after H sessions '
                      '(about 2/H of the covered names per session; fastest earn_car_21)',
    'size': 'very low: the market-cap rank moves with relative price and lagged share updates, s21 decay',
}
BORROW_EXPOSURE = {
    'short_interest_ratio': 'high: the prior short leg is high SI/shares_out, and SI/shares_out > 10% is a swap-fin-v1 special-tier flag',
    'days_to_cover': 'high: the prior short leg is heavily shorted, often small names (SI and mcap flags)',
    'short_interest_change': 'moderate: the short leg is rising short interest, partly already special-tier',
    'implied_vol_level': 'moderate: high-IV names skew to small caps and low prices (mcap < $1bn, price < $5, young listings)',
    'implied_vol_term_slope': 'low to moderate',
    'implied_vol_change': 'low to moderate',
    'implied_minus_realized_vol': 'moderate: high IV-RV spreads concentrate in small, hard-to-borrow names',
    'earnings_drift': 'low: shorts are negative-surprise names across sizes',
    'size': 'low if the prior sign holds (shorts big names); high if TRAIN flips the sign (shorts small names)',
}


# Where the cited evidence does not map one-to-one onto the brief's declared stock-return
# prior. The declared sign is kept (it is cosmetic for the runner: rank(-x) oriented by
# the TRAIN sign fit equals rank(x) oriented), but the caveat is recorded pre-measurement.
PRIOR_CAVEATS = {
    'implied_vol_term_slope': 'Vasquez (2017) documents the term-slope effect on option (straddle) returns; the stock-return '
                              'direction declared in the brief is an extrapolation',
    'implied_vol_change': 'An, Ang, Bali and Cakici (2014) find call-IV increases predict higher and put-IV increases lower '
                          'stock returns; for ATM (call/put blended) IV the direction is ambiguous a priori; the brief declares '
                          'rise -> lower',
    'implied_minus_realized_vol': 'Bali and Hovakimian (2009), as recalled by the implementer (unverified here), report a negative '
                                  'relation between the realized-minus-implied spread and stock returns, i.e. high IV-RV -> higher '
                                  'returns, opposite to the brief; Goyal and Saretto (2009) concern option returns; the brief sign is kept',
    'short_interest_change': 'the brief pairs ~2 and ~4 FINRA cycles with ~10 and ~21 sessions, but one cycle is ~10.5 sessions; '
                             'the grid 10/21/42 covers both readings (1, 2 and 4 cycles)',
}


def expected_overlap() -> list[dict]:
    rows = [
        ('iv_level_21', 'low_vol_63', 'frozen_v1', '0.7-0.85', 'implied and 63-session realized volatility levels rank alike'),
        ('iv_rv_21', 'iv_level_21', 'new_v3', '0.5-0.7', 'the spread shares the IV level term'),
        ('iv_change_21', 'vol_expansion_21_126', 'frozen_v1', '0.3-0.5', 'implied and realized volatility changes co-move'),
        ('dtc', 'si_ratio', 'new_v3', '0.6-0.8', 'days to cover is short interest over volume; the short ratio is over shares'),
        ('size_mcap', 'dollar_liquidity_63', 'frozen_v1', '0.85-0.95', 'market cap and dollar ADV rank alike; the construction '
                                                                          'also neutralizes logADV63'),
    ]
    return [dict(template=a, reference=b, reference_origin=o, prior_abs_rank_corr=c, basis=d) for a, b, o, c, d in rows]


def documents() -> dict[str, bytes]:
    _, library_bytes_v2, recipe_bytes_v2 = load_v2()
    v2_library, v2_recipe = json.loads(library_bytes_v2), json.loads(recipe_bytes_v2)
    v2_family_ids = [f['id'] for f in v2_library['families']]
    assert [f['name'] for f in v2_library['fields']] == ['close', 'raw_close', 'volume', 'mkt_ret']
    assert len(v2_library['candidates']) == V2_CANDIDATES and len(v2_recipe['lineage']) == V2_CANDIDATES
    candidates, lineage, static = [], [], []
    for candidate, row in zip(v2_library['candidates'], v2_recipe['lineage'], strict=True):
        assert candidate['id'] == row['id'] and candidate['family'] == row['family']
        assert row['dsl_sha256'] == hashlib.sha256(candidate['dsl'].encode()).hexdigest()
        check = validate(candidate['dsl'], row['prior_bars'])
        assert check['native_prior_bars'] == row['native_prior_bars']
        static.append(check)
        candidates.append(candidate)
        lineage.append(row)
    families = new_families()
    templates, priors, used_fields = [], [], set()
    for family, description, citation, sign, smoothing, family_templates in families:
        assert family not in v2_family_ids and sign in (-1, 1) and 1 <= len(family_templates) <= 4
        family_fields = set()
        for template, base, params in family_templates:
            base_check = parse(base.text)
            assert base_check[2] == base.lookback, (template, base_check[2], base.lookback)
            base_fields = sorted(fields_of(base_check[0]))
            family_fields |= set(base_fields)
            templates.append(dict(family=family, template=template, base_dsl=base.text, base_prior_bars=base.lookback,
                                  fields=base_fields, **params))
            for s in smoothing:
                expression = wrap(base, s)
                check = validate(expression.text, expression.lookback)
                static.append(check)
                candidate_id = f'{template}_s{s}'
                candidates.append(dict(id=candidate_id, family=family, dsl=expression.text,
                                       horizons=[5, 21, 63], sign_policy='train-rank-ic21'))
                lineage.append(dict(id=candidate_id, family=family, template=template, smoothing_sessions=s,
                                    prior_bars=expression.lookback,
                                    dsl_sha256=hashlib.sha256(expression.text.encode()).hexdigest(),
                                    native_prior_bars=check['native_prior_bars'], origin='new_v3',
                                    prior_sign=sign, fields=check['fields']))
        used_fields |= family_fields
        priors.append(dict(family=family, origin='new_v3', prior=citation, description=description, prior_sign=sign,
                           prior_sign_meaning='sign of the declared prior on the raw quantity; the DSL base is prior_sign * '
                                              'quantity, so a TRAIN sign of +1 agrees with the prior',
                           smoothing_sessions=list(smoothing), templates=[t for t, _, _ in family_templates],
                           fields=sorted(family_fields), expected_turnover=EXPECTED_TURNOVER[family],
                           borrow_exposure=BORROW_EXPOSURE[family], prior_caveat=PRIOR_CAVEATS.get(family)))
    total = V2_CANDIDATES + NEW_CANDIDATES
    ids = [c['id'] for c in candidates]
    assert len(candidates) == total and len(set(ids)) == total and len({c['dsl'] for c in candidates}) == total
    assert len({row['dsl_sha256'] for row in lineage}) == total
    assert all(re.fullmatch(r'[a-z0-9_]{1,64}', s) for s in ids + [c['family'] for c in candidates])
    assert candidates[:V2_CANDIDATES] == v2_library['candidates']
    assert lineage[:V2_CANDIDATES] == v2_recipe['lineage']
    assert not any('vec_avg' in c['dsl'] for c in candidates)
    template_ids = {row['template'] for row in lineage}
    assert all(o['template'] in template_ids and o['reference'] in template_ids for o in expected_overlap())
    new_names =[f['name'] for f in NEW_FIELDS]
    assert used_fields - {'close', 'raw_close', 'mkt_ret'} == set(new_names), used_fields  # declared == referenced
    fields = v2_library['fields'] + NEW_FIELDS
    family_rows = v2_library['families'] + [dict(id=f, description=d) for f, d, _, _, _, _ in families]
    assert len(family_rows) <= MAX_FAMILIES
    library = dict(schema='atx.dsl-ic-library/v1', id=LIBRARY_ID, fields=fields, families=family_rows, candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    new_static = static[V2_CANDIDATES:]
    extra_field_users = {f: [c['id'] for c, s in zip(candidates, static) if f in s['fields']] for f in ['mkt_ret'] + new_names}
    family_sizes = {f: sum(c['family'] == f for c in candidates) for f in [r['id'] for r in family_rows]}
    fixed = ('The nine new families, their templates and their window/smoothing grids were declared from published priors '
             'in the T8 brief before any v3 measurement; no IC, return, turnover or correlation output of any v3 candidate '
             'was consulted. The TRAIN fields manifest was read only for field names, units and coverage. TRAIN fits only '
             'per-candidate signs; validation is not used for selection.')
    recipe = dict(schema='atx.dsl-ic-experiment/v1', id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        generation=dict(rule='frozen-v2-96-plus-nine-field-families-v3', revision='initial',
                        candidates=total, families=len(family_rows),
                        wrapper='W_s(rank(base)): s = 1 -> rank(base); s > 1 -> decay_linear(rank(base), s)',
                        smoothing_grids=dict(slow_inputs=[1, 21, 63], daily_inputs=[21, 63], short_interest_change=[21],
                                             earnings_drift=[1], size=[21]),
                        smoothing_rationale=(
                            'slow inputs (FINRA short interest levels) keep a plain-rank variant because they move only at '
                            'dissemination; daily inputs (IV level, slope, change, IV-RV) require decay (tau_k <= 0.70); '
                            'earnings_drift takes no decay because its base is NaN outside the hold window and a full-window '
                            'decay would blank it; one size variant because a second decay of a near-constant rank is a '
                            'duplicate trial'),
                        prior_orientation='each new base is written prior-positive (a negative prior is multiplied by -1); '
                                          'the TRAIN rank-IC21 sign fit still orients every candidate',
                        seed=None, max_prior_bars=MAX_PRIOR_BARS, complete_observations=MAX_PRIOR_BARS + 1,
                        frozen_v2=dict(generator=V2_GENERATOR, library=V2_LIBRARY, library_sha256=V2_LIBRARY_SHA256,
                                       recipe=V2_RECIPE, recipe_sha256=V2_RECIPE_SHA256, revision='fix-round-2',
                                       candidates=V2_CANDIDATES, families=len(v2_family_ids),
                                       copy='ids, families, family descriptions, field declarations and DSL bytes identical; '
                                            'dsl_sha256 and lineage rows unchanged'),
                        new=dict(candidates=NEW_CANDIDATES, families=len(families), templates=len(templates)),
                        generator_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest()),
        iv_guard=dict(rule=f'ts_backfill(x + 0 * log((x - {IV_FLOOR}) * ({IV_CAP} - x)), {IV_BACKFILL})',
                      admitted=f'open interval ({IV_FLOOR}, {IV_CAP}); outside it (and at its ends) the cell is NaN before the backfill',
                      backfill=f'last valid guarded value within {IV_BACKFILL} sessions (t-{IV_BACKFILL - 1}..t), else NaN; never zero',
                      rationale='vendor IV holds garbage (iv_atm_126d member values up to 1.2e16, IV maxima ~69) and is often '
                                'null; rank after the guard so no single cell dominates; a full-window ts op over sporadic '
                                'nulls would otherwise blank the name',
                      applies_to=['implied_vol_level', 'implied_vol_term_slope', 'implied_vol_change', 'implied_minus_realized_vol']),
        missing_policy='NaN propagates: a candidate excludes the name that session; no zero fill (the IV backfill carries a '
                       'past valid value only); earnings_drift is NaN (not zero) without an event in its window',
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=fixed),
        family_priors=v2_recipe['family_priors'] + priors,
        templates=v2_recipe['templates'] + templates,
        lineage=lineage,
        expected_overlap=dict(rule='pre-measurement prior rank correlations (v2 pairs from review arithmetic; v3 pairs are the '
                                   'implementer priors); not measured, not a selection input; the admission screen culls '
                                   'empirical duplicates',
                              pairs=v2_recipe['expected_overlap']['pairs'] + expected_overlap()),
        data=dict(v2_recipe['data'], fields=fields, producer=PRODUCER,
                  field_clocks=NEW_FIELD_CLOCKS,
                  causality='row d of a signal uses fields known by the session-d close mark (the v2 close clock) and is '
                            'scored against returns after d; si_* are strictly before date(d); shares_out lags 90 days',
                  short_interest_ratio='si_shares / shares_out; a split between the FINRA settlement and the session '
                                       'misstates it by the split factor until the next dissemination',
                  short_interest_change='si_change_10 spans about one FINRA cycle: on sessions with no dissemination in '
                                        '(t-10, t] the change is zero for most names (tied ranks) and the s21 decay carries '
                                        'the previous cycle',
                  vendor_caveats=['iv_atm_*: vendor clean IV removes the earnings effect with the vendor earnings calendar '
                                  '(forward-looking by construction; calendar vintage unproven); no vintage proof',
                                  'earn_recent: vendor calendar vintage unproven; the reaction session is public by its close',
                                  'si_*: settlements before 2021-06 come from a later FINRA consolidated republication'],
                  size='shares_out * raw_close (USD); mktcap_lagged is not used (about 75% member coverage vs 99.7%)',
                  earnings_window='earn_recent flags the reaction session and the next; the event return is '
                                  'earn_recent * (ret - mkt_ret) summed over the hold window',
                  realized_vol=f'stddev(ret, 21) * {ANNUALIZE} (sqrt 252), annualized decimal like the IV fields',
                  unused_fields=['mktcap_lagged', 'size_grp', 'is_common'],
                  unused_field_reasons='mktcap_lagged/size_grp: lower coverage than shares_out * raw_close; is_common: '
                                       'static, not point-in-time'),
        orientation=v2_recipe['orientation'],
        composition=dict(v2_recipe['composition'], family_weight=f'1/{len(family_rows)}',
                         within_family_weight='1/(candidates in the family)', template_budget='equal within family',
                         candidate_weight='1/(families * candidates in the family)', family_sizes=family_sizes),
        planned_turnover=v2_recipe['planned_turnover'],
        trials=dict(generated_candidates=total, reused_v2_candidates=V2_CANDIDATES, new_candidates=NEW_CANDIDATES,
                    orientation_fits=total, inverse_additional_vm_evaluations=0, family_or_template_selection=False,
                    families_fixed_before_measurement=True, combined_recipes=1, validation_selection=False,
                    reruns='retain attempts; v1/v2 attempts remain prior trials of the reused 96',
                    each_new_candidate_is_one_admission_trial=True, horizons_and_smoothers_are_independent_trials=False),
        static_validation=dict(rule='python re-parse: canonical round-trip, registry arity, declared fields, positive integer '
                                    'windows, lookback recomputed per typecheck, dag slot bound',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_native_prior_bars=max(s['native_prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               new_max_prior_bars=max(s['prior_bars'] for s in new_static),
                               new_max_dag_nodes=max(s['dag_nodes'] for s in new_static),
                               new_max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in new_static),
                               extra_field_candidates=sum(1 for s in static if set(s['fields']) & set(extra_field_users)),
                               extra_field_users=extra_field_users,
                               slot_estimate='post-order interning and refcount retirement; native --plan-only authoritative'),
        qualification=dict(native_parse_vm='pending root --plan-only compile check with --train-fields support (T7)',
                           empirical='unmeasured at source freeze',
                           low_turnover='smoothing intent; actual tau_k measured at admission',
                           prior_phase='v1 and v2 files and generators are unchanged by v3'))
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
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
    library, recipe = json.loads(docs[LIBRARY]), json.loads(docs[RECIPE])
    new = library['candidates'][V2_CANDIDATES:]
    print(f"candidates {len(library['candidates'])} (frozen v2 {V2_CANDIDATES} byte-identical, new {len(new)}); "
          f"families {len(library['families'])}; max prior bars {recipe['static_validation']['max_prior_bars']} "
          f"(new {recipe['static_validation']['new_max_prior_bars']})")
    for field, users in recipe['static_validation']['extra_field_users'].items():
        print(f'  {field}: {len(users)} candidates ({users[0]} .. {users[-1]})')
    print(registry_crosscheck())


if __name__ == '__main__':
    main()
