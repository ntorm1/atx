"""Deterministic 37-candidate research library v4: prior-signed, themed; no prices, fitting or random search.

Library v4 (T24) implements the v4 pre-registration section R1: nine themes, one canonical
variant per published hypothesis, the literature sign embedded in the DSL (a higher value is
the predicted long side, so every candidate carries prior_sign +1), and firm-level accounting
ratios of themes 1-3 ranked within industry (group_rank on grp_ff12). Each candidate records
theme, tier, prior_sign and citation in its library row and in the recipe lineage; the T23
fitter reads them (admission v4-prior-v1, composition ew-theme-v1). Definitions follow the
cited papers. No v3 TRAIN performance output (admission tables, daily IC, summary IC
statistics, v3 DSL catalogue) was read to choose any definition, window or smoothing.

The static validator is the v2/v3 validator lineage: the tokenizer, expression builders and
the DAG slot estimate are imported from the pinned v2 generator, and the v3 parse is extended
with the native dtype rule (typecheck.cpp): a group field (grp_*, typed Group by T22) is a Group
classifier that may appear only as the second argument of a group operator. It re-parses every
DSL string (canonical round-trip, registry arity, declared fields, dtype, positive integer
windows, prior-bar lookback, DAG slot bound). The native DSL compiler remains authoritative:
group operators on grp_* compile only with T22 (is_group_field accepts the grp_ prefix); the
registry cross-check reads the engine sources of this tree (or --engine-root) and reports it.

Fix round 1 (controller rulings declared before any v4 TRAIN read): iv_change is dropped (blended
ATM IV mixes the opposite-signed call-IV and put-IV effects, so there is no unambiguous prior);
droe takes the canonical form with the new field be_lag1q_lag4.
Fix round 2 (review of fix round 1 plus controller ruling, before any v4 TRAIN read): mgmt_sy and
qmj_lite are dropped (their components are members of the same themes, and they alone push the IC
runner plan over its memory cap); issuance_vendor is split-neutral (Daniel-Titman composite
issuance); ear carries the most recent announcement's CAR; seasonality is re-centred under s21.

Run with --check to verify the committed exact JSON bytes.
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
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V2_GENERATOR = 'generate_price_volume_ic96_v2.py'  # grammar helpers only (tokenize, builders, peak_slots)
V2_GENERATOR_SHA256_LF = 'f4f1cb5838d86c4e30b4698d4e37c90469b2eb5cfb32b2bf622e126455afff4f'
LIBRARY = 'fund_industry_ic_v4.json'
RECIPE = 'fund_industry_ic_v4.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v4'
PRODUCER = 'atx-engine/tools/prepare_research_fields.py'
PREREG = '.superpowers/sdd/mega-alpha-20260926/v4-prereg.md section R1 (declared 2026-09-27)'
MAX_PRIOR_BARS = 314   # strategy_ic_runner admits lookback <= score_begin - 63; v1-v3 bound kept
MAX_SLOTS = 64         # strategy_ic_runner rejects program.num_slots > 64
MAX_DSL_BYTES = 4096   # strategy_ic_runner rejects longer DSL text
MAX_FAMILIES = 32      # strategy_ic_runner rejects more declared families
MAX_EXTRA_FIELDS = 64  # strategy_ic_runner: a library may reference at most 64 extra fields
SMOOTHING = 21         # prereg R1: s21 smoothing default
EAR_CARRY = 126        # ear: carry the most recent announcement CAR at most six months (CJL 1996 holding period)
WITHIN_INDUSTRY = 'grp_ff12'  # prereg R1: within-industry ranking of themes 1-3
FINE_INDUSTRY = 'grp_ff49'    # industry-level price members (T18 section 6 sketches)
BASE_FIELDS = ('close', 'raw_close', 'volume')  # mandatory runner declarations

# IV guard (unchanged from v3; a data guard, not a variant): values outside the open interval
# (IV_FLOOR, IV_CAP) become NaN through x + 0 * log((x - IV_FLOOR) * (IV_CAP - x)), then
# ts_backfill carries the last valid guarded value for at most IV_BACKFILL sessions.
IV_FLOOR, IV_CAP, IV_BACKFILL = '0.02', '5', 5
ANNUALIZE = '15.874507866387544'  # sqrt(252): daily sample sd -> annualized decimal vol, IV units
assert float(ANNUALIZE) == math.sqrt(252)

# Prior tiers (T18 section 6; tier-1 alpha-priors doc for the members T18 did not grade).
# tier_rank orders admission redundancy (prereg R3: ordered by (tier, roster order)); 1 = strongest.
TIER_RANK = {'A': 1, 'A-': 2, 'B+': 3, 'B': 4, 'B-': 5, 'C+': 6}

THEMES = [  # (theme id, description), prereg R1 order
    ('value', 'Price-scaled fundamentals (book, earnings, cash flow, free cash flow, EBIT/EV, net payout, sales, R&D), '
              'ranked within FF12 industry; high value predicts higher returns.'),
    ('profitability_quality', 'Profitability and quality (gross, operating, cash, ROE, ROA, low accruals, F-score), '
                              'ranked within FF12 industry; high quality predicts higher returns.'),
    ('investment_issuance', 'Low asset growth, low net operating assets and low share issuance (XBRL and split-neutral '
                            'vendor), ranked within FF12 industry; low investment and issuance predict higher returns.'),
    ('earnings_momentum', 'Earnings news: SUE, change in ROE, change in tax expense and the 3-day earnings '
                          'announcement return; good news predicts continuation.'),
    ('price_momentum', '12-1 momentum, industry 12-1 momentum, within-industry momentum and nearness to the 52-week '
                       'high; winners continue.'),
    ('low_risk', 'Low beta, low idiosyncratic volatility, low MAX and within-industry low volatility; low risk '
                 'predicts higher risk-adjusted returns.'),
    ('short_interest', 'FINRA short interest ratio, days to cover and one-month change in short interest; heavy '
                       'or rising shorting predicts lower returns.'),
    ('reversal_seasonality', 'Industry-adjusted short-term reversal and same-calendar-month seasonality.'),
    ('options_implied', 'Implied-minus-realized volatility spread; high implied volatility relative to realized '
                        'volatility predicts higher returns.'),
]
THEME_IDS = [t for t, _ in THEMES]
WITHIN_INDUSTRY_THEMES = ('value', 'profitability_quality', 'investment_issuance')
# Prereg R1: ids of themes 1-3 that are explicitly industry-level keep a cross-sectional rank.
# The v4 roster has none (T18's bm_ind / gpa_ind industry variants are not in the roster: the
# within-industry rule covers them), so the sole exception list is empty.
WITHIN_INDUSTRY_EXCEPTIONS: list[str] = []

# ---- fields ----------------------------------------------------------------
FUND_CLOCK = ('T21 fund group over the T20 atx.fundamental-events/v1 contract: the latest event row (row-level, '
              'latest clock wins; restatements enter at the restating filing\'s clock) of the session line\'s T19 '
              'primary-linked CIK whose clock (FSDS accepted_utc; FC1 fallback filed + 46 h, labelled) is before the '
              'session close mark, usable from the next session (--fund-lag-sessions 1); every item of a row shares '
              'its anchor period_end; the row is stale (all NaN) when date(session) - period_end exceeds 200 days '
              '(quarterly filer) or 400 days (annual-only); no link or no row -> NaN; values modeled/unaccepted')
PLANNED = 'planned fields-v5 name (T18 section 6, produced by T21); the producer manifest is authoritative'


@dataclass(frozen=True)
class Field:
    name: str
    basis: str
    clock: str
    origin: str          # base | fields_v4 | fields_v5
    group: bool = False  # Group classifier (typecheck dtype Group after T22)
    prior_bars: int = 0  # prior bars charged beyond the session (mkt_ret: d-1..d)


def _fund(name: str, item: str) -> Field:
    return Field(name, f'{item}; {PLANNED}', FUND_CLOCK, 'fields_v5')


FIELDS = [
    Field('close', 'role adjusted close (split and dividend continuous)', 'session close mark', 'base'),
    Field('raw_close', 'role raw close (unadjusted)', 'session close mark', 'base'),
    Field('volume', 'role raw share volume', 'session close mark', 'base'),
    Field('mkt_ret', ('equal-weight mean adjusted close-to-close return (close[d] / close[d-1] - 1) over the previous '
                      'session\'s (d-1) as-of members, broadcast to every name present at d; produced upstream'),
          'role-derived at the session close mark (charged one prior bar, like ret)', 'fields_v4', prior_bars=1),
    Field('si_shares', ('FINRA consolidated shares short (currentShortPositionQuantity); as-of: latest row with '
                        'available_at strictly before date(session), NaN when older than 45 calendar days'),
          'finra-asof: available_at < date(session), strict; age > 45 calendar days -> NaN', 'fields_v4'),
    Field('si_dtc', 'FINRA days to cover (daysToCoverQuantity, floored at 1.00 by FINRA); as si_shares',
          'finra-asof: available_at < date(session), strict; age > 45 calendar days -> NaN', 'fields_v4'),
    Field('iv_atm_21d', ('vendor ATM implied volatility, annualized decimal, 21-session constant maturity; producer '
                         'domain [0.02, 5] else NaN; candidates also apply the v3 guard'),
          'vendor end-of-day row dated the session; known at the session close mark', 'fields_v4'),
    Field('earn_recent', ('indicator: 1 on the vendor earnings price-reaction session (earnFlag 0) and the session '
                          'after (earnFlag 1), 0 otherwise; NaN when the vendor row is absent'),
          'vendor end-of-day row dated the session; the reaction session is public by its close', 'fields_v4'),
    Field('shares_out', ('vendor shares outstanding of the last line row dated <= date(session) - 90 calendar days '
                         '(A8 lag), restated to the session share basis'),
          'A8: vendor share count dated <= date(session) - 90 calendar days, restated by the session factor',
          'fields_v4'),
    _fund('be', 'book equity at the anchor period_end A: stockholders equity (else equity including minority interest '
                'minus minority interest) minus preferred stock, USD'),
    _fund('at', 'total assets at A, USD'),
    _fund('at_lag4', 'total assets at the balance date nearest A - 365 d, USD'),
    _fund('che', 'cash and short-term investments at A, USD'),
    _fund('debt', 'short-term plus long-term debt at A, each zero-filled (flagged) when absent while assets exist, USD'),
    _fund('sale_ttm', 'revenue, trailing twelve months ending at A, USD'),
    _fund('gp_ttm', 'gross profit (else revenue minus cost of revenue), trailing twelve months, USD'),
    _fund('oi_ttm', 'operating income (EBIT), trailing twelve months, USD'),
    _fund('ni_ttm', 'net income, trailing twelve months, USD'),
    _fund('ni_q', 'net income of the fiscal quarter ending at A, USD'),
    _fund('ni_q_lag4', 'net income of the fiscal quarter ending near A - 365 d, USD'),
    _fund('be_lag1q', 'book equity at the balance date nearest A - 91 d (opening equity of the quarter), USD'),
    _fund('be_lag1q_lag4', 'book equity at the balance date nearest A - 456 d (opening equity of the quarter one year '
                           'earlier), USD; added in fix round 1'),
    _fund('cfo_ttm', 'net cash from operating activities, trailing twelve months, USD'),
    _fund('capx_ttm', 'capital expenditure (payments for PP&E, positive), trailing twelve months, USD; NaN when not '
                      'reported'),
    _fund('xrd_ttm', 'research and development expense, trailing twelve months, USD; NaN when not reported'),
    _fund('dvc_ttm', 'common dividends paid, trailing twelve months, USD; zero-filled (flagged) when absent'),
    _fund('prstkc_ttm', 'purchases of common stock (buybacks), trailing twelve months, USD; zero-filled (flagged) '
                        'when absent'),
    _fund('sstk_ttm', 'proceeds from issuance of common stock, trailing twelve months, USD; zero-filled (flagged) '
                      'when absent'),
    _fund('txt_q', 'income tax expense of the fiscal quarter ending at A, USD'),
    _fund('txt_q_lag4', 'income tax expense of the fiscal quarter ending near A - 365 d, USD'),
    _fund('shrs_q', 'weighted-average diluted shares (else basic) of the fiscal quarter ending at A (else the fiscal '
                    'year ending at A), shares; as reported, split-restated by the reporting filing'),
    _fund('shrs_q_lag4', 'the same concept and duration class ending near A - 365 d, as known at the row clock (latest '
                         'filing reporting that period, so the anchor filing\'s split-restated comparative); the pair is '
                         'NaN when abs(log10 ratio) >= 2 (XBRL scale error)'),
    _fund('noa', 'net operating assets at A: at - che - lt + debt (Hirshleifer, Hou, Teoh and Zhang 2004), USD'),
    _fund('sue', 'standardized unexpected earnings of the quarter ending at A: seasonal random walk on first-reported '
                 'quarterly net income, (NI_q - NI_q-4) / sd of the previous <= 8 seasonal differences (>= 4 required)'),
    _fund('fscore', 'Piotroski (2000) F-score 0-9 at A, all nine signals required, producer-computed'),
    Field('me_company', ('company market equity: sum over the issuer\'s role lines of shares_out x raw_close (NaN if '
                         f'any linked line is NaN), USD; {PLANNED}'),
          'T21: session close mark (shares_out keeps its A8 lag)', 'fields_v5'),
    Field('grp_ff12', f'Fama-French 12-industry code of the latest visible SUB SIC (T21 grp group); {PLANNED}',
          'T21: latest visible FSDS SUB SIC event, 550-day staleness -> NaN; NaN label excluded from group statistics',
          'fields_v5', group=True),
    Field('grp_ff49', f'Fama-French 49-industry code of the latest visible SUB SIC (T21 grp group); {PLANNED}',
          'T21: latest visible FSDS SUB SIC event, 550-day staleness -> NaN; NaN label excluded from group statistics',
          'fields_v5', group=True),
]
FIELD_BY_NAME = {f.name: f for f in FIELDS}


# ---- pinned grammar helpers (v2 generator) ---------------------------------
@functools.cache
def grammar() -> ModuleType:
    """The v2 generator module, pinned by LF-normalized source SHA-256 (its documents() is never called)."""
    blob = (HERE / V2_GENERATOR).read_bytes().replace(b'\r\n', b'\n')
    assert hashlib.sha256(blob).hexdigest() == V2_GENERATOR_SHA256_LF, 'pinned v2 grammar helpers changed'
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('_frozen_v2_grammar_for_v4', HERE / V2_GENERATOR)
    assert spec is not None and spec.loader is not None, V2_GENERATOR
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


# Rows of atx-engine/src/alpha/registry.cpp builtin_ops() used by this library:
# name -> (min_arity, max_arity, OpCode). Lookback classes follow typecheck.
REGISTRY = {
    'abs': (1, 1, 'Abs'), 'log': (1, 1, 'Log'), 'signedpower': (2, 2, 'Spow'),
    'rank': (1, 1, 'CsRank'), 'group_rank': (2, 2, 'CsRankG'), 'group_neutralize': (2, 2, 'CsNeutG'),
    'group_mean': (2, 2, 'CsMeanG'),
    'delay': (2, 2, 'TsDelay'), 'ts_sum': (2, 2, 'TsSum'), 'stddev': (2, 2, 'TsStd'), 'ts_max': (2, 2, 'TsMax'),
    'ts_backfill': (2, 2, 'TsBackfill'), 'decay_linear': (2, 2, 'TsDecayLinear'), 'correlation': (3, 3, 'TsCorr'),
}
SHIFT_OPS = {'TsDelay'}
ROLLING_OPS = {'TsSum', 'TsStd', 'TsMax', 'TsBackfill', 'TsDecayLinear', 'TsCorr'}
GROUP_OPS = {'CsRankG', 'CsNeutG', 'CsMeanG'}  # typecheck needs_group_arg: 2nd argument must be a Group


def is_group_field(name: str) -> bool:
    """typecheck.hpp is_group_field after T22: 'sector', or the IndClass. / grp_ prefix with a non-empty suffix."""
    return name == 'sector' or any(len(name) > len(p) and name.startswith(p) for p in ('IndClass.', 'grp_'))


# ---- static validator --------------------------------------------------------
def parse(text: str, fields: dict[str, Field] | None = None):
    """v3 grammar + dtype rule -> (node, shape, dtype, prior bars, native prior bars, rendered text)."""
    field_table = FIELD_BY_NAME if fields is None else fields
    tokens, i = grammar().tokenize(text), 0

    def take(expected: str | None = None) -> str:
        nonlocal i
        if i >= len(tokens) or (expected is not None and tokens[i] != expected):
            raise ValueError(f'expected {expected!r} at token {i}: {text!r}')
        i += 1
        return tokens[i - 1]

    def numeric(arg, what: str) -> None:
        if arg[2] != 'f64':
            raise ValueError(f'{what} requires a numeric (f64) operand, got a Group classifier: {text!r}')

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
            while i < len(tokens) and tokens[i] == ',':  # at the end, take(')') raises ValueError
                take(',')
                args.append(primary())
            take(')')
            if tok not in REGISTRY:
                raise ValueError(f'unregistered operator {tok!r}')
            low, high, opcode = REGISTRY[tok]
            if not low <= len(args) <= high:
                raise ValueError(f'arity {len(args)} for {tok}')
            lookback, native = max(a[3] for a in args), max(a[4] for a in args)
            if opcode in GROUP_OPS:
                numeric(args[0], tok)
                if args[0][1] == 'scalar':
                    raise ValueError(f'{tok} primary is scalar')
                if args[1][2] != 'group':
                    raise ValueError(f'{tok} requires a Group classifier 2nd argument: {text!r}')
            else:
                for a in args:
                    numeric(a, tok)
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
            rendered = f"{tok}({', '.join(a[5] for a in args)})"
            return (('call', opcode, *(a[0] for a in args)), 'panel', 'f64', lookback, native, rendered)
        if tok not in field_table:
            raise ValueError(f'undeclared field {tok!r}')
        field = field_table[tok]
        if field.group != is_group_field(tok):
            raise ValueError(f'field {tok!r} group declaration disagrees with is_group_field')
        return (('field', tok), 'panel', 'group' if field.group else 'f64', field.prior_bars, 0, tok)

    result = primary()
    if i != len(tokens) or result[5] != text:
        raise ValueError(f'trailing tokens or non-canonical text: {text!r}')
    if result[2] != 'f64':
        raise ValueError(f'a Group classifier cannot be a signal root: {text!r}')
    return result


def fields_of(node) -> set[str]:
    if node[0] == 'field':
        return {node[1]}
    if node[0] in ('call', 'bin'):
        return set().union(*(fields_of(c) for c in node[2:]))
    return set()


def opcodes_of(node) -> set[str]:
    if node[0] == 'call':
        return {node[1]}.union(*(opcodes_of(c) for c in node[2:]))
    if node[0] == 'bin':
        return opcodes_of(node[2]) | opcodes_of(node[3])
    return set()


def validate(dsl: str, expected_lookback: int) -> dict:
    tree, shape, _, lookback, native, _ = parse(dsl)
    nodes, slots = grammar().peak_slots(tree)
    assert shape == 'panel' and lookback == expected_lookback <= MAX_PRIOR_BARS, (dsl, lookback, expected_lookback)
    assert native <= lookback and nodes <= MAX_SLOTS and slots <= MAX_SLOTS and len(dsl.encode()) <= MAX_DSL_BYTES, dsl
    fields = sorted(fields_of(tree))
    return dict(prior_bars=lookback, native_prior_bars=native, dag_nodes=nodes, estimated_peak_slots=slots,
                fields=fields, extra_fields=[f for f in fields if f not in BASE_FIELDS])


def registry_crosscheck(engine_root: Path | None = None) -> str:
    engine = (HERE.parent.parent if engine_root is None else Path(engine_root)) / 'atx-engine'
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
    group_body = re.search(r'needs_group_arg\(OpCode op\) noexcept \{(.*?)\}', header, re.S)
    if shift_body is None or group_body is None:
        raise SystemExit('typecheck.hpp is_shift_ts / needs_group_arg not found')
    shift = set(re.findall(r'OpCode::(\w+)', shift_body[1]))
    group = set(re.findall(r'OpCode::(\w+)', group_body[1]))
    for name, sig in REGISTRY.items():
        if rows.get(name) != sig:
            raise SystemExit(f'registry row differs for {name}: {rows.get(name)} != {sig}')
        opcode = sig[2]
        if (opcode in ROLLING_OPS) != (opcode in rolling) or (opcode in SHIFT_OPS) != (opcode in shift):
            raise SystemExit(f'lookback class differs for {name}')
        if (opcode in GROUP_OPS) != (opcode in group):
            raise SystemExit(f'group-argument class differs for {name}')
    return f'registry cross-check ok ({len(REGISTRY)} operators vs registry.cpp/typecheck); {group_field_status(header)}'


def group_field_status(header: str) -> str:
    """Static check of typecheck.hpp is_group_field: does it type grp_<suffix> as a Group classifier (T22)?"""
    match = re.search(r'is_group_field\(std::string_view name\) noexcept \{(.*?)\n\}', header, re.S)
    if match is None:
        raise SystemExit('typecheck.hpp is_group_field not found')
    body = match[1]
    prefix = re.search(r'constexpr std::string_view (\w+) = "grp_";', body)
    checked = prefix is not None and re.search(
        rf'name\.size\(\) > {prefix[1]}\.size\(\) && name\.starts_with\({prefix[1]}\)', body) is not None
    return ('grp_ group typing checked: typecheck.hpp is_group_field accepts grp_<suffix> (T22)' if checked
            else 'grp_ group typing PENDING T22 (native compile of group operators on grp_* needs it)')


# ---- candidates --------------------------------------------------------------
@dataclass(frozen=True)
class Spec:
    id: str
    theme: str
    tier: str
    base: object              # grammar().Expr, sign-embedded (prior-positive) base quantity
    ranking: str              # within_industry_grp_ff12 | cross_section
    citation: str
    formula: str              # the canonical quantity in words/symbols (raw, before the sign)
    raw_prior_direction: int  # literature sign of the raw quantity; base = raw_prior_direction * raw
    domain: str = ''          # canonical exclusion rule (NaN guard) or ''
    deviation: str = ''       # where the available fields depart from the cited construction
    smoothing: int = SMOOTHING
    smoothing_reason: str = ''


def specs() -> list[Spec]:
    g = grammar()
    Expr, binary, call, window, pair = g.Expr, g.binary, g.call, g.window, g.pair
    F = {f.name: Expr(f.name, f.prior_bars) for f in FIELDS}
    one, zero, two, neg1 = Expr('1'), Expr('0'), Expr('2'), Expr('-1')
    div = lambda a, b: binary(a, '/', b)
    sub = lambda a, b: binary(a, '-', b)
    add = lambda a, b: binary(a, '+', b)
    mul = lambda a, b: binary(a, '*', b)
    neg = lambda x: mul(neg1, x)  # prior-positive orientation of a negative prior

    def positive(x, *domains):  # x + 0 * log(d): NaN unless every d > 0 (log(0) = -inf, 0 * -inf = NaN)
        for d in domains:
            x = add(x, mul(zero, call('log', d)))
        return x

    close, raw, mkt, me = F['close'], F['raw_close'], F['mkt_ret'], F['me_company']
    ret = sub(div(close, window('delay', close, 1)), one)  # v2 spelling, byte-identical
    excess = sub(ret, mkt)
    mom = sub(div(window('delay', close, 21), window('delay', close, 252)), one)
    at, at_lag4 = F['at'], F['at_lag4']
    avg_at = div(add(at, at_lag4), two)
    si_ratio = div(F['si_shares'], F['shares_out'])
    iv = F['iv_atm_21d']
    ivg = window('ts_backfill', add(iv, mul(zero, call('log', mul(sub(iv, Expr(IV_FLOOR)), sub(Expr(IV_CAP), iv))))),
                 IV_BACKFILL)
    rho = lambda n: pair('correlation', ret, mkt, n)
    earn = F['earn_recent']
    event = mul(earn, window('delay', earn, 1))  # 1 on the session after the reaction session (both flagged)
    # Split-neutral vendor share count: line ME over adjusted close (shares_out and raw_close are both on the
    # session's share basis, so a split cancels); its log change is Daniel-Titman composite issuance.
    split_neutral_shares = div(mul(F['shares_out'], raw), close)
    ev = sub(add(me, F['debt']), F['che'])
    W, X = 'within_industry_grp_ff12', 'cross_section'
    positive_note = 'non-positive {} -> NaN (excluded), as in Hou, Xue and Zhang (2020)'
    assets_note = 'non-positive {} -> NaN (house guard)'
    si_split = ('si_shares is on the FINRA settlement-date share basis and shares_out on the session basis: from a '
                'split until the next dissemination the ratio is off by the split factor (v3 caveat)')

    return [
        # ---- 1 value
        Spec('bm', 'value', 'B', positive(div(F['be'], me), F['be']), W,
             'Rosenberg, Reid and Lanstein (1985, JPM); Fama and French (1992, JF); robust pre-1963 (Linnainmaa and '
             'Roberts 2018), weak 2017-2020', 'be / me_company', 1, 'non-positive book equity -> NaN (Fama-French)'),
        Spec('ep', 'value', 'B', positive(div(F['ni_ttm'], me), F['ni_ttm']), W,
             'Basu (1977, JF); Fama and French (1992, JF) E/P for positive earnings', 'ni_ttm / me_company', 1,
             positive_note.format('earnings')),
        Spec('cfp', 'value', 'B+', positive(div(F['cfo_ttm'], me), F['cfo_ttm']), W,
             'Lakonishok, Shleifer and Vishny (1994, JF) cash flow to price; operating cash flow as in Desai, Rajgopal '
             'and Venkatachalam (2004, TAR)', 'cfo_ttm / me_company', 1, positive_note.format('cash flow'),
             'LSV cash flow is earnings plus depreciation; reported operating cash flow is used'),
        Spec('fcfp', 'value', 'B+', div(sub(F['cfo_ttm'], F['capx_ttm']), me), W,
             'Lakonishok, Shleifer and Vishny (1994, JF); free cash flow yield (Hackel, Livnat and Rai 1994, FAJ)',
             '(cfo_ttm - capx_ttm) / me_company', 1, '', 'negative free cash flow kept (ranks lowest)'),
        Spec('ebit_ev', 'value', 'B+', positive(div(F['oi_ttm'], ev), F['oi_ttm'], ev), W,
             'Loughran and Wellman (2011, JFQA) enterprise multiple (works in large caps)',
             'oi_ttm / (me_company + debt - che)', 1,
             'non-positive EBIT or enterprise value -> NaN (negative multiples excluded, Loughran-Wellman)',
             'EBIT (operating income) replaces EBITDA: no depreciation field; debt follows the T20 zero-fill rule'),
        Spec('net_payout', 'value', 'A', div(sub(add(F['dvc_ttm'], F['prstkc_ttm']), F['sstk_ttm']), me), W,
             'Boudoukh, Michaely, Richardson and Roberts (2007, JF) net payout yield',
             '(dvc_ttm + prstkc_ttm - sstk_ttm) / me_company', 1, '', 'net issuers (negative yield) kept'),
        Spec('sp', 'value', 'B', positive(div(F['sale_ttm'], me), F['sale_ttm']), W,
             'Barbee, Mukherji and Raines (1996, FAJ) sales to price', 'sale_ttm / me_company', 1,
             positive_note.format('sales')),
        Spec('rd_me', 'value', 'B-', positive(div(F['xrd_ttm'], me), F['xrd_ttm']), W,
             'Chan, Lakonishok and Sougiannis (2001, JF) R&D to market equity', 'xrd_ttm / me_company', 1,
             'zero or unreported R&D -> NaN (R&D-reporting firms only, as in Chan-Lakonishok-Sougiannis)'),
        # ---- 2 profitability_quality
        Spec('gpa', 'profitability_quality', 'A', positive(div(F['gp_ttm'], at), at), W,
             'Novy-Marx (2013, JFE) gross profitability (works in large caps; replicated by Hou, Xue and Zhang 2020)',
             'gp_ttm / at', 1, assets_note.format('total assets')),
        Spec('opbe', 'profitability_quality', 'A-', positive(div(F['oi_ttm'], F['be']), F['be']), W,
             'Fama and French (2015, JFE) operating profitability (RMW)', 'oi_ttm / be', 1,
             'non-positive book equity -> NaN', 'operating income stands in for revenue - COGS - SG&A - interest'),
        Spec('cfoa', 'profitability_quality', 'A', positive(div(F['cfo_ttm'], avg_at), avg_at), W,
             'Ball, Gerakos, Linnainmaa and Nikolaev (2016, JFE) cash-based operating profitability',
             'cfo_ttm / ((at + at_lag4) / 2)', 1, assets_note.format('average total assets'),
             'reported operating cash flow proxies cash-based operating profitability (no R&D add-back)'),
        Spec('roe_q', 'profitability_quality', 'A-', positive(div(F['ni_q'], F['be_lag1q']), F['be_lag1q']), W,
             'Hou, Xue and Zhang (2015, RFS) q-factor ROE', 'ni_q / be_lag1q', 1,
             'non-positive opening book equity -> NaN'),
        Spec('roa', 'profitability_quality', 'B+', positive(div(F['ni_ttm'], at), at), W,
             'Balakrishnan, Bartov and Faurel (2010, JAE); Chen, Novy-Marx and Zhang (2011, WP)', 'ni_ttm / at', 1,
             assets_note.format('total assets'),
             'trailing-twelve-month earnings over current assets (no one-quarter-lagged assets field)'),
        Spec('accruals', 'profitability_quality', 'C+',
             positive(neg(div(sub(F['ni_ttm'], F['cfo_ttm']), avg_at)), avg_at), W,
             'Sloan (1996, TAR); Hribar and Collins (2002, JAR) cash-flow-statement accruals; decay: Green, Hand and '
             'Soliman (2011, MS)', '(ni_ttm - cfo_ttm) / ((at + at_lag4) / 2)', -1,
             assets_note.format('average total assets')),
        Spec('fscore', 'profitability_quality', 'C+', F['fscore'], W,
             'Piotroski (2000, JAR) F-score', 'fscore (producer, nine signals)', 1),
        # ---- 3 investment_issuance
        Spec('asset_growth', 'investment_issuance', 'C+', positive(neg(sub(div(at, at_lag4), one)), at_lag4), W,
             'Cooper, Gulen and Schill (2008, JF) asset growth (weak in big stocks: Fama and French 2008)',
             'at / at_lag4 - 1', -1, assets_note.format('lagged total assets')),
        Spec('noa', 'investment_issuance', 'B', positive(neg(div(F['noa'], at_lag4)), at_lag4), W,
             'Hirshleifer, Hou, Teoh and Zhang (2004, JAE) net operating assets', 'noa / at_lag4', -1,
             assets_note.format('lagged total assets')),
        Spec('issuance_xbrl', 'investment_issuance', 'A', neg(call('log', div(F['shrs_q'], F['shrs_q_lag4']))), W,
             'Pontiff and Woodgate (2008, JF) share issuance; Daniel and Titman (2006, JF); present in big stocks '
             '(Fama and French 2008)', 'log(shrs_q / shrs_q_lag4)', -1, 'non-positive share ratio -> NaN (log)',
             'weighted-average diluted (else basic) shares of the anchor quarter (else fiscal year), not period-end '
             'shares outstanding. Split consistency by the T20 contract: facts are keyed by (start, end) and a later '
             'clock overrides, so shrs_q_lag4 is the anchor filing\'s own prior-year comparative, which the filing '
             'presents (Reg S-X 10-01(c); ASC 260 EPS denominators for every period presented) restated '
             'retroactively for splits (ASC 260-10-55-12). No price-factor correction is applied: it would '
             'double-correct that restated pair. A filer that does not re-report the comparative leaves the pair '
             'split-contaminated; T25 checks known splitters (AAPL 2020-08, TSLA 2020-08 and 2022-08, NVDA 2021-07, '
             'AMZN 2022-06, GOOGL 2022-07)'),
        Spec('issuance_vendor', 'investment_issuance', 'A-',
             neg(call('log', div(split_neutral_shares, window('delay', split_neutral_shares, 252)))), W,
             'Daniel and Titman (2006, JF) composite equity issuance over one year; Pontiff and Woodgate (2008, JF)',
             'log(adj / adj[t-252]) with adj = shares_out * raw_close / close: log growth of line market equity minus '
             'the log total return', -1, 'non-positive ratio -> NaN (log)',
             'shares_out and raw_close share the session basis, so splits and consolidations cancel; the dividend-'
             'adjusted close makes dividends count as payout (Daniel-Titman composite issuance, not the '
             'Pontiff-Woodgate share count); the vendor share count lags 90 days (A8)'),
        # ---- 4 earnings_momentum
        Spec('sue', 'earnings_momentum', 'C+', F['sue'], X,
             'Bernard and Thomas (1989, JAR); Livnat and Mendenhall (2006, JAR); filing-clock lag, decay: Martineau '
             '(2022, CFR)', 'sue (producer: seasonal random walk on first-reported quarterly net income)', 1, '',
             'filing clock (10-Q/10-K acceptance + 1 session), not the announcement date; net income, not EPS'),
        Spec('droe', 'earnings_momentum', 'B',
             positive(sub(div(F['ni_q'], F['be_lag1q']), div(F['ni_q_lag4'], F['be_lag1q_lag4'])),
                      F['be_lag1q'], F['be_lag1q_lag4']), X,
             'Hou, Mo, Xue and Zhang (2021, RF) change in ROE; Balakrishnan, Bartov and Faurel (2010, JAE)',
             'ni_q / be_lag1q - ni_q_lag4 / be_lag1q_lag4 (ROE_q - ROE_{q-4})', 1,
             'non-positive opening book equity in either quarter -> NaN'),
        Spec('chtax', 'earnings_momentum', 'B', positive(div(sub(F['txt_q'], F['txt_q_lag4']), at_lag4), at_lag4), X,
             'Thomas and Zhang (2011, JAR) tax expense surprises', '(txt_q - txt_q_lag4) / at_lag4', 1,
             assets_note.format('lagged total assets')),
        Spec('ear', 'earnings_momentum', 'B+',
             window('ts_backfill', positive(window('ts_sum', excess, 3), event), EAR_CARRY), X,
             'Chan, Jegadeesh and Lakonishok (1996, JF) earnings announcement return; Brandt, Kishore, Santa-Clara and '
             'Venkatachalam (2008, WP)',
             'market-adjusted return over the 3 sessions [r-1, r+1] around the most recent vendor reaction session r, '
             f'recorded at r+1 and carried until the next announcement, at most {EAR_CARRY} sessions', 1,
             'NaN on non-event sessions before the carry (0 * log(0)); NaN when no announcement in '
             f'{EAR_CARRY} sessions',
             'Chan-Jegadeesh-Lakonishok measure the abnormal return around the most recent quarterly announcement '
             'and hold six months, hence a six-month carry cap (one missed quarter keeps the name); the 3-day '
             '[-1, +1] window is the prereg choice (Brandt et al.; CJL, as recalled, use days -2..+1); equal-weight '
             'member market return (mkt_ret) as the benchmark'),
        # ---- 5 price_momentum
        Spec('mom_12_1', 'price_momentum', 'B+', mom, X,
             'Jegadeesh and Titman (1993, JF); 12-1 as in Fama-French UMD; crash risk: Daniel and Moskowitz (2016, JFE)',
             'close[t-21] / close[t-252] - 1', 1),
        Spec('ind_mom_12_1', 'price_momentum', 'B', call('group_mean', mom, F[FINE_INDUSTRY]), X,
             'Moskowitz and Grinblatt (1999, JF) industry momentum', 'FF49 equal-weight mean of mom_12_1', 1, '',
             'equal-weight FF49 industry mean (Moskowitz-Grinblatt: value-weight, 20 industries)'),
        Spec('within_ind_mom', 'price_momentum', 'C+', call('group_neutralize', mom, F[FINE_INDUSTRY]), X,
             'Asness, Porter and Stevens (2000, WP) within-industry momentum', 'mom_12_1 - FF49 mean', 1),
        Spec('high_52w', 'price_momentum', 'A-', div(close, window('ts_max', close, 252)), X,
             'George and Hwang (2004, JF) 52-week high', 'close / max(close over 252 sessions)', 1, '',
             'adjusted close (split and dividend continuous) in both terms'),
        # ---- 6 low_risk
        Spec('low_beta', 'low_risk', 'B-',
             neg(div(mul(pair('correlation', window('ts_sum', ret, 3), window('ts_sum', mkt, 3), 250),
                         window('stddev', ret, 252)), window('stddev', mkt, 252))), X,
             'Frazzini and Pedersen (2014, JFE) betting against beta',
             'corr(3-day returns, 250) * sd(ret, 252) / sd(mkt_ret, 252)', -1, '',
             'correlation window 250 sessions (Frazzini-Pedersen: 5 years; the runner bounds lookback at 314); '
             'equal-weight member market; no Vasicek shrink (rank-invariant)'),
        Spec('low_ivol', 'low_risk', 'B-',
             neg(mul(window('stddev', ret, 21),
                     call('signedpower', call('abs', sub(one, mul(rho(21), rho(21)))), Expr('0.5')))), X,
             'Ang, Hodrick, Xing and Zhang (2006, JF) idiosyncratic volatility',
             'sd(ret, 21) * sqrt(|1 - corr(ret, mkt_ret, 21)^2|): market-model residual volatility, one month', -1,
             '', 'market model residual (the paper uses Fama-French three-factor residuals; it reports CAPM robustness)'),
        Spec('low_max', 'low_risk', 'B-', neg(window('ts_max', ret, 21)), X,
             'Bali, Cakici and Whitelaw (2011, JFE) MAX', 'max daily return over 21 sessions', -1),
        Spec('lowvol_ind', 'low_risk', 'B-', call('group_neutralize', neg(window('stddev', ret, 252)), F[WITHIN_INDUSTRY]),
             X, 'Asness, Frazzini and Pedersen (2014, FAJ) low-risk investing without industry bets',
             'sd(ret, 252) minus its FF12 industry mean', -1),
        # ---- 7 short_interest
        Spec('si_ratio', 'short_interest', 'B+', neg(si_ratio), X,
             'Asquith, Pathak and Ritter (2005, JFE); Boehmer, Huszar and Jordan (2010, JFE)', 'si_shares / shares_out',
             -1, '', si_split),
        Spec('dtc', 'short_interest', 'B+', neg(F['si_dtc']), X,
             'Hong, Li, Ni, Scheinkman and Yan (2015, WP) days to cover', 'si_dtc', -1, '',
             'FINRA computes days to cover on one basis (short interest over average daily volume): no split mismatch'),
        Spec('si_change', 'short_interest', 'B-', neg(sub(si_ratio, window('delay', si_ratio, 21))), X,
             'Rapach, Ringgenberg and Zhou (2016, JFE) short interest predicts lower returns (direction)',
             'si_ratio - si_ratio[t-21] (one month, two FINRA cycles)', -1, '',
             'Rapach-Ringgenberg-Zhou is an aggregate time-series result; the cross-sectional change is the '
             f'pre-registered extrapolation; {si_split}, so a split gives a spike and an opposite-signed echo 21 '
             'sessions later'),
        # ---- 8 reversal_seasonality
        Spec('ind_adj_rev_5', 'reversal_seasonality', 'B-',
             neg(call('group_neutralize', sub(div(close, window('delay', close, 5)), one), F[FINE_INDUSTRY])), X,
             'Da, Liu and Schaumburg (2014, MS); Hameed and Mian (2015, JFQA) within-industry reversal',
             'close / close[t-5] - 1 minus its FF49 mean', -1, '',
             'weekly industry-adjusted return; the s21 decay spreads it over about a month (the papers use one month)'),
        Spec('seasonality_same_month', 'reversal_seasonality', 'C+',
             sub(div(window('delay', close, 224), window('delay', close, 245)), one), X,
             'Heston and Sadka (2008, JFE) seasonality',
             'close[t-224] / close[t-245] - 1: the 21-session window whose s21-decayed centre (t-241.2) matches the '
             'centre (t-241) of the same 21 sessions one year before the next 21', 1, '',
             'annual lag 12 only (lags 24+ exceed the 314-bar bound); the window is shifted 7 sessions later than '
             'the undecayed [t-252, t-231] because the s21 linear decay moves the effective centre 6.7 sessions '
             'earlier (mean lag of weights 21..1 = 1540 / 231)'),
        # ---- 9 options_implied
        Spec('iv_rv_spread', 'options_implied', 'B', sub(ivg, mul(window('stddev', ret, 21), Expr(ANNUALIZE))), X,
             'Bali and Hovakimian (2009, MS) volatility spreads: realized minus implied volatility predicts lower '
             'returns', 'iv_atm_21d - sd(ret, 21) * sqrt(252)', 1, 'v3 IV guard',
             'implied minus realized (the negative of the paper\'s realized-minus-implied spread)'),
    ]


def expression(spec: Spec):
    """R(base) then the smoothing wrapper; R = group_rank on grp_ff12 (themes 1-3) or rank."""
    g = grammar()
    ranked = (g.call('group_rank', spec.base, g.Expr(WITHIN_INDUSTRY)) if spec.ranking.startswith('within_industry')
              else g.call('rank', spec.base))
    return ranked if spec.smoothing == 1 else g.window('decay_linear', ranked, spec.smoothing)


# Fix rounds 1-2: controller rulings and review fixes, declared before any v4 TRAIN read.
REVISIONS = [
    dict(round=1, ruling='drop iv_change', change='iv_change removed; options_implied keeps iv_rv_spread only (39 '
         'candidates)', reason='blended ATM implied volatility mixes the call-IV (+) and put-IV (-) effects of An, Ang, '
         'Bali and Cakici (2014), so there is no unambiguous prior'),
    dict(round=1, ruling='canonical droe', change='droe = ni_q / be_lag1q - ni_q_lag4 / be_lag1q_lag4 (Hou, Mo, Xue and '
         'Zhang 2021); new declared field be_lag1q_lag4, which T21 emits', reason='replaces the fix-round-0 proxy '
         '(ni_q - ni_q_lag4) / be_lag1q'),
    dict(round=1, ruling='T22 landed', change='the registry cross-check reports grp_ group typing as checked when the '
         'engine tree carries T22 (static check of typecheck.hpp is_group_field); --engine-root selects the tree',
         reason='no library byte depends on it'),
    dict(round=2, ruling='drop mgmt_sy and qmj_lite', change='both composites removed (37 candidates)',
         reason='redundancy: every component is already a member of the same theme, so the equal theme weights '
                'already combine them; memory: they alone push the IC runner plan to 1.70 GB, above the 1.5 GB cap '
                '(1.449 GB without them); decided before any TRAIN read'),
    dict(round=2, ruling='review I1', change='issuance_vendor = -log(adj / adj[t-252]), adj = shares_out * raw_close / '
         'close', reason='shares_out is restated to each session\'s own share basis, so the plain ratio counted every '
         'split and consolidation of the trailing year as issuance; the split-neutral form is Daniel-Titman composite '
         'issuance'),
    dict(round=2, ruling='review minor 1', change=f'ear = ts_backfill(3-day CAR at r+1, {EAR_CARRY}) under the s21 '
         'default', reason='the 63-session average equalled the mean inter-announcement gap (NaN on long gaps, two '
         'events averaged on short ones, 63-session NaN exposure); the carried most recent announcement is the '
         'Chan-Jegadeesh-Lakonishok definition, and the smoothing exemption (NaN gaps) no longer applies'),
    dict(round=2, ruling='issuance_xbrl split check', change='no DSL change; field text and deviation state the '
         'T20 contract argument', reason='shrs_q_lag4 is the anchor filing\'s split-restated comparative (facts keyed '
         'by period, later clock wins); a price-factor correction would double-correct; T25 verifies on splitters'),
    dict(round=2, ruling='review minors', change='seasonality window re-centred under s21 (delay 224 / 245); SI split '
         'caveat restored; field texts match the T20 contract (shrs_q weighted-average diluted, sue on net income); '
         'asset denominators guarded; the validator raises ValueError at the end of input',
         reason='review minors 2-6'),
]


EXPECTED_TURNOVER = {
    'value': 'low (~0.02-0.05/day): price-scaled ratios move with price; fundamentals step at filings; s21 decay',
    'profitability_quality': 'very low (< 0.02/day): accounting ranks step at filings; s21 decay',
    'investment_issuance': 'very low (< 0.02/day): accounting and 252-session share ranks; s21 decay',
    'earnings_momentum': 'low: SUE/dROE/ChTax step at filings; EAR steps at events (about 1/63 of names per session)',
    'price_momentum': 'low (~0.03/day): 12-1 and 52-week-high ranks with s21 decay',
    'low_risk': 'low to moderate: 21-session risk ranks are the fastest; s21 decay',
    'short_interest': 'very low: FINRA dissemination about every 10.5 sessions; s21 decay',
    'reversal_seasonality': 'moderate (the fastest theme; cost-bound): weekly and one-month windows with s21 decay',
    'options_implied': 'moderate: daily IV and 21-session realized volatility with s21 decay',
}


def documents() -> dict[str, bytes]:
    roster = specs()
    candidates, lineage, templates, static = [], [], [], []
    theme_members = {t: [] for t in THEME_IDS}
    for order, spec in enumerate(roster, start=1):
        assert spec.theme in theme_members and spec.tier in TIER_RANK and spec.raw_prior_direction in (-1, 1)
        in_block = spec.theme in WITHIN_INDUSTRY_THEMES and spec.id not in WITHIN_INDUSTRY_EXCEPTIONS
        assert spec.ranking.startswith('within_industry') == in_block, spec.id
        assert spec.smoothing == SMOOTHING or spec.smoothing_reason, spec.id
        base_check = parse(spec.base.text)
        assert base_check[3] == spec.base.lookback, (spec.id, base_check[3], spec.base.lookback)
        expr = expression(spec)
        check = validate(expr.text, expr.lookback)
        static.append(check)
        theme_members[spec.theme].append(spec.id)
        labels = dict(theme=spec.theme, tier=spec.tier, tier_rank=TIER_RANK[spec.tier], prior_sign=1,
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
    ids = [c['id'] for c in candidates]
    total = len(candidates)
    assert total == 37 and len(set(ids)) == total and len({c['dsl'] for c in candidates}) == total
    assert all(re.fullmatch(r'[a-z0-9_]{1,64}', s) for s in ids + THEME_IDS)
    assert all(theme_members.values()) and len(THEME_IDS) <= MAX_FAMILIES
    referenced = set().union(*(set(s['fields']) for s in static))
    assert referenced - set(BASE_FIELDS) == {f.name for f in FIELDS} - set(BASE_FIELDS), referenced  # declared == used
    fields = [dict(name=f.name, basis=f.basis) for f in FIELDS]
    extras = sorted({f['name'] for f in fields} - set(BASE_FIELDS))
    assert len(extras) <= MAX_EXTRA_FIELDS
    library = dict(schema='atx.dsl-ic-library/v1', id=LIBRARY_ID, fields=fields,
                   families=[dict(id=t, description=d) for t, d in THEMES], candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    capacity = max(len(s['extra_fields']) for s in static)
    field_users = {f: [c['id'] for c, s in zip(candidates, static) if f in s['fields']] for f in extras}
    hygiene = ('Roster, themes, member ids and signs are prereg R1 (declared 2026-09-27 before any v4 TRAIN read), with '
               'the fix-round-1 and fix-round-2 rulings (iv_change, mgmt_sy and qmj_lite dropped; canonical droe), '
               'also before any v4 TRAIN read. Each '
               'member takes the canonical definition of its cited paper (window, scaling, exclusion rule) and the s21 '
               'default; the implementer read no v3 TRAIN performance output (admission tables, daily IC, summary IC '
               'statistics, v3 DSL catalogue) and no validation data. Field names come from the T18 section 6 '
               'proposal and the fields-v4 manifest field list (names and units only). Disclosed (prereg R1): v3 '
               'per-candidate TRAIN statistics were seen by the controller for price/volume families; the PV, SI, IV '
               'and EAR definitions here follow the literature, not v3 results.')
    recipe = dict(schema='atx.dsl-ic-experiment/v1', id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(rule='prior-signed-themed-v4', revision='fix-round-2', candidates=total, families=len(THEME_IDS),
                        family_is_theme=True, one_variant_per_hypothesis=True,
                        wrapper=f'decay_linear(R(base), {SMOOTHING}); R = group_rank(., {WITHIN_INDUSTRY}) for themes '
                                '1-3, rank(.) otherwise; no exemption',
                        smoothing_default=SMOOTHING,
                        smoothing_exemptions={s.id: s.smoothing_reason for s in roster if s.smoothing != SMOOTHING},
                        seed=None, max_prior_bars=MAX_PRIOR_BARS, complete_observations=MAX_PRIOR_BARS + 1,
                        grammar_helpers=dict(generator=V2_GENERATOR, sha256_lf=V2_GENERATOR_SHA256_LF,
                                             used='tokenize, Expr/binary/call/window/pair builders, peak_slots'),
                        generator_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest()),
        orientation=dict(rule='prior sign embedded in the DSL: base = raw_prior_direction * raw quantity, so a higher '
                              'value is the predicted long side and every candidate has prior_sign +1',
                         prior_sign=1, sign_policy_field='train-rank-ic21 (runner contract only)',
                         pinned_signs='T23 fitter --orientation prior writes pinned +1 signs; no TRAIN sign flips',
                         sign_estimation=False),
        labels=dict(per_candidate=['theme', 'tier', 'tier_rank', 'prior_sign', 'citation'],
                    where='library candidate rows and recipe lineage rows (identical values)',
                    tier_scale=TIER_RANK, tier_rank_meaning='1 = strongest prior; admission redundancy order is '
                                                            '(tier_rank, roster_order) (prereg R3)',
                    tier_basis='T18 report section 6 tiers; members T18 did not grade take the tier-1 alpha-priors doc '
                               '(docs/superpowers/handoffs/2026-09-26-tier1-v2-alpha-priors.md, literature only) rows: '
                               'momentum row 3 (A; plain 12-1 B+ for the Daniel-Moskowitz crash caveat, 52-week '
                               'high A-), EAR row 6 (B+), short interest and DTC row 7 (B+; the SI change has no '
                               'cross-sectional anchor: B-), low risk row 11 (B-), seasonality row 12 (C+), options '
                               '(IV-RV spread B on its published direction)'),
        themes=[dict(theme=t, index=k + 1, description=d, members=theme_members[t], expected_turnover=EXPECTED_TURNOVER[t])
                for k, (t, d) in enumerate(THEMES)],
        within_industry=dict(group_field=WITHIN_INDUSTRY, operator='group_rank', themes=list(WITHIN_INDUSTRY_THEMES),
                             members=[s.id for s in roster if s.ranking.startswith('within_industry')],
                             exceptions=WITHIN_INDUSTRY_EXCEPTIONS,
                             exceptions_statement='sole exception list (prereg R1): empty; no theme 1-3 member of the '
                                                  'v4 roster is explicitly industry-level',
                             nan_label='a NaN industry label leaves the name out of the group (NaN signal)',
                             reason='prereg R1: v3 failed on regime concentration; industry-adjusted value and '
                                    'profitability are the stronger published forms (Novy-Marx 2013; Asness-Porter-'
                                    'Stevens 2000); cost if wrong: the between-industry value premium is lost'),
        industry_level_price_members=dict(
            group_fields={FINE_INDUSTRY: ['ind_mom_12_1', 'within_ind_mom', 'ind_adj_rev_5'],
                          WITHIN_INDUSTRY: ['lowvol_ind']},
            source='T18 section 6 DSL sketches (group field per member kept as sketched)'),
        revisions=REVISIONS,
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene),
        templates=templates,
        lineage=lineage,
        data=dict(fields=fields, producer=PRODUCER,
                  field_clocks={f.name: f.clock for f in FIELDS},
                  field_origin={f.name: f.origin for f in FIELDS},
                  group_fields=[f.name for f in FIELDS if f.group],
                  requires=dict(fields_v5='T21 fields-v5 manifest with every declared name (runner refuses a declared '
                                          'extra absent from the manifest)',
                                t22='grp_* typed as Group by typecheck is_group_field (T22; landed at root)'),
                  causality='row d of a signal uses fields known by the session-d close mark and is scored against '
                            'returns after d; fundamentals add the declared 1-session lag; si_* are strictly before '
                            'date(d); shares_out lags 90 days',
                  daily_return='ret = (close / delay(close, 1)) - 1; excess = ret - mkt_ret',
                  domain_guard='x + 0 * log(d) makes x NaN unless d > 0 (log(0) = -inf, 0 * -inf = NaN)',
                  iv_guard=f'ts_backfill(x + 0 * log((x - {IV_FLOOR}) * ({IV_CAP} - x)), {IV_BACKFILL}) (v3 rule)',
                  realized_vol=f'stddev(ret, 21) * {ANNUALIZE} (sqrt 252), annualized decimal like the IV field',
                  earnings_window='event = earn_recent * delay(earn_recent, 1) is 1 on r+1 (reaction r and r+1 '
                                  'flagged); ts_sum(excess, 3) at r+1 spans r-1..r+1',
                  unused_producer_fields=['iv_atm_63d', 'iv_atm_126d', 'lt', 'noa_lag4', 'grp_sic2', 'mktcap_lagged',
                                          'size_grp', 'is_common']),
        admission=dict(policy='v4-prior-v1 (T23; prereg R3)', trials=total),
        composition=dict(policy='ew-theme-v1 (T23; prereg R4)', theme_weight=f'1/{len(THEME_IDS)} per theme with >= 1 '
                         'admitted member, split equally among admitted members; renormalised over themes present',
                         runner_default='equal family weight = equal theme weight (family is theme)'),
        trials=dict(generated_candidates=total, admission_trials=total, orientation_fits=0, composition_recipes=1,
                    construction_recipes=1, family_or_template_selection=False, validation_selection=False,
                    variants_per_hypothesis=1),
        static_validation=dict(rule='python re-parse (v2/v3 validator lineage + dtype rule): canonical round-trip, '
                                    'registry arity, declared fields, Group classifier only as a group operator\'s 2nd '
                                    'argument, positive integer windows, lookback recomputed per typecheck, dag slot '
                                    'bound',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_native_prior_bars=max(s['native_prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               extra_field_capacity=capacity, declared_extra_fields=len(extras),
                               extra_field_users=field_users,
                               slot_estimate='post-order interning and refcount retirement; native --plan-only '
                                             'authoritative'),
        qualification=dict(native_parse_vm='pending: root --plan-only compile against fields-v5 (needs T21 and T22)',
                           empirical='unmeasured at source freeze',
                           low_turnover='smoothing intent; tau_k measured at admission (<= 0.70)',
                           prior_phase='v1, v2 and v3 files and generators are unchanged by v4'))
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
    print(f"candidates {len(library['candidates'])}; themes {len(library['families'])}; max prior bars "
          f"{sv['max_prior_bars']}; max dag nodes {sv['max_dag_nodes']}; max peak slots {sv['max_estimated_peak_slots']}; "
          f"extra-field capacity {sv['extra_field_capacity']} of {sv['declared_extra_fields']} declared")
    for theme in recipe['themes']:
        print(f"  {theme['index']} {theme['theme']}: {', '.join(theme['members'])}")
    print(registry_crosscheck(args.engine_root))


if __name__ == '__main__':
    main()
