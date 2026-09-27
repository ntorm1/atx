"""Synthetic fixture for generate_pv_fields_ic121_v3.py: pure Python + numpy, no build, no real data.

Run: python -B -m pytest -q atx-impl/strategies/test_generate_pv_fields_ic121_v3.py -p no:cacheprovider

It checks that the frozen v2 candidates are byte-identical in v3, that every new
candidate parses against the declared fields, that the static validator rejects
malformed DSL, and it evaluates the new candidates on a random synthetic panel
with a small numpy mirror of the pinned VM semantics (vm.hpp / ts_ops.hpp /
cs_ops.hpp: IEEE element-wise ops, full-window any-NaN -> NaN, ts_backfill,
average-tie cross-sectional rank) to check causality, the declared lookback, the
IV guard and the earnings-window gating. The native VM remains authoritative.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True


def _load(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


gen = _load('_generate_pv_fields_ic121_v3_under_test', 'generate_pv_fields_ic121_v3.py')


@pytest.fixture(scope='module')
def docs():
    out = gen.documents()
    return out, json.loads(out[gen.LIBRARY]), json.loads(out[gen.RECIPE])


# ---- documents -----------------------------------------------------------------
def test_documents_are_deterministic_and_committed(docs):
    raw, _, recipe = docs
    assert gen.documents() == raw
    for name, blob in raw.items():
        assert (HERE / name).read_bytes() == blob, name
    assert recipe['library']['sha256'] == hashlib.sha256(raw[gen.LIBRARY]).hexdigest()


def test_frozen_v2_prefix_is_byte_identical(docs):
    _, library, recipe = docs
    v2_library = json.loads((HERE / gen.V2_LIBRARY).read_bytes())
    v2_recipe = json.loads((HERE / gen.V2_RECIPE).read_bytes())
    assert hashlib.sha256((HERE / gen.V2_LIBRARY).read_bytes()).hexdigest() == gen.V2_LIBRARY_SHA256
    assert hashlib.sha256((HERE / gen.V2_RECIPE).read_bytes()).hexdigest() == gen.V2_RECIPE_SHA256
    n = len(v2_library['candidates'])
    assert n == gen.V2_CANDIDATES == 96
    encode = lambda obj: json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False)
    for old, new in zip(v2_library['candidates'], library['candidates'][:n], strict=True):
        assert encode(old) == encode(new)
        assert old['dsl'].encode() == new['dsl'].encode()
    assert library['families'][:len(v2_library['families'])] == v2_library['families']
    assert library['fields'][:len(v2_library['fields'])] == v2_library['fields']
    assert recipe['lineage'][:n] == v2_recipe['lineage']
    v2_hashes = {row['id']: row['dsl_sha256'] for row in v2_recipe['lineage']}
    for candidate in library['candidates'][:n]:
        assert hashlib.sha256(candidate['dsl'].encode()).hexdigest() == v2_hashes[candidate['id']]
    frozen = recipe['generation']['frozen_v2']
    assert (frozen['library_sha256'], frozen['recipe_sha256']) == (gen.V2_LIBRARY_SHA256, gen.V2_RECIPE_SHA256)


def test_new_candidates_parse_against_declared_fields(docs):
    _, library, recipe = docs
    declared = {f['name'] for f in library['fields']}
    new = library['candidates'][gen.V2_CANDIDATES:]
    rows = recipe['lineage'][gen.V2_CANDIDATES:]
    assert len(new) == gen.NEW_CANDIDATES == 25 and len(library['candidates']) == 121
    assert len(library['families']) == 25 <= gen.MAX_FAMILIES
    referenced = set()
    for candidate, row in zip(new, rows, strict=True):
        tree, shape, prior_bars, native, text = gen.parse(candidate['dsl'])
        fields = gen.fields_of(tree)
        assert shape == 'panel' and text == candidate['dsl'] and fields <= declared
        assert (prior_bars, native) == (row['prior_bars'], row['native_prior_bars']) and prior_bars <= 87
        assert row['dsl_sha256'] == hashlib.sha256(candidate['dsl'].encode()).hexdigest()
        assert candidate['horizons'] == [5, 21, 63] and candidate['sign_policy'] == 'train-rank-ic21'
        referenced |= fields
    assert referenced == declared - {'volume'}  # every declared v3 field is used; volume is used by v1/v2 only
    iv_users = [c['id'] for c in new if 'iv_atm' in c['dsl']]
    assert iv_users and all('ts_backfill((iv_atm_' in c['dsl'] and '0.02' in c['dsl'] for c in new if c['id'] in iv_users)


@pytest.mark.parametrize('dsl', [
    'rank(iv_atm_252d)',                       # undeclared field
    'rank(mktcap_lagged)',                     # producer field deliberately not declared
    'ts_backfill(si_dtc, 2.5)',                # fractional window
    'ts_backfill(si_dtc, 0)',                  # zero window
    'delay(si_dtc, -1)',                       # negative shift (future reference)
    'ts_backfill(si_dtc, si_dtc)',             # non-literal window
    'ts_foo(si_dtc, 5)',                       # unregistered operator
    'log(si_dtc, 2)',                          # wrong arity
    'rank(si_dtc',                             # unbalanced
    'rank( si_dtc)',                           # non-canonical text
    'rank(si_dtc) si_dtc',                     # trailing tokens
])
def test_validator_rejects_malformed_dsl(dsl):
    with pytest.raises((ValueError, IndexError)):
        gen.parse(dsl)


# ---- numpy mirror of the pinned VM semantics (subset used by the new candidates) --
def _rank_row(row: np.ndarray) -> np.ndarray:
    out = np.full(row.shape, np.nan)
    valid = np.flatnonzero(~np.isnan(row))
    if len(valid) == 1:
        out[valid] = 0.5
    elif len(valid) > 1:
        values = row[valid]
        order = np.argsort(values, kind='stable')
        ranks = np.empty(len(valid))
        ranks[order] = np.arange(len(valid), dtype=float)
        for v in np.unique(values):  # average ties
            tie = values == v
            ranks[tie] = ranks[tie].mean()
        out[valid] = ranks / (len(valid) - 1)
    return out


def evaluate(node, panel: dict[str, np.ndarray]) -> np.ndarray:
    shape = next(iter(panel.values())).shape
    kind = node[0]
    if kind == 'num':
        return np.full(shape, node[1])
    if kind == 'field':
        return panel[node[1]].astype(float).copy()
    with np.errstate(all='ignore'):
        if kind == 'bin':
            a, b = evaluate(node[2], panel), evaluate(node[3], panel)
            return {'+': a + b, '-': a - b, '*': a * b, '/': a / b}[node[1]]
        opcode, args = node[1], node[2:]
        x = evaluate(args[0], panel)
        if opcode == 'Log':
            return np.log(x)
        if opcode == 'Sign':
            return np.sign(x)
        if opcode == 'Abs':
            return np.abs(x)
        if opcode == 'CsRank':
            return np.vstack([_rank_row(r) for r in x])
        d = int(args[-1][1])
        out = np.full(shape, np.nan)
        for t in range(shape[0]):
            if opcode == 'TsDelay':
                if t >= d:
                    out[t] = x[t - d]
                continue
            if opcode == 'TsBackfill':
                for j in range(shape[1]):
                    for s in range(t, max(t - d, -1), -1):
                        if not np.isnan(x[s, j]):
                            out[t, j] = x[s, j]
                            break
                continue
            if t + 1 < d:
                continue
            w = x[t + 1 - d:t + 1]
            ok = ~np.isnan(w).any(axis=0)
            if opcode in ('TsSum', 'TsStd'):
                ok &= np.isfinite(w).all(axis=0)
            if opcode == 'TsSum':
                value = w.sum(axis=0)
            elif opcode == 'TsStd':
                value = w.std(axis=0, ddof=1)
            elif opcode == 'TsDecayLinear':
                weights = np.arange(1, d + 1, dtype=float)
                value = (weights[:, None] * w).sum(axis=0) / weights.sum()
            else:
                raise AssertionError(f'evaluator lacks {opcode}')
            out[t] = np.where(ok, value, np.nan)
        return out


DATES, NAMES = 150, 9


def synthetic_panel(seed: int = 7) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    close = 50 * np.exp(np.cumsum(rng.normal(0, 0.02, (DATES, NAMES)), axis=0))
    raw = close * rng.uniform(0.5, 2.0, NAMES)
    iv21 = rng.uniform(0.15, 0.9, (DATES, NAMES))
    earn = np.zeros((DATES, NAMES))
    for j in range(NAMES):
        for event in range(int(rng.integers(0, 60)), DATES, 63):
            earn[event:event + 2, j] = 1.0
    earn[:, 0] = 0.0  # name 0 never reports
    si = rng.uniform(1e5, 5e7, (DATES, NAMES))
    for t in range(1, DATES):  # FINRA-like steps: short interest moves only every ~10 sessions
        if t % 10:
            si[t] = si[t - 1]
    return dict(close=close, raw_close=raw, volume=rng.uniform(1e5, 1e7, (DATES, NAMES)),
                mkt_ret=np.repeat(rng.normal(0, 0.01, (DATES, 1)), NAMES, axis=1),
                si_shares=si, si_dtc=np.maximum(1.0, rng.gamma(2.0, 2.0, (DATES, NAMES))),
                iv_atm_21d=iv21, iv_atm_63d=iv21 * rng.uniform(0.8, 1.2, (DATES, NAMES)),
                iv_atm_126d=iv21 * rng.uniform(0.8, 1.3, (DATES, NAMES)),
                earn_recent=earn, shares_out=np.repeat(rng.uniform(1e7, 1e9, (1, NAMES)), DATES, axis=0))


def new_trees(library):
    return [(c['id'], gen.parse(c['dsl'])) for c in library['candidates'][gen.V2_CANDIDATES:]]


def same(a: np.ndarray, b: np.ndarray) -> bool:
    return bool(np.array_equal(a, b, equal_nan=True))


def test_new_candidates_are_causal_and_lookback_is_sufficient(docs):
    _, library, _ = docs
    base = synthetic_panel()
    rng = np.random.default_rng(11)
    t0 = 120
    future = {k: v.copy() for k, v in base.items()}
    for k, v in future.items():  # scramble every field strictly after t0
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    for cid, (tree, _, prior_bars, _, _) in new_trees(library):
        clean = evaluate(tree, base)
        assert same(clean[:t0 + 1], evaluate(tree, future)[:t0 + 1]), f'{cid} reads the future'
        assert np.isfinite(clean[prior_bars + 1:, 1:]).any(), f'{cid} never defined'
        past = {k: v.copy() for k, v in base.items()}
        cut = t0 - prior_bars  # data before t0 - prior_bars must not matter at t0
        for v in past.values():
            v[:cut] = v[:cut][::-1]
        assert same(clean[t0], evaluate(tree, past)[t0]), f'{cid} reads beyond prior_bars={prior_bars}'


@pytest.mark.parametrize('garbage', [1.2e16, 69.3, 5.0, 0.02, 0.01, 0.0, -0.3])
def test_iv_guard_treats_out_of_range_as_missing(docs, garbage):
    _, library, _ = docs
    panel = synthetic_panel()
    g, j = 100, 3
    for field in ('iv_atm_21d', 'iv_atm_63d', 'iv_atm_126d'):
        dirty, missing = ({k: v.copy() for k, v in panel.items()} for _ in range(2))
        dirty[field][g, j], missing[field][g, j] = garbage, np.nan
        for cid, (tree, *_rest) in new_trees(library):
            if field in gen.fields_of(tree):
                assert same(evaluate(tree, dirty), evaluate(tree, missing)), (cid, field, garbage)


def test_iv_guard_backfill_is_bounded_and_admits_valid_values(docs):
    _, _, recipe = docs
    template = next(t for t in recipe['templates'] if t['template'] == 'iv_level_21')
    tree = gen.parse(template['base_dsl'])[0]
    panel = synthetic_panel()
    j = 2
    panel['iv_atm_21d'][100, j] = 0.35                 # inside (0.02, 5): admitted as is
    panel['iv_atm_21d'][101:107, j] = np.nan           # six missing sessions
    base = evaluate(tree, panel)
    assert base[100, j] == -0.35
    assert all(base[t, j] == -0.35 for t in range(101, 105))  # carried through t-4
    assert np.isnan(base[105:107, j]).all()                   # never beyond the 5-session window, never zero


def test_earnings_car_is_nan_without_event_and_sums_flagged_excess(docs):
    _, _, recipe = docs
    panel = synthetic_panel()
    ret = np.full((DATES, NAMES), np.nan)
    ret[1:] = panel['close'][1:] / panel['close'][:-1] - 1
    excess = ret - panel['mkt_ret']
    for hold in (21, 42, 63):
        template = next(t for t in recipe['templates'] if t['template'] == f'earn_car_{hold}')
        base = evaluate(gen.parse(template['base_dsl'])[0], panel)
        for t in range(hold, DATES):
            window = slice(t + 1 - hold, t + 1)
            flags = panel['earn_recent'][window]
            for j in range(NAMES):
                if flags[:, j].sum() == 0:
                    assert np.isnan(base[t, j]), (hold, t, j)
                else:
                    expected = (flags[:, j] * excess[window, j]).sum()
                    assert base[t, j] == pytest.approx(expected, rel=1e-12, abs=1e-15), (hold, t, j)
        assert np.isnan(base[hold:, 0]).all()  # the never-reporting name is excluded, not zero


def test_prior_positive_orientation_of_short_interest_and_size(docs):
    _, library, recipe = docs
    panel = synthetic_panel()
    t = 140
    by_id = dict(new_trees(library))
    si_rank = evaluate(by_id['si_ratio_s1'][0], panel)[t]
    ratio = panel['si_shares'][t] / panel['shares_out'][t]
    assert np.argmax(si_rank) == np.argmin(ratio)  # lowest short interest ranks highest (prior: high SI -> lower returns)
    size_dsl = next(r['base_dsl'] for r in recipe['templates'] if r['template'] == 'size_mcap')
    size = evaluate(gen.parse(size_dsl)[0], panel)[t]
    assert np.argmax(size) == np.argmin(panel['shares_out'][t] * panel['raw_close'][t])
