"""Synthetic fixture for generate_fund_ic_v6.py (library v6) and check_fund_ic_v6.py: pure Python + numpy, no data.

Run: "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v6.py -q

It checks the v6 roster edits and smoothing forms against an independent transcription of the V6-L brief, the
IC-runner static budget, --check determinism, the fitter's label read, and evaluates the members on the v4 test's
synthetic panel with its numpy mirror of the pinned VM semantics (extended here with power, ts_count_nans,
ts_regression and the member mask of every Cs op): causality and lookback, the 21-session membership blackout the
switch removes, the R&D zero-fill of cbop, the algebraic identity of res_mom_12_1 with v4.2's base, and the
composite. The native VM remains authoritative.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import shutil
import subprocess
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


gen = _load('_generate_fund_ic_v6_under_test', HERE / 'generate_fund_ic_v6.py')
chk = _load('_check_fund_ic_v6_under_test', HERE / 'check_fund_ic_v6.py')
v4test = _load('_test_generate_fund_ic_v4_helpers_for_v6', HERE / 'test_generate_fund_ic_v4.py')

# ---- the V6-L brief, transcribed independently of the generator -------------------------
V51_REMOVED = {'cfoa', 'low_beta', 'low_ivol', 'low_max', 'lowvol_ind'}
V6_ADDED = {'value_composite', 'cbop', 'res_mom_12_1', 'bac', 'smax'}
FAST = {'si_change', 'ind_adj_rev_5', 'seasonality_same_month', 'iv_rv_spread'}  # tau >= .08 (low_max removed)
BLACKOUT_UNFIXABLE = {'ind_mom_12_1', 'within_ind_mom'}  # x holds a Cs op: left byte-identical
NEW_DSL = {
    'value_composite': '(((group_rank(decay_linear((be / me_company), 21), grp_ff12) + group_rank(decay_linear((ni_ttm '
                       '/ me_company), 21), grp_ff12)) + group_rank(decay_linear((cfo_ttm / me_company), 21), grp_ff12)) '
                       '/ 3)',
    'cbop': 'group_rank(decay_linear((((cfo_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - '
            'ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12)',
    'bac': 'rank(decay_linear((-1 * correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(mkt_ret, 3), 250)), '
           '21))',
    'smax': 'rank(decay_linear((-1 * ((ts_max(((close / delay(close, 1)) - 1), 21) / stddev(((close / delay(close, 1)) '
            '- 1), 252)) + (0 * log(stddev(((close / delay(close, 1)) - 1), 252))))), 21))',
}
V42_RES_MOM_BASE = ('delay(((ts_sum(((close / delay(close, 1)) - 1), 231) - (ts_regression(((close / delay(close, 1)) - '
                    '1), mkt_ret, 231) * ts_sum(mkt_ret, 231))) / (stddev(((close / delay(close, 1)) - 1), 231) * '
                    'signedpower(abs((1 - (correlation(((close / delay(close, 1)) - 1), mkt_ret, 231) * '
                    'correlation(((close / delay(close, 1)) - 1), mkt_ret, 231)))), 0.5))), 21)')


@pytest.fixture(scope='module')
def docs():
    out = gen.documents()
    return out, json.loads(out[gen.LIBRARY]), json.loads(out[gen.RECIPE])


@pytest.fixture(scope='module')
def v51():
    return {c['id']: c for c in json.loads((HERE / gen.V5_LIBRARY).read_bytes())['candidates']}


def _row(library, cid):
    return next(c for c in library['candidates'] if c['id'] == cid)


# ---- documents ----------------------------------------------------------------------------
def test_documents_are_deterministic_and_committed(docs):
    raw, _, recipe = docs
    assert gen.documents() == raw
    for name, blob in raw.items():
        assert (HERE / name).read_bytes() == blob, name
    assert recipe['library']['sha256'] == hashlib.sha256(raw[gen.LIBRARY]).hexdigest()
    assert hashlib.sha256((HERE / gen.V5_LIBRARY).read_bytes()).hexdigest() == gen.V5_LIBRARY_SHA256


def test_check_mode_accepts_committed_and_rejects_tampered(tmp_path):
    out = subprocess.run([sys.executable, '-B', str(HERE / 'generate_fund_ic_v6.py'), '--check'],
                         capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr
    assert f'{gen.LIBRARY} {hashlib.sha256((HERE / gen.LIBRARY).read_bytes()).hexdigest()}' in out.stdout
    for name in ('generate_fund_ic_v6.py', gen.V5_GENERATOR, gen.V4_GENERATOR, 'generate_price_volume_ic96_v2.py',
                 gen.V5_LIBRARY, gen.V5_RECIPE, 'fund_industry_ic_v4.json', 'fund_industry_ic_v4.recipe.json', gen.RECIPE):
        shutil.copyfile(HERE / name, tmp_path / name)
    blob = (HERE / gen.LIBRARY).read_bytes()
    (tmp_path / gen.LIBRARY).write_bytes(blob.replace(b', 250)), 21))', b', 249)), 21))'))
    out = subprocess.run([sys.executable, '-B', str(tmp_path / 'generate_fund_ic_v6.py'), '--check'],
                         capture_output=True, text=True, timeout=300)
    assert out.returncode != 0 and f'fixed artifact differs: {gen.LIBRARY}' in out.stderr


# ---- roster and forms -----------------------------------------------------------------------
def test_roster_edits_match_the_brief(docs, v51):
    _, library, recipe = docs
    ids = [c['id'] for c in library['candidates']]
    assert len(ids) == len(set(ids)) == 38 <= 48
    assert set(ids) == (set(v51) - V51_REMOVED) | V6_ADDED
    order = {i: k for k, i in enumerate(ids)}
    # replacements take the replaced slot; additions precede the incumbent they upgrade
    v51_ids = list(v51)
    assert order['value_composite'] + 1 == order['bm'] and order['res_mom_12_1'] + 1 == order['mom_12_1']
    assert ids[order['cbop'] - 1] == v51_ids[v51_ids.index('cfoa') - 1] == 'opbe'
    assert ids[order['bac'] + 1] == 'smax' and ids[order['smax'] + 1] == 'si_ratio'
    members = {t['theme']: t['members'] for t in recipe['themes']}
    assert members['low_risk'] == ['bac', 'smax']
    assert [r['roster_order'] for r in recipe['lineage']] == list(range(1, 39))


def test_every_candidate_is_prior_signed_with_a_source(docs):
    _, library, recipe = docs
    for c, row in zip(library['candidates'], recipe['lineage'], strict=True):
        assert c['prior_sign'] == row['prior_sign'] == 1 and c['citation'] and c['tier'] in gen.v4().TIER_RANK
        direction, source = gen.PRIOR_SIGN_SOURCES[c['id']]
        assert row['raw_prior_direction'] == direction and row['prior_sign_source'] == source
        assert any(ch.isdigit() for ch in source), c['id']  # author-year
    raw = {r['id']: r['raw_prior_direction'] for r in recipe['lineage']}
    assert (raw['bac'], raw['smax'], raw['cbop'], raw['res_mom_12_1'], raw['value_composite']) == (-1, -1, 1, 1, 1)


def test_smoothing_forms(docs, v51):
    _, library, recipe = docs
    forms = {r['id']: r['smoothing_form'] for r in recipe['lineage']}
    for c in library['candidates']:
        cid, dsl = c['id'], c['dsl']
        tree = gen.parse(dsl)[0]
        if cid in FAST:
            assert forms[cid] == gen.FORM_RANK and 'decay_linear' not in dsl and tree[1] == 'CsRank', cid
        elif cid in BLACKOUT_UNFIXABLE:
            assert forms[cid] == gen.FORM_DECAY_OF_RANK and dsl == v51[cid]['dsl'], cid
            assert tree[1] == 'TsDecayLinear' and gen.has_cs_op(tree[2][2]), cid
        elif cid == 'value_composite':
            assert forms[cid] == gen.FORM_COMPOSITE and dsl == NEW_DSL[cid]
        else:
            assert forms[cid] == gen.FORM_RANK_OF_DECAY, cid
            assert tree[1] in ('CsRank', 'CsRankG') and tree[2][1] == 'TsDecayLinear' and not gen.has_cs_op(tree[2]), cid
            if cid in v51:  # the same base, only the order of decay and rank swapped
                old = gen.parse(v51[cid]['dsl'])[0]
                assert old[1] == 'TsDecayLinear' and old[2][1] == tree[1] and old[2][2] == tree[2][2], cid
    for cid, dsl in NEW_DSL.items():
        assert _row(library, cid)['dsl'] == dsl, cid
    # seasonality without the decay returns to the undecayed same-month window
    assert _row(library, 'seasonality_same_month')['dsl'] == 'rank(((delay(close, 231) / delay(close, 252)) - 1))'


def test_static_budget(docs):
    _, library, recipe = docs
    for c, row in zip(library['candidates'], recipe['lineage'], strict=True):
        tree, shape, dtype, prior_bars, _, text = gen.parse(c['dsl'])
        assert shape == 'panel' and dtype == 'f64' and text == c['dsl'] and prior_bars == row['prior_bars'] <= 314
        assert len(set(row['fields']) - {'close', 'raw_close', 'volume'}) <= 5, c['id']
        assert gen.v4().grammar().peak_slots(tree)[1] <= 7, c['id']
    sv = recipe['static_validation']
    assert sv['max_estimated_peak_slots'] <= 7 and sv['extra_field_capacity'] <= 5 and sv['max_prior_bars'] <= 314


def test_fitter_reads_the_v6_labels(docs):
    _, library, _ = docs
    fcw = _load('_fcw_for_v6', HERE.parent / 'tools' / 'fit_composition_weights.py')
    lib_path, rec_path = HERE / gen.LIBRARY, HERE / gen.RECIPE
    lib_sha, rec_sha = (hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib_path, rec_path))
    rows = fcw.load_library(lib_path, lib_sha)
    priors = fcw.load_priors(lib_path, lib_sha, rec_path, rec_sha, rows)
    assert priors['themes'] == [c['theme'] for c in library['candidates']] and set(priors['themes']) <= set(fcw.V4_THEMES)
    assert priors['prior_signs'] == [1] * 38 and priors['source'] == 'library+recipe'


# ---- numpy mirror: v4's evaluator + power / ts_count_nans / ts_regression + member-masked Cs ops -----------
_V4_EVALUATE = v4test.evaluate
MASK: dict[str, np.ndarray | None] = {'member': None}


def _evaluate(node, panel):
    kind = node[0]
    if kind != 'call':
        return _V4_EVALUATE(node, panel)
    opcode, args = node[1], node[2:]
    shape = next(iter(panel.values())).shape
    with np.errstate(all='ignore'):
        if opcode == 'Pow':
            return np.power(_evaluate(args[0], panel), _evaluate(args[1], panel))
        if opcode.startswith('Cs'):
            x = _evaluate(args[0], panel)
            if MASK['member'] is not None:
                x = np.where(MASK['member'], x, np.nan)  # non-members are excluded and emit NaN
            if opcode == 'CsRank':
                return np.vstack([v4test._rank_row(r) for r in x])
            g = _evaluate(args[1], panel)
            return np.vstack([v4test._group_row(x[t], g[t], opcode) for t in range(shape[0])])
        if opcode in ('TsCountNans', 'TsRegression'):
            x = _evaluate(args[0], panel)
            y = _evaluate(args[1], panel) if opcode == 'TsRegression' else None
            d = int(args[-1][1])
            out = np.full(shape, np.nan)
            for t in range(d - 1, shape[0]):
                w = x[t + 1 - d:t + 1]
                if opcode == 'TsCountNans':
                    out[t] = np.isnan(w).sum(axis=0)
                    continue
                v = y[t + 1 - d:t + 1]  # slope of x (dependent) on y (regressor)
                ok = ~np.isnan(w).any(axis=0) & ~np.isnan(v).any(axis=0)
                a, b = w - w.mean(axis=0), v - v.mean(axis=0)
                out[t] = np.where(ok, (a * b).sum(axis=0) / (b * b).sum(axis=0), np.nan)
            return out
    return _V4_EVALUATE(node, panel)


@pytest.fixture(autouse=True)
def _mirror(monkeypatch):
    monkeypatch.setattr(v4test, 'evaluate', _evaluate)  # v4's recursion resolves the module global
    MASK['member'] = None
    yield
    MASK['member'] = None


def test_pow_nan_zero_is_one():
    assert math.pow(float('nan'), 0.0) == 1.0 and float(np.power(np.nan, 0.0)) == 1.0  # C99 Annex F


def test_candidates_are_causal_and_lookback_is_sufficient(docs):
    _, library, _ = docs
    base = v4test.synthetic_panel()
    rng = np.random.default_rng(11)
    t0 = 300
    future = {k: v.copy() for k, v in base.items()}
    for v in future.values():
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    for c in library['candidates']:
        tree, _, _, prior_bars, _, _ = gen.parse(c['dsl'])
        clean = _evaluate(tree, base)
        assert v4test.same(clean[:t0 + 1], _evaluate(tree, future)[:t0 + 1]), f"{c['id']} reads the future"
        assert np.isfinite(clean[prior_bars + 1:]).any(), f"{c['id']} never defined"
        past = {k: v.copy() for k, v in base.items()}
        cut = t0 - prior_bars
        for v in past.values():
            v[:cut] = v[:cut][::-1]
        assert v4test.same(clean[t0], _evaluate(tree, past)[t0]), f"{c['id']} reads beyond prior_bars={prior_bars}"


def test_switch_removes_the_membership_blackout(docs, v51):
    _, library, _ = docs
    p = v4test.synthetic_panel()
    gap = 280
    span = slice(gap - 1, gap + 22)

    def blank_one(old_tree, new_tree):
        """A name whose signal is finite around the gap without a mask, then a one-session membership gap."""
        MASK['member'] = None
        old, new = _evaluate(old_tree, p), _evaluate(new_tree, p)
        j = next(k for k in range(p['close'].shape[1]) if np.isfinite(old[span, k]).all() and
                 np.isfinite(new[span, k]).all())
        member = np.ones_like(p['close'], dtype=bool)
        member[gap, j] = False  # one session outside the top-N
        MASK['member'] = member
        return _evaluate(old_tree, p)[:, j], _evaluate(new_tree, p)[:, j]

    for cid in ('bm', 'gpa', 'mom_12_1', 'sue', 'ear', 'si_ratio', 'high_52w', 'issuance_vendor'):
        old, new = blank_one(gen.parse(v51[cid]['dsl'])[0], gen.parse(_row(library, cid)['dsl'])[0])
        assert np.isfinite(old[gap - 1]) and np.isfinite(new[gap - 1]), cid
        assert np.isnan(old[gap:gap + 21]).all() and np.isfinite(old[gap + 21]), cid  # 21-session blackout
        assert np.isnan(new[gap]) and np.isfinite(new[gap + 1:gap + 22]).all(), cid  # the gap day only
    for cid in BLACKOUT_UNFIXABLE:  # x holds a masked Cs op: the blackout binds under either order
        dsl = _row(library, cid)['dsl']
        head, tail = 'decay_linear(rank(', '), 21)'
        assert dsl.startswith(head) and dsl.endswith(tail), cid
        swapped = f'rank(decay_linear({dsl[len(head):-len(tail)]}, 21))'
        a, b = blank_one(gen.parse(dsl)[0], gen.parse(swapped)[0])
        assert np.isnan(a[gap:gap + 21]).all() and np.isnan(b[gap:gap + 21]).all(), cid


def test_cbop_adds_reported_rnd_and_zero_fills_unreported(docs):
    _, _, recipe = docs
    p = v4test.synthetic_panel()
    t = 250
    p['xrd_ttm'][:, [1, 4, 7]] = np.nan  # not reported
    p['at'][t, 5] = 0.0
    base = _evaluate(gen.parse(next(x for x in recipe['templates'] if x['id'] == 'cbop')['base_dsl'])[0], p)[t]
    keep = p['at'][t] > 0
    with np.errstate(divide='ignore'):
        want = (p['cfo_ttm'][t] + np.nan_to_num(p['xrd_ttm'][t], nan=0.0)) / p['at'][t]
    assert np.array_equal(np.isfinite(base), keep) and np.allclose(base[keep], want[keep], rtol=1e-12)


def test_res_mom_equals_the_v42_base(docs):
    _, _, recipe = docs
    p = v4test.synthetic_panel()
    ours = _evaluate(gen.parse(next(x for x in recipe['templates'] if x['id'] == 'res_mom_12_1')['base_dsl'])[0], p)
    parent = gen.v4()
    parent_registry = dict(parent.REGISTRY, ts_regression=(3, 3, 'TsRegression'))
    saved = parent.REGISTRY, parent.ROLLING_OPS
    parent.REGISTRY, parent.ROLLING_OPS = parent_registry, set(parent.ROLLING_OPS) | {'TsRegression'}
    try:
        v42 = _evaluate(gen.parse(V42_RES_MOM_BASE)[0], p)
    finally:
        parent.REGISTRY, parent.ROLLING_OPS = saved
    finite = np.isfinite(v42)
    assert finite.sum() > 0 and np.array_equal(finite, np.isfinite(ours))
    assert np.allclose(ours[finite], v42[finite], rtol=1e-9, atol=1e-9)


def test_value_composite_is_the_mean_within_ff12_rank_of_three_yields(docs):
    _, library, _ = docs
    p = v4test.synthetic_panel()
    t = 250
    p['be'][:, 2] = -1.0e8  # negative book equity is ranked (as the most expensive), not dropped
    got = _evaluate(gen.parse(_row(library, 'value_composite')['dsl'])[0], p)[t]
    parts = [_evaluate(gen.parse(f'group_rank(decay_linear(({n} / me_company), 21), grp_ff12)')[0], p)[t]
             for n in ('be', 'ni_ttm', 'cfo_ttm')]
    want = sum(parts) / 3
    assert v4test.same(np.isfinite(got), np.isfinite(want)) and np.allclose(got[np.isfinite(got)],
                                                                              want[np.isfinite(want)])
    assert np.isfinite(got[2]) and parts[0][2] == 0.0


def test_smax_and_bac_orientation(docs):
    _, library, _ = docs
    p = v4test.synthetic_panel()
    t = 300
    base = lambda cid, panel: _evaluate(gen.parse(_row(library, cid)['dsl'])[0][2][2], panel)  # base under decay
    q = {k: v.copy() for k, v in p.items()}
    q['close'][t:, 0] *= 1.3  # a lottery-like jump raises scaled MAX: less long
    assert base('smax', q)[t, 0] < base('smax', p)[t, 0]
    q = {k: v.copy() for k, v in p.items()}
    q['close'][:, 0] = 50 * np.cumprod(1 + p['mkt_ret'][:, 0])  # moves one-for-one with the market: less long
    assert base('bac', q)[t, 0] < base('bac', p)[t, 0] and np.isclose(base('bac', q)[t, 0], -1.0)


# ---- check script -----------------------------------------------------------------------------------
def test_check_script(tmp_path, docs):
    _, library, _ = docs
    names = [f['name'] for f in library['fields'] if f['name'] not in chk.BASE_FIELDS] + ['iv_atm_63d']
    manifest = tmp_path / 'manifest.json'
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names]}))
    run = lambda lib: subprocess.run([sys.executable, '-B', str(HERE / 'check_fund_ic_v6.py'), '--manifest',
                                      str(manifest), '--library', str(lib)], capture_output=True, text=True, timeout=60)
    out = run(HERE / gen.LIBRARY)
    assert out.returncode == 0, out.stdout
    assert "removed 5 ['cfoa', 'low_beta', 'low_ivol', 'low_max', 'lowvol_ind']" in out.stdout
    assert 'unchanged 2' in out.stdout and 'changed 31' in out.stdout
    bad = json.loads((HERE / gen.LIBRARY).read_bytes())
    bad['candidates'][0]['dsl'] = 'trade_when(close, close, close)'
    bad['candidates'][1]['prior_sign'] = -1
    bad['candidates'][2]['dsl'] = bad['candidates'][2]['dsl'].replace('ni_ttm', 'opex_ttm')
    bad['candidates'].append(dict(bad['candidates'][3]))
    path = tmp_path / 'bad.json'
    path.write_text(json.dumps(bad))
    out = run(path)
    assert out.returncode == 1
    for needle in ('forbidden operator trade_when', 'prior_sign -1', "field 'opex_ttm' not in the manifest",
                   'duplicate ids'):
        assert needle in out.stdout, needle
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names if n != 'xrd_ttm']}))
    out = run(HERE / gen.LIBRARY)
    assert out.returncode == 1 and "field 'xrd_ttm' not in the manifest" in out.stdout
