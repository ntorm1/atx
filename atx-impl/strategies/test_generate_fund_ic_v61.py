"""Library v6.1 (generate_fund_ic_v61.py) and the v6.1 mode of check_fund_ic_v6.py: pure Python + numpy, no data.

Run: "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v61.py -q
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
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


gen = _load('_generate_fund_ic_v61_under_test', HERE / 'generate_fund_ic_v61.py')
v4test = _load('_test_generate_fund_ic_v4_helpers_for_v61', HERE / 'test_generate_fund_ic_v4.py')
SV_DSL = 'rank((-1 * group_neutralize(sv_ratio126, grp_ff12)))'  # the prereg member, transcribed independently


@pytest.fixture(scope='module')
def docs():
    out = gen.documents()
    return out, json.loads(out[gen.LIBRARY]), json.loads(out[gen.RECIPE])


def test_documents_are_deterministic_committed_and_pinned(docs):
    raw, _, recipe = docs
    assert gen.documents() == raw
    for name, blob in raw.items():
        assert (HERE / name).read_bytes() == blob, name
    assert recipe['library']['sha256'] == hashlib.sha256(raw[gen.LIBRARY]).hexdigest()
    assert hashlib.sha256((HERE / gen.V6_LIBRARY).read_bytes()).hexdigest() == gen.V6_LIBRARY_SHA256
    assert hashlib.sha256((HERE / gen.V6_RECIPE).read_bytes()).hexdigest() == gen.V6_RECIPE_SHA256
    out = subprocess.run([sys.executable, '-B', str(HERE / 'generate_fund_ic_v61.py'), '--check'],
                         capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr


def test_v6_entries_unchanged_and_sv_flow_appended(docs):
    raw, library, recipe = docs
    v6_bytes = (HERE / gen.V6_LIBRARY).read_bytes()
    v6 = json.loads(v6_bytes)
    assert library['candidates'][:38] == v6['candidates']            # same objects, same order
    region = lambda blob: blob.split(b'"candidates": [\n', 1)[1]
    assert region(raw[gen.LIBRARY]).startswith(region(v6_bytes)[:-len(b'\n  ]\n}\n')])  # byte-identical text
    assert library['fields'][:-1] == v6['fields'] and library['fields'][-1]['name'] == 'sv_ratio126'
    sv = library['candidates'][-1]
    assert len(library['candidates']) == 39 and sv['id'] == 'sv_flow' and sv['dsl'] == SV_DSL
    assert (sv['theme'], sv['family'], sv['tier'], sv['prior_sign']) == ('short_interest', 'short_interest', 'B-', 1)
    assert 'Wang, Yan and Zheng (2020, JFE)' in sv['citation']
    row = recipe['lineage'][-1]
    assert (row['id'], row['roster_order'], row['raw_prior_direction'], row['prior_bars']) == ('sv_flow', 39, -1, 0)
    assert recipe['lineage'][:38] == json.loads((HERE / gen.V6_RECIPE).read_bytes())['lineage']
    members = {t['theme']: t['members'] for t in recipe['themes']}
    assert members['short_interest'] == ['si_ratio', 'dtc', 'si_change', 'sv_flow']
    assert 'decay_linear' not in sv['dsl'] and 'ts_' not in sv['dsl']  # no time-series op on top of the aggregate


def test_fitter_reads_the_labels(docs):
    _, library, _ = docs
    fcw = _load('_fcw_for_v61', HERE.parent / 'tools' / 'fit_composition_weights.py')
    lib_path, rec_path = HERE / gen.LIBRARY, HERE / gen.RECIPE
    lib_sha, rec_sha = (hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib_path, rec_path))
    rows = fcw.load_library(lib_path, lib_sha)
    priors = fcw.load_priors(lib_path, lib_sha, rec_path, rec_sha, rows)
    assert priors['prior_signs'] == [1] * 39 and priors['themes'][-1] == 'short_interest'
    assert priors['tiers'][-1] == 'B-' and priors['source'] == 'library+recipe'


def test_sv_flow_is_the_negated_ff12_demean_ranked():
    p = v4test.synthetic_panel()
    rng = np.random.default_rng(3)
    x = rng.uniform(0.3, 0.6, p['close'].shape)
    x[10:20, 4] = np.nan                                             # < 63 sessions: NaN field
    p['sv_ratio126'] = x
    tree = gen.parse(SV_DSL)[0]
    got = v4test.evaluate(tree, p)
    t = 100
    g = p['grp_ff12'][t]
    demeaned = np.full(x.shape[1], np.nan)
    for label in np.unique(g[~np.isnan(g)]):
        m = (g == label) & ~np.isnan(x[t])
        demeaned[m] = x[t, m] - x[t, m].mean()
    want = v4test._rank_row(-demeaned)
    assert v4test.same(got[t], want) and np.isnan(got[t, 11])        # NaN FF12 label stays NaN
    assert np.isnan(got[15, 4]) and np.isfinite(got[25, 4])
    q = dict(p, sv_ratio126=x.copy())
    q['sv_ratio126'][t, 0] += 0.2                                    # more shorting flow within its industry
    assert v4test.evaluate(tree, q)[t, 0] < got[t, 0]                # ... ranks lower (prior sign negative)


def test_checker_v61_mode(tmp_path, docs):
    _, library, _ = docs
    names = [f['name'] for f in library['fields'] if f['name'] not in ('close', 'raw_close', 'volume')]
    manifest = tmp_path / 'manifest.json'
    run = lambda lib, *extra: subprocess.run(
        [sys.executable, '-B', str(HERE / 'check_fund_ic_v6.py'), '--manifest', str(manifest), '--library', str(lib),
         '--baseline', str(HERE / gen.V6_LIBRARY), '--require-baseline-prefix', '--expect-added', 'sv_flow', *extra],
        capture_output=True, text=True, timeout=60)
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names]}))
    out = run(HERE / gen.LIBRARY)
    assert out.returncode == 0, out.stdout
    assert 'baseline prefix: 38 entries identical' in out.stdout and "added 1 ['sv_flow']" in out.stdout
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names if n != 'sv_ratio126']}))
    out = run(HERE / gen.LIBRARY)
    assert out.returncode == 1 and "declared field 'sv_ratio126' is not in the manifest" in out.stdout
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names]}))
    bad = json.loads((HERE / gen.LIBRARY).read_bytes())
    bad['candidates'][5]['tier'] = 'A'
    path = tmp_path / 'bad.json'
    path.write_text(json.dumps(bad))
    out = run(path)
    assert out.returncode == 1 and 'baseline candidate 5 (ebit_ev) is not identical' in out.stdout
