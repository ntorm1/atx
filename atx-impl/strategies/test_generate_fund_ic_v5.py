"""Synthetic fixture for generate_fund_ic_v5.py (library v5.1): pure Python + numpy, no build, no real data.

Run: "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v5.py -q

It checks that the frozen v4 library is untouched and its 37 rows are copied byte-identically into v5.1,
that exactly one candidate (opex_at, prereg v5.1 R1'') is appended with the pre-registered spec, the IC
runner memory budget (<= 5 extras, <= 7 slots), that --check accepts the committed bytes and rejects
tampered ones, that the fitter reads the labels, and it evaluates opex_at on the v4 test's synthetic panel
with the v4 numpy mirror of the pinned VM semantics (proxy, domain guard, within-FF12 rank, causality).
The native VM remains authoritative.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
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


gen = _load('_generate_fund_ic_v5_under_test', HERE / 'generate_fund_ic_v5.py')
v4test = _load('_test_generate_fund_ic_v4_helpers_for_v5', HERE / 'test_generate_fund_ic_v4.py')

# Prereg v5.1 R1'' and controller ruling R-a, transcribed independently of the generator.
OPEX_DSL = 'decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21)'
OPEX_CITATION = ('Novy-Marx (2011, RF) operating leverage; proxy opex = sale_ttm - oi_ttm (includes D&A) because '
                 'fields-v6 has no opex_ttm')
V4_LIBRARY_SHA256 = 'daa9663e43119102fbb42aba3be9b57961cd34925920c50baee7f340f0e1eddf'
V4_RECIPE_SHA256 = '62b510f12a79cfacd2e74d64e020f20cee202cadb82153653b2e1065ec250c81'
ARRAY_END = b'\n  ]\n}\n'


@pytest.fixture(scope='module')
def docs():
    out = gen.documents()
    return out, json.loads(out[gen.LIBRARY]), json.loads(out[gen.RECIPE])


@pytest.fixture(scope='module')
def v4docs():
    return json.loads((HERE / gen.V4_LIBRARY).read_bytes()), json.loads((HERE / gen.V4_RECIPE).read_bytes())


def _row(library, cid):
    return next(c for c in library['candidates'] if c['id'] == cid)


# ---- documents -----------------------------------------------------------------
def test_documents_are_deterministic_and_committed(docs):
    raw, _, recipe = docs
    assert gen.documents() == raw
    for name, blob in raw.items():
        assert (HERE / name).read_bytes() == blob, name
    assert recipe['library']['sha256'] == hashlib.sha256(raw[gen.LIBRARY]).hexdigest()


def test_total_is_38_and_v4_members_are_byte_identical(docs, v4docs):
    raw, library, recipe = docs
    lib4, rec4 = v4docs
    v4_bytes, v5_bytes = (HERE / gen.V4_LIBRARY).read_bytes(), raw[gen.LIBRARY]
    assert hashlib.sha256(v4_bytes).hexdigest() == gen.V4_LIBRARY_SHA256 == V4_LIBRARY_SHA256
    assert hashlib.sha256((HERE / gen.V4_RECIPE).read_bytes()).hexdigest() == gen.V4_RECIPE_SHA256 == V4_RECIPE_SHA256
    assert gen.frozen_v4() == (v4_bytes, (HERE / gen.V4_RECIPE).read_bytes())
    assert len(library['candidates']) == 38 == gen.TOTAL_CANDIDATES and len(lib4['candidates']) == 37
    # Byte level: the pinned v4 candidates array, as encoded in its file, is a prefix of v5.1's.
    head = b'  "candidates": [\n'
    block4 = v4_bytes[v4_bytes.index(head):-len(ARRAY_END)]
    assert v4_bytes.endswith(ARRAY_END) and v5_bytes.endswith(ARRAY_END)
    assert v5_bytes[v5_bytes.index(head):].startswith(block4 + b',\n    {\n      "id": "opex_at",')
    encode = lambda obj: json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False)
    assert [encode(c) for c in library['candidates'][:37]] == [encode(c) for c in lib4['candidates']]
    assert recipe['lineage'][:37] == rec4['lineage'] and recipe['templates'][:37] == rec4['templates']
    assert library['fields'] == lib4['fields'] and library['schema'] == lib4['schema']
    changed = [f['id'] for f, f4 in zip(library['families'], lib4['families'], strict=True) if f != f4]
    assert [f['id'] for f in library['families']] == [f['id'] for f in lib4['families']] and changed == [
        'profitability_quality']
    frozen = recipe['generation']['frozen_v4']
    assert (frozen['library_sha256'], frozen['recipe_sha256'], frozen['candidates']) == (
        V4_LIBRARY_SHA256, V4_RECIPE_SHA256, 37)


def test_opex_at_spec_fields(docs):
    _, library, recipe = docs
    c = library['candidates'][-1]
    assert c == dict(id='opex_at', family='profitability_quality', dsl=OPEX_DSL, horizons=[5, 21, 63],
                     sign_policy='train-rank-ic21', theme='profitability_quality', tier='B', tier_rank=4,
                     prior_sign=1, citation=OPEX_CITATION)
    row = recipe['lineage'][-1]
    assert row['id'] == 'opex_at' and row['roster_order'] == 38
    assert [r['roster_order'] for r in recipe['lineage']] == list(range(1, 39))
    for key in ('family', 'theme', 'tier', 'tier_rank', 'prior_sign', 'citation'):
        assert row[key] == c[key], key
    assert (row['raw_prior_direction'], row['ranking'], row['smoothing_sessions'], row['prior_bars'],
            row['native_prior_bars']) == (1, 'within_industry_grp_ff12', 21, 20, 20)
    assert row['fields'] == ['at', 'grp_ff12', 'oi_ttm', 'sale_ttm']
    assert row['dsl_sha256'] == hashlib.sha256(OPEX_DSL.encode()).hexdigest()
    t = recipe['templates'][-1]
    assert (t['id'], t['formula'], t['base_dsl'], t['raw_prior_direction'], t['smoothing_reason']) == (
        'opex_at', '(sale_ttm - oi_ttm) / at', '(((sale_ttm - oi_ttm) / at) + (0 * log(at)))', 1, None)
    assert 'D&A' in t['deviation'] and 'non-positive total assets' in t['domain']
    members = {th['theme']: th['members'] for th in recipe['themes']}
    assert members['profitability_quality'][-1] == 'opex_at' and sum(len(v) for v in members.values()) == 38
    assert 'opex_at' in recipe['within_industry']['members'] and len(recipe['within_industry']['members']) == 20
    # Ruling R-b: a profitability_quality addition is ranked group_rank(., grp_ff12), as v4 themes 1-3.
    tree = gen.parse(OPEX_DSL)[0]
    assert tree[1] == 'TsDecayLinear' and tree[2][:2] == ('call', 'CsRankG') and tree[2][3] == ('field', 'grp_ff12')
    assert recipe['admission']['policy'].startswith('v4-prior-v1') and recipe['trials']['admission_trials'] == 38
    assert recipe['generation']['smoothing_exemptions'] == {} and recipe['generation']['new']['ids'] == ['opex_at']


def test_memory_budget_extras_and_slots(docs, v4docs):
    _, library, recipe = docs
    _, rec4 = v4docs
    peaks = {}
    for c, row in zip(library['candidates'], recipe['lineage'], strict=True):
        tree, shape, dtype, prior_bars, _, text = gen.parse(c['dsl'])
        assert shape == 'panel' and dtype == 'f64' and text == c['dsl'] and prior_bars == row['prior_bars'] <= 314
        extras = set(row['fields']) - {'close', 'raw_close', 'volume'}
        assert len(extras) <= gen.MAX_EXTRAS_PER_CANDIDATE == 5, c['id']
        peaks[c['id']] = gen.v4().grammar().peak_slots(tree)[1]
        assert peaks[c['id']] <= gen.MAX_SLOTS_PER_CANDIDATE == 7, c['id']
    assert peaks['opex_at'] == 4 and recipe['lineage'][-1]['fields'] == ['at', 'grp_ff12', 'oi_ttm', 'sale_ttm']
    sv, sv4 = recipe['static_validation'], rec4['static_validation']
    # The library maxima the runner charges are v4's, so --plan-only grows by one composition row only.
    assert sv['max_estimated_peak_slots'] == sv4['max_estimated_peak_slots'] == max(peaks.values()) == 7
    assert sv['extra_field_capacity'] == sv4['extra_field_capacity'] == 5
    assert sv['max_prior_bars'] == sv4['max_prior_bars'] and sv['declared_extra_fields'] == sv4['declared_extra_fields']
    assert set(sv['extra_field_users']['sale_ttm']) == {'sp', 'opex_at'}
    assert set(sv['extra_field_users']['oi_ttm']) == {'ebit_ev', 'opbe', 'opex_at'}


def test_check_mode_accepts_committed_bytes():
    out = subprocess.run([sys.executable, '-B', str(HERE / 'generate_fund_ic_v5.py'), '--check'],
                         capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    assert 'candidates 38 (frozen v4 37 byte-identical, new 1)' in out.stdout
    assert f'{gen.LIBRARY} {hashlib.sha256((HERE / gen.LIBRARY).read_bytes()).hexdigest()}' in out.stdout


def test_check_mode_rejects_tampered_bytes(tmp_path):
    for name in ('generate_fund_ic_v5.py', gen.V4_GENERATOR, 'generate_price_volume_ic96_v2.py', gen.V4_LIBRARY,
                 gen.V4_RECIPE, gen.RECIPE):
        shutil.copyfile(HERE / name, tmp_path / name)
    blob = (HERE / gen.LIBRARY).read_bytes()
    tampered = blob.replace(b'"prior_sign": 1,\n      "citation": "Novy-Marx (2011',
                            b'"prior_sign": -1,\n      "citation": "Novy-Marx (2011')
    assert len(tampered) == len(blob) + 1  # exactly opex_at's row changed
    (tmp_path / gen.LIBRARY).write_bytes(tampered)
    out = subprocess.run([sys.executable, '-B', str(tmp_path / 'generate_fund_ic_v5.py'), '--check'],
                         capture_output=True, text=True, timeout=120)
    assert out.returncode != 0 and f'fixed artifact differs: {gen.LIBRARY}' in out.stderr


def test_fitter_reads_the_v51_labels(docs):
    _, library, _ = docs
    fcw = _load('_fcw_for_v51', HERE.parent / 'tools' / 'fit_composition_weights.py')
    lib_path, rec_path = HERE / gen.LIBRARY, HERE / gen.RECIPE
    lib_sha, rec_sha = (hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib_path, rec_path))
    rows = fcw.load_library(lib_path, lib_sha)
    priors = fcw.load_priors(lib_path, lib_sha, rec_path, rec_sha, rows)
    assert priors['themes'] == [c['theme'] for c in library['candidates']] and set(priors['themes']) <= set(fcw.V4_THEMES)
    assert priors['prior_signs'] == [1] * 38 and priors['source'] == 'library+recipe'
    assert priors['tier_rank'][-1] == 4  # fitter's 0-based grade index: A+ 0, A 1, A- 2, B+ 3, B 4


# ---- opex_at semantics on the v4 synthetic panel (v4 numpy mirror) -----------------
def test_opex_at_is_operating_costs_to_assets_with_the_asset_guard(docs):
    _, _, recipe = docs
    p = v4test.synthetic_panel()
    t = 250
    p['at'][t, 3], p['at'][t, 5] = 0.0, -1.0e9  # non-positive assets are excluded
    template = next(x for x in recipe['templates'] if x['id'] == 'opex_at')
    base = v4test.evaluate(gen.parse(template['base_dsl'])[0], p)[t]
    keep = p['at'][t] > 0
    want = (p['sale_ttm'][t] - p['oi_ttm'][t]) / np.where(keep, p['at'][t], np.nan)
    assert np.array_equal(np.isfinite(base), keep) and not keep.all()
    assert np.allclose(base[keep], want[keep], rtol=1e-12)
    # Prior-positive orientation: more operating cost per asset dollar is the long side.
    q = {k: v.copy() for k, v in p.items()}
    q['oi_ttm'][t, 0] -= 1.0e8
    assert v4test.evaluate(gen.parse(template['base_dsl'])[0], q)[t, 0] > base[0]


def test_opex_at_ranks_within_ff12_and_smooths_over_21_sessions(docs):
    _, library, _ = docs
    p = v4test.synthetic_panel()
    tree = gen.parse(_row(library, 'opex_at')['dsl'])[0]
    ranked = v4test.evaluate(tree[2], p)  # group_rank(base, grp_ff12)
    t = 250
    opex = (p['sale_ttm'][t] - p['oi_ttm'][t]) / p['at'][t]
    labels = p['grp_ff12'][t]
    assert np.isnan(ranked[t, 11]) and np.isnan(labels[11])  # no industry -> out of the signal
    for g in np.unique(labels[~np.isnan(labels)]):
        members = np.flatnonzero(labels == g)
        order = members[np.argsort(opex[members])]
        assert np.allclose(ranked[t, order], np.linspace(0.0, 1.0, len(members)))
    weights = np.arange(1, 22, dtype=float)
    want = (weights[:, None] * ranked[t - 20:t + 1]).sum(axis=0) / weights.sum()
    got = v4test.evaluate(tree, p)[t]
    finite = np.isfinite(ranked[t - 20:t + 1]).all(axis=0)
    assert finite.sum() == 11 and np.allclose(got[finite], want[finite], rtol=1e-12) and np.isnan(got[~finite]).all()


def test_opex_at_is_causal_and_lookback_is_sufficient(docs):
    _, library, _ = docs
    tree, _, _, prior_bars, _, _ = gen.parse(_row(library, 'opex_at')['dsl'])
    base = v4test.synthetic_panel()
    rng = np.random.default_rng(11)
    t0 = 300
    future = {k: v.copy() for k, v in base.items()}
    for v in future.values():
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    clean = v4test.evaluate(tree, base)
    assert v4test.same(clean[:t0 + 1], v4test.evaluate(tree, future)[:t0 + 1])
    assert np.isfinite(clean[prior_bars + 1:]).any()
    past = {k: v.copy() for k, v in base.items()}
    for v in past.values():
        v[:t0 - prior_bars] = v[:t0 - prior_bars][::-1]
    assert v4test.same(clean[t0], v4test.evaluate(tree, past)[t0])
