"""Synthetic fixture for generate_fund_ic_v42.py: pure Python + numpy, no build, no real data.

Run: python -B -m pytest -q atx-impl/strategies/test_generate_fund_ic_v42.py -p no:cacheprovider

It checks that the frozen v4 library is untouched and copied byte-identically into v4.2, the three
v4.2 R1' additions (roster, labels, smoothing, memory budget), the extended registry rows, and it
evaluates every candidate on the v4 test's synthetic panel with a numpy mirror of the pinned VM
semantics (the v4 mirror plus sign, ts_mean and ts_regression) to check causality, the declared
lookback, residual momentum, the earnings-announcement-premium window and split-neutral share
turnover. It also loads the library through the fitter's prior-metadata reader. The native VM
remains authoritative.
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


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


gen = _load('_generate_fund_ic_v42_under_test', HERE / 'generate_fund_ic_v42.py')
v4test = _load('_test_generate_fund_ic_v4_helpers', HERE / 'test_generate_fund_ic_v4.py')
NEW = {'res_mom_12_1': 'price_momentum', 'eap': 'earnings_momentum', 'low_share_turnover': 'low_risk'}
DATES, NAMES = v4test.DATES, v4test.NAMES


@pytest.fixture(scope='module')
def docs():
    out = gen.documents()
    return out, json.loads(out[gen.LIBRARY]), json.loads(out[gen.RECIPE])


@pytest.fixture(scope='module')
def v4docs():
    return json.loads((HERE / gen.V4_LIBRARY).read_bytes()), json.loads((HERE / gen.V4_RECIPE).read_bytes())


# ---- documents -----------------------------------------------------------------
def test_documents_are_deterministic_and_committed(docs):
    raw, _, recipe = docs
    assert gen.documents() == raw
    for name, blob in raw.items():
        assert (HERE / name).read_bytes() == blob, name
    assert recipe['library']['sha256'] == hashlib.sha256(raw[gen.LIBRARY]).hexdigest()


def test_frozen_v4_files_are_untouched_and_copied(docs, v4docs):
    _, library, recipe = docs
    lib4, rec4 = v4docs
    assert hashlib.sha256((HERE / gen.V4_LIBRARY).read_bytes()).hexdigest() == gen.V4_LIBRARY_SHA256
    assert hashlib.sha256((HERE / gen.V4_RECIPE).read_bytes()).hexdigest() == gen.V4_RECIPE_SHA256
    assert gen.frozen_v4() == ((HERE / gen.V4_LIBRARY).read_bytes(), (HERE / gen.V4_RECIPE).read_bytes())
    n = gen.V4_CANDIDATES
    encode = lambda obj: json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False)
    assert [encode(c) for c in library['candidates'][:n]] == [encode(c) for c in lib4['candidates']]
    assert recipe['lineage'][:n] == rec4['lineage'] and recipe['templates'][:n] == rec4['templates']
    assert library['fields'] == lib4['fields'] and [f['id'] for f in library['families']] == [f['id'] for f in lib4['families']]
    frozen = recipe['generation']['frozen_v4']
    assert (frozen['library_sha256'], frozen['recipe_sha256']) == (gen.V4_LIBRARY_SHA256, gen.V4_RECIPE_SHA256)


def test_roster_and_labels_of_the_additions(docs, v4docs):
    _, library, recipe = docs
    lib4, _ = v4docs
    ids = [c['id'] for c in library['candidates']]
    assert ids == [c['id'] for c in lib4['candidates']] + list(NEW) and len(ids) == 40
    assert [r['roster_order'] for r in recipe['lineage']] == list(range(1, 41))
    members = {t['theme']: t['members'] for t in recipe['themes']}
    for c, row in zip(library['candidates'][gen.V4_CANDIDATES:], recipe['lineage'][gen.V4_CANDIDATES:], strict=True):
        assert c['family'] == c['theme'] == NEW[c['id']] and c['id'] in members[c['theme']]
        assert c['prior_sign'] == 1 and c['tier_rank'] == gen.v4().TIER_RANK[c['tier']] and len(c['citation']) > 10
        assert c['horizons'] == [5, 21, 63] and c['sign_policy'] == 'train-rank-ic21'
        for key in ('theme', 'tier', 'tier_rank', 'prior_sign', 'citation'):
            assert row[key] == c[key]
    assert {c['id']: c['tier'] for c in library['candidates'][gen.V4_CANDIDATES:]} == {
        'res_mom_12_1': 'A-', 'eap': 'B-', 'low_share_turnover': 'C+'}
    assert recipe['admission']['policy'].startswith('v4-prior-v2') and recipe['trials']['admission_trials'] == 40


def test_memory_budget_smoothing_and_lookback(docs):
    _, library, recipe = docs
    for c, row in zip(library['candidates'], recipe['lineage'], strict=True):
        tree, shape, dtype, prior_bars, native, text = gen.parse(c['dsl'])
        assert shape == 'panel' and dtype == 'f64' and text == c['dsl'] and prior_bars == row['prior_bars'] <= 314
        extras = set(row['fields']) - {'close', 'raw_close', 'volume'}
        assert len(extras) <= gen.MAX_EXTRAS_PER_CANDIDATE == 5, c['id']
    assert recipe['static_validation']['extra_field_capacity'] == 5
    rows = {r['id']: r for r in recipe['lineage']}
    assert (rows['res_mom_12_1']['smoothing_sessions'], rows['low_share_turnover']['smoothing_sessions'],
            rows['eap']['smoothing_sessions']) == (21, 21, 1)
    assert list(recipe['generation']['smoothing_exemptions']) == ['eap']
    assert (rows['res_mom_12_1']['prior_bars'], rows['eap']['prior_bars'], rows['low_share_turnover']['prior_bars']) == (
        272, 126, 145)


def test_extended_registry_rows_match_the_engine():
    status = gen.registry_crosscheck()
    assert status.startswith('registry cross-check ok (17 operators') or 'skipped' in status
    assert set(gen.V42_REGISTRY_ROWS) <= set(gen.v4().REGISTRY)
    fresh = _load('_fresh_v4_generator', HERE / gen.V4_GENERATOR)  # the frozen generator itself is not extended
    assert not set(gen.V42_REGISTRY_ROWS) & set(fresh.REGISTRY)
    with pytest.raises(ValueError):
        fresh.parse('rank(sign(close))')


def test_fitter_reads_the_v42_labels(docs):
    raw, library, _ = docs
    fcw = _load('_fcw_for_v42', HERE.parent / 'tools' / 'fit_composition_weights.py')
    lib_path, rec_path = HERE / gen.LIBRARY, HERE / gen.RECIPE
    lib_sha, rec_sha = (hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib_path, rec_path))
    rows = fcw.load_library(lib_path, lib_sha)
    priors = fcw.load_priors(lib_path, lib_sha, rec_path, rec_sha, rows)
    assert priors['themes'] == [c['theme'] for c in library['candidates']]
    assert priors['prior_signs'] == [1] * 40 and priors['source'] == 'library+recipe'
    assert priors['tier_rank'][-3:] == [2, 5, 6]  # fitter's 0-based grade index: A+ 0 .. A- 2, B- 5, C+ 6


# ---- numpy mirror (the v4 test's, plus Sign, TsMean and TsRegression) ------------
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
        if opcode in ('Log', 'Abs', 'Sign'):
            return {'Log': np.log, 'Abs': np.abs, 'Sign': np.sign}[opcode](x)
        if opcode == 'Spow':
            return np.sign(x) * np.abs(x) ** evaluate(args[1], panel)
        if opcode == 'CsRank':
            return np.vstack([v4test._rank_row(r) for r in x])
        if opcode in ('CsRankG', 'CsNeutG', 'CsMeanG'):
            g = evaluate(args[1], panel)
            return np.vstack([v4test._group_row(x[t], g[t], opcode) for t in range(shape[0])])
        y = evaluate(args[1], panel) if opcode in ('TsCorr', 'TsRegression') else None
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
            if opcode == 'TsSum':
                value = w.sum(axis=0)
            elif opcode == 'TsMean':
                value = w.mean(axis=0)
            elif opcode == 'TsStd':
                value = w.std(axis=0, ddof=1)
            elif opcode == 'TsMax':
                value = w.max(axis=0)
            elif opcode == 'TsDecayLinear':
                weights = np.arange(1, d + 1, dtype=float)
                value = (weights[:, None] * w).sum(axis=0) / weights.sum()
            elif opcode in ('TsCorr', 'TsRegression'):  # args: (y, x, d) for the regression slope of y on x
                v = y[t + 1 - d:t + 1]
                ok &= ~np.isnan(v).any(axis=0)
                a, b = w - w.mean(axis=0), v - v.mean(axis=0)
                value = ((a * b).sum(axis=0) / np.sqrt((a * a).sum(axis=0) * (b * b).sum(axis=0)) if opcode == 'TsCorr'
                         else (a * b).sum(axis=0) / (b * b).sum(axis=0))
            else:
                raise AssertionError(f'evaluator lacks {opcode}')
            out[t] = np.where(ok, value, np.nan)
        return out


def _base(recipe, cid, panel):
    template = next(t for t in recipe['templates'] if t['id'] == cid)
    return evaluate(gen.parse(template['base_dsl'])[0], panel)


def test_every_candidate_is_causal_and_lookback_is_sufficient(docs):
    _, library, _ = docs
    base = v4test.synthetic_panel()
    rng = np.random.default_rng(11)
    t0 = 300
    future = {k: v.copy() for k, v in base.items()}
    for v in future.values():
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    for c in library['candidates']:
        tree, _, _, prior_bars, _, _ = gen.parse(c['dsl'])
        clean = evaluate(tree, base)
        assert v4test.same(clean[:t0 + 1], evaluate(tree, future)[:t0 + 1]), f"{c['id']} reads the future"
        assert np.isfinite(clean[prior_bars + 1:]).any(), f"{c['id']} never defined"
        past = {k: v.copy() for k, v in base.items()}
        for v in past.values():
            v[:t0 - prior_bars] = v[:t0 - prior_bars][::-1]
        assert v4test.same(clean[t0], evaluate(tree, past)[t0]), f"{c['id']} reads beyond prior_bars={prior_bars}"


def test_residual_momentum_is_the_standardized_market_model_residual(docs):
    _, _, recipe = docs
    p = v4test.synthetic_panel()
    rng = np.random.default_rng(5)
    m = 0.003 + rng.normal(0, 0.01, DATES)                 # a strong up-market
    p['mkt_ret'] = np.repeat(m[:, None], NAMES, axis=1)
    r = rng.normal(0, 0.01, (DATES, NAMES))
    r[:, 0] = 1.6 * m + rng.normal(0, 0.004, DATES)          # high beta, no alpha: plain momentum winner
    r[:, 1] = 0.4 * m + 0.0015 + rng.normal(0, 0.004, DATES)  # low beta, positive alpha: residual winner
    r[0] = 0.0
    p['close'] = 50 * np.cumprod(1 + r, axis=0)
    t = 300
    ret = np.full(r.shape, np.nan)
    ret[1:] = p['close'][1:] / p['close'][:-1] - 1
    w = slice(t - 251, t - 20)                               # the 231 returns ending at t-21
    y, x = ret[w], p['mkt_ret'][w]
    a, b = y - y.mean(axis=0), x - x.mean(axis=0)
    beta = (a * b).sum(axis=0) / (b * b).sum(axis=0)
    rho = (a * b).sum(axis=0) / np.sqrt((a * a).sum(axis=0) * (b * b).sum(axis=0))
    want = (y.sum(axis=0) - beta * x.sum(axis=0)) / (y.std(axis=0, ddof=1) * np.sqrt(np.abs(1 - rho * rho)))
    got = _base(recipe, 'res_mom_12_1', p)[t]
    assert np.allclose(got, want, rtol=1e-9)
    mom = _base(recipe, 'mom_12_1', p)[t]
    assert mom[0] > mom[1] and got[1] > got[0]              # residual momentum removes the market-beta winner


def test_eap_is_long_exactly_when_the_expected_announcement_is_within_21_sessions(docs):
    _, _, recipe = docs
    p = v4test.synthetic_panel()
    earn = p['earn_recent']
    earn[:, 2] = 0.0
    earn[100:102, 2] = 1.0  # a single announcement: reaction r = 100, marker 101, expected next 163
    base = _base(recipe, 'eap', p)
    markers = (earn == 1) & (np.vstack([np.zeros((1, NAMES)), earn[:-1]]) == 1)  # s = r + 1
    for t in range(126, DATES):
        for j in range(NAMES):
            seen = np.flatnonzero(markers[t - 125:t + 1, j]) + t - 125
            if seen.size == 0:
                assert np.isnan(base[t, j]), (t, j)
                continue
            last_r = seen[-1] - 1
            expected_next = last_r + gen.EAP_PREDICTED_GAP
            assert base[t, j] == float(t + 1 <= expected_next <= t + gen.EAP_HORIZON), (t, j)
    assert np.isnan(base[126:, 0]).all()                      # never reports: no schedule, not zero
    assert np.flatnonzero(base[126:227, 2] == 1).tolist() == [s - 126 for s in range(142, 163)]  # t in [r+42, r+62]
    assert np.isnan(base[227:, 2]).all()                      # 126 sessions after the marker, no schedule is left


def test_low_share_turnover_is_the_mean_turnover_and_split_neutral(docs):
    _, _, recipe = docs
    p = v4test.synthetic_panel()
    t = 280
    want = -(p['volume'][t - 125:t + 1] / p['shares_out'][t - 125:t + 1]).mean(axis=0)
    assert np.allclose(_base(recipe, 'low_share_turnover', p)[t], want, rtol=1e-12)
    split = {k: v.copy() for k, v in p.items()}
    split['shares_out'][200:] *= 4.0  # a 4:1 split: volume and the restated share count both scale
    split['volume'][200:] *= 4.0
    split['raw_close'][200:] /= 4.0
    a, b = _base(recipe, 'low_share_turnover', p), _base(recipe, 'low_share_turnover', split)
    assert np.allclose(a, b, rtol=1e-12, equal_nan=True) and np.isfinite(a[130:]).all()
