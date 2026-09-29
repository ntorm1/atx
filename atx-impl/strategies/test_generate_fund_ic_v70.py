"""Library v7.0 (generate_fund_ic_v70.py) and the v7.0 mode of check_fund_ic_v6.py: pure Python + numpy, no data.

The five wave-1 formulas are checked on the v4 synthetic panel against independent numpy transcriptions of the
registered definitions (library-v7-draft section 1; W2 op semantics from atx-engine alpha/lit_ops.hpp).

Run: "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v70.py -q
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import re
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


gen = _load('_generate_fund_ic_v70_under_test', HERE / 'generate_fund_ic_v70.py')
v4test = _load('_test_generate_fund_ic_v4_helpers_for_v70', HERE / 'test_generate_fund_ic_v4.py')
WAVE1 = ['qmj_safety', 'nincr', 'q5_eg', 'smax5', 'res_mom_ind']
R = '((close / delay(close, 1)) - 1)'
# q5_eg as implemented, transcribed independently: RoF 2021 Table I Panel D tau = 1 slopes
Q5_ROF = ('group_rank(decay_linear(((((-0.029 * log((me_company / at))) + (0.516 * (cfo_ttm / at))) + (0.771 * '
          '((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))), 21), grp_ff12)')
DRAFT_PATH = '.superpowers/sdd/platform-20260928/library-v7-draft.md'


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
    assert hashlib.sha256((HERE / gen.V61_LIBRARY).read_bytes()).hexdigest() == gen.V61_LIBRARY_SHA256
    assert hashlib.sha256((HERE / gen.V61_RECIPE).read_bytes()).hexdigest() == gen.V61_RECIPE_SHA256
    lf = (HERE / 'generate_fund_ic_v70.py').read_bytes().replace(b'\r\n', b'\n')
    assert recipe['generation']['generator_sha256'] == hashlib.sha256(lf).hexdigest()
    out = subprocess.run([sys.executable, '-B', str(HERE / 'generate_fund_ic_v70.py'), '--check'],
                         capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr
    assert 'registry cross-check ok' in out.stdout


def test_v61_entries_byte_identical_and_wave1_appended(docs):
    raw, library, recipe = docs
    v61_bytes = (HERE / gen.V61_LIBRARY).read_bytes()
    v61, r61 = json.loads(v61_bytes), json.loads((HERE / gen.V61_RECIPE).read_bytes())
    assert library['candidates'][:39] == v61['candidates']          # same objects, same order
    region = lambda blob: blob.split(b'"candidates": [\n', 1)[1]
    assert region(raw[gen.LIBRARY]).startswith(region(v61_bytes)[:-len(b'\n  ]\n}\n')] + b',\n')  # byte-identical text
    assert library['fields'] == v61['fields']                         # wave 1 declares no field
    assert (library['schema'], library['id']) == (v61['schema'], 'fund_industry_ic_v70')
    assert [c['id'] for c in library['candidates'][39:]] == WAVE1 and len(library['candidates']) == 44 <= 56
    want = {'qmj_safety': ('low_risk', 'B', 4), 'nincr': ('earnings_momentum', 'C+', 6),
            'q5_eg': ('investment_issuance', 'B', 4), 'smax5': ('low_risk', 'B+', 3),
            'res_mom_ind': ('price_momentum', 'B', 4)}
    for c in library['candidates'][39:]:
        assert (c['theme'], c['tier'], c['tier_rank']) == want[c['id']] and c['family'] == c['theme']
        assert c['prior_sign'] == 1 and c['citation'] and c['horizons'] == [5, 21, 63]
        assert c['sign_policy'] == 'train-rank-ic21'
    changed = {f['id'] for f, g in zip(library['families'], v61['families']) if f != g}
    assert changed == {'low_risk', 'earnings_momentum', 'investment_issuance', 'price_momentum'}
    assert [f['id'] for f in library['families']] == [f['id'] for f in v61['families']]  # no ownership_flow here
    for key in ('templates', 'lineage'):
        assert recipe[key][:39] == r61[key], key
    rows = {r['id']: r for r in recipe['lineage'][39:]}
    assert [rows[i]['roster_order'] for i in WAVE1] == [40, 41, 42, 43, 44]
    assert [rows[i]['raw_prior_direction'] for i in WAVE1] == [1, 1, 1, -1, 1]
    assert rows['q5_eg']['ranking'] == 'within_industry_grp_ff12'
    assert all(rows[i]['ranking'] == 'cross_section' for i in WAVE1 if i != 'q5_eg')
    members = {t['theme']: t['members'] for t in recipe['themes']}
    assert members['low_risk'] == ['bac', 'smax', 'qmj_safety', 'smax5']
    assert members['investment_issuance'][-1] == 'q5_eg' and members['earnings_momentum'][-1] == 'nincr'
    assert members['price_momentum'][-1] == 'res_mom_ind'
    assert recipe['trials']['admission_trials'] == 5 and recipe['trials']['dsr_n'] == 34
    assert recipe['generation']['max_roster'] == 56 and recipe['generation']['appended'] == WAVE1


def test_dsl_strings_are_the_draft_verbatim_except_the_q5_slopes(docs):
    _, library, recipe = docs
    got = {c['id']: c['dsl'] for c in library['candidates'][39:]}
    assert got['q5_eg'] == Q5_ROF
    for term, nber, rof in gen.Q5_SLOPES:  # the only respelling: three slope literals, NBER -> RoF
        assert got['q5_eg'].count(rof) == 1 and gen.DRAFT_DSL['q5_eg'].count(nber) == 1, term
    back = got['q5_eg']
    for _, nber, rof in gen.Q5_SLOPES:
        back = back.replace(rof, nber)
    assert back == gen.DRAFT_DSL['q5_eg']
    assert {i: got[i] for i in WAVE1 if i != 'q5_eg'} == {i: gen.DRAFT_DSL[i] for i in WAVE1 if i != 'q5_eg'}
    assert all(R in got[i] for i in ('qmj_safety', 'smax5', 'res_mom_ind'))
    slopes = recipe['literature']['q5_eg_slopes']
    assert '-0.029 (t -5.63)' in slopes['used'] and '0.516 (t 12.75)' in slopes['used'] and '0.771' in slopes['used']
    assert '-0.031 (t -5.86)' in slopes['registered'] and slopes['differ'] is True
    try:  # the draft file at the registered commit (git), else the working copy
        text = subprocess.run(['git', 'show', f'{gen.DRAFT_COMMIT}:{DRAFT_PATH}'], capture_output=True, check=True,
                              cwd=HERE).stdout.decode('utf-8')
    except (OSError, subprocess.CalledProcessError):
        path = HERE.parents[1] / DRAFT_PATH
        if not path.is_file():
            pytest.skip('library-v7-draft.md is unavailable')
        text = path.read_text(encoding='utf-8')
    blocks = re.findall(r'```\n(.*?)\n```', text.split('## 2.', 1)[0], re.S)
    assert blocks == [gen.DRAFT_DSL[i] for i in WAVE1]


def test_extended_parser_matches_v4_and_the_engine_registry(docs):
    _, library, recipe = docs
    m, table = gen.v4(), gen.field_table()
    for c in library['candidates'][:39]:
        assert gen.parse(c['dsl']) == m.parse(c['dsl'], table), c['id']
    assert gen.registry_crosscheck().startswith('v7.0 registry cross-check ok')
    assert set(gen.V70_REGISTRY_ROWS).isdisjoint(m.REGISTRY)  # the pinned v4 copy is not modified
    stats = recipe['static_validation']['v70']
    assert {i: (s['prior_bars'], s['native_prior_bars']) for i, s in stats.items()} == {
        'qmj_safety': (272, 272), 'nincr': (272, 272), 'q5_eg': (272, 272), 'smax5': (41, 41),
        'res_mom_ind': (272, 272)}
    # lookback per typecheck analyze_lit_call: (w - 1) + child; the count / min-periods operand adds nothing
    assert gen.parse(f'rank(ts_topk_mean({R}, 21, 5))')[3] == 21
    assert gen.parse(f'rank(ts_mean_mp({R}, 231, 116))')[3] == 231
    assert gen.parse(f'rank(ts_beta_on({R}, mkt_ret, 252))')[3:5] == (252, 252)
    assert gen.parse('rank(ts_beta_on(close, mkt_ret, 252))')[3:5] == (252, 251)  # mkt_ret charges one prior bar


def test_static_budget_and_the_two_rulings(docs):
    _, _, recipe = docs
    sv = recipe['static_validation']
    got = {i: (s['estimated_peak_slots'], len(s['extra_fields'])) for i, s in sv['v70'].items()}
    assert got == {'qmj_safety': (8, 5), 'nincr': (7, 2), 'q5_eg': (6, 6), 'smax5': (6, 0), 'res_mom_ind': (5, 1)}
    assert gen.SLOTS_EXCEPTIONS == {'qmj_safety': 8} == sv['max_estimated_peak_slots_exceptions']
    assert gen.EXTRAS_EXCEPTIONS == {'q5_eg': 6} == sv['max_extras_per_candidate_exceptions']
    assert sv['v70']['q5_eg']['extra_fields'] == ['at', 'be_lag1q', 'cfo_ttm', 'grp_ff12', 'me_company', 'ni_q']
    with pytest.raises(AssertionError):  # the exceptions are per candidate: another id gets the house budget
        gen.validate('nincr', gen.member_dsl('qmj_safety'))
    with pytest.raises(AssertionError):
        gen.validate('nincr', gen.member_dsl('q5_eg'))
    assert sv['max_estimated_peak_slots'] == 8 and sv['extra_field_capacity'] == 6


@pytest.mark.parametrize('dsl', [
    f'rank(ts_topk_mean({R}, 21, 22))',            # k > w
    f'rank(ts_topk_mean({R}, 21))',                # arity
    f'rank(ts_mean_mp({R}, 231, 0))',              # m < 1
    f'rank(ts_std_mp({R}, 21.5, 5))',              # fractional window
    f'rank(ts_beta_on({R}, mkt_ret, 2))',          # w < regressors + 2
    f'rank(ts_beta_on({R}, mkt_ret, volume, 252))',  # this library registers the one-regressor form only
    'rank(max(close))',                            # arity
    'rank(sign(grp_ff12))',                        # a Group classifier as a numeric operand
    'rank(ts_resid_on(close, mkt_ret, 252))',      # not registered for v7.0
])
def test_validator_refuses_malformed_w2_uses(dsl):
    with pytest.raises(ValueError):
        gen.parse(dsl)


# ---------------------------------------------------------------- formulas on the v4 synthetic panel
NEW_OPS = {'Sign', 'MaxP', 'TsBetaOn', 'TsTopkMean', 'TsMeanMp', 'TsStdMp'}


def ev(node, panel):
    """The v4 reference evaluator plus the six added rows (lit_ops.hpp semantics)."""
    kind = node[0]
    if kind in ('num', 'field'):
        return v4test.evaluate(node, panel)
    with np.errstate(all='ignore'):
        if kind == 'bin':
            a, b = ev(node[2], panel), ev(node[3], panel)
            return {'+': a + b, '-': a - b, '*': a * b, '/': a / b}[node[1]]
        op, args = node[1], node[2:]
        if op not in NEW_OPS:
            sub, kids = dict(panel), []
            for k, a in enumerate(args):
                if a[0] == 'num':
                    kids.append(a)
                else:
                    sub[f'__arg{k}'] = ev(a, panel)
                    kids.append(('field', f'__arg{k}'))
            return v4test.evaluate(('call', op, *kids), sub)
        x = ev(args[0], panel)
        if op == 'Sign':
            return np.sign(x)
        if op == 'MaxP':
            y = ev(args[1], panel)
            return np.where(np.isnan(x) | np.isnan(y), np.nan, np.maximum(x, y))
        out = np.full(x.shape, np.nan)
        if op == 'TsBetaOn':
            y, w = ev(args[1], panel), int(args[2][1])
            for t in range(w - 1, x.shape[0]):
                a, b = x[t + 1 - w:t + 1], y[t + 1 - w:t + 1]
                ok = np.isfinite(a).all(axis=0) & np.isfinite(b).all(axis=0)
                bc = b - b.mean(axis=0)
                out[t] = np.where(ok, ((a - a.mean(axis=0)) * bc).sum(axis=0) / (bc * bc).sum(axis=0), np.nan)
            return out
        w, n = int(args[1][1]), int(args[2][1])
        for t in range(x.shape[0]):
            win = x[max(0, t + 1 - w):t + 1]
            if op == 'TsTopkMean':
                if t + 1 >= w:
                    ok = ~np.isnan(win).any(axis=0)
                    out[t] = np.where(ok, np.sort(win, axis=0)[-n:].mean(axis=0), np.nan)
                continue
            fin = np.isfinite(win)
            cnt = fin.sum(axis=0)
            mean = np.where(fin, win, 0.0).sum(axis=0) / cnt
            if op == 'TsMeanMp':
                out[t] = np.where(cnt >= n, mean, np.nan)
            else:
                ss = (np.where(fin, win - mean, 0.0) ** 2).sum(axis=0)
                out[t] = np.where(cnt >= max(n, 2), np.sqrt(ss / (cnt - 1)), np.nan)
        return out


def decay21(x: np.ndarray, t: int) -> np.ndarray:
    w = x[t - 20:t + 1]
    return np.where(np.isnan(w).any(axis=0), np.nan, (np.arange(1, 22)[:, None] * w).sum(axis=0) / 231.0)


@pytest.fixture(scope='module')
def panel():
    return v4test.synthetic_panel()


@pytest.fixture(scope='module')
def signals(panel, docs):
    _, library, _ = docs
    return {c['id']: ev(gen.parse(c['dsl'])[0], panel) for c in library['candidates'][39:]}


def close_enough(a, b) -> bool:
    return bool(np.allclose(a, b, rtol=1e-9, atol=1e-12, equal_nan=True))


DATES = (300, 317, 329)


def test_q5_eg_is_the_rof_expected_growth_ranked_within_ff12(panel, signals):
    p = panel
    with np.errstate(all='ignore'):
        roe = p['ni_q'] / p['be_lag1q']
        droe = np.full(roe.shape, np.nan)
        droe[252:] = roe[252:] - roe[:-252]
        eg = (-0.029 * np.log(p['me_company'] / p['at']) + 0.516 * p['cfo_ttm'] / p['at'] + 0.771 * droe +
              0 * np.log(p['be_lag1q']))
    for t in DATES:
        want = v4test._group_row(decay21(eg, t), p['grp_ff12'][t], 'CsRankG')
        assert close_enough(signals['q5_eg'][t], want), t
    assert np.isnan(signals['q5_eg'][300, 11])                     # NaN FF12 label
    assert np.isfinite(signals['q5_eg'][300]).sum() >= 6


def test_nincr_counts_consecutive_year_on_year_increases_capped_at_5(panel, signals):
    p = dict(panel)
    base_tree = gen.parse(gen.base_of(gen.MEMBERS[1], gen.member_dsl('nincr')))[0]
    base = ev(base_tree, p)
    inc = np.where(np.isnan(p['ni_q'] - p['ni_q_lag4']), np.nan, (p['ni_q'] > p['ni_q_lag4']).astype(float))
    for t in DATES:
        i = [inc[t - 63 * k] for k in range(5)]
        want = i[0] * (1 + i[1] * (1 + i[2] * (1 + i[3] * (1 + i[4]))))
        assert close_enough(base[t], want), t
        assert set(np.unique(base[t][np.isfinite(base[t])])) <= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0}
        want_sig = v4test._rank_row(decay21(base, t))
        assert close_enough(signals['nincr'][t], want_sig), t
    q = {k: v.copy() for k, v in p.items()}
    q['ni_q'][:, 0] = q['ni_q_lag4'][:, 0] + 1.0                   # always up: the cap
    q['ni_q'][:, 1] = q['ni_q_lag4'][:, 1] + 1.0
    q['ni_q'][329 - 63, 1] = q['ni_q_lag4'][329 - 63, 1] - 1.0     # broken one quarter back: streak 1
    got = ev(base_tree, q)
    assert got[329, 0] == 5.0 and got[329, 1] == 1.0 and np.isnan(got[200, 0])  # below 252 prior sessions: NaN


def test_smax5_is_minus_max5_over_one_month_volatility(panel, signals):
    p = panel
    with np.errstate(all='ignore'):
        r = np.full(p['close'].shape, np.nan)
        r[1:] = p['close'][1:] / p['close'][:-1] - 1
        x = np.full(r.shape, np.nan)
        for t in range(21, r.shape[0]):
            w = r[t - 20:t + 1]
            x[t] = -(np.sort(w, axis=0)[-5:].mean(axis=0) / w.std(axis=0, ddof=1))
    for t in DATES:
        assert close_enough(signals['smax5'][t], v4test._rank_row(decay21(x, t))), t


def test_res_mom_ind_standardizes_the_ff49_excess_return_with_min_periods(panel, signals):
    p = panel
    with np.errstate(all='ignore'):
        r = np.full(p['close'].shape, np.nan)
        r[1:] = p['close'][1:] / p['close'][:-1] - 1
        e = r - np.vstack([v4test._group_row(r[t], p['grp_ff49'][t], 'CsMeanG') for t in range(r.shape[0])])
        z = np.full(r.shape, np.nan)
        for t in range(r.shape[0]):
            w = e[max(0, t - 230):t + 1]
            fin = np.isfinite(w)
            n = fin.sum(axis=0)
            mean = np.nansum(w, axis=0) / n
            sd = np.sqrt(np.nansum((w - mean) ** 2, axis=0) / (n - 1))
            z[t] = np.where(n >= 116, mean / sd, np.nan)
        zd = np.full(z.shape, np.nan)
        zd[21:] = z[:-21]
    for t in DATES:
        assert close_enough(signals['res_mom_ind'][t], v4test._rank_row(decay21(zd, t))), t
    # name 11 loses its FF49 label for 15 sessions (260..274): min periods keep the window alive
    assert np.isfinite(signals['res_mom_ind'][300, 11])


def test_qmj_safety_is_the_mean_rank_of_minus_beta_leverage_and_roe_volatility(panel, signals):
    p = panel
    with np.errstate(all='ignore'):
        r = np.full(p['close'].shape, np.nan)
        r[1:] = p['close'][1:] / p['close'][:-1] - 1
        beta = np.full(r.shape, np.nan)
        for t in range(251, r.shape[0]):
            a, b = r[t - 251:t + 1], p['mkt_ret'][t - 251:t + 1]
            bc = b - b.mean(axis=0)
            beta[t] = np.where(np.isfinite(a).all(axis=0), ((a - a.mean(axis=0)) * bc).sum(axis=0) / (bc * bc).sum(axis=0),
                               np.nan)
        lev = -(p['debt'] / p['at']) + 0 * np.log(p['at'])
        roe = p['ni_q'] / p['be_lag1q'] + 0 * np.log(p['be_lag1q'])
        evol = np.full(r.shape, np.nan)
        for t in range(251, r.shape[0]):
            w = roe[t - 251:t + 1]
            evol[t] = np.where(np.isnan(w).any(axis=0), np.nan, -w.std(axis=0, ddof=1))
    for t in DATES:
        legs = [v4test._rank_row(decay21(x, t)) for x in (-beta, lev, evol)]
        assert close_enough(signals['qmj_safety'][t], (legs[0] + legs[1] + legs[2]) / 3), t
    q = {k: v.copy() for k, v in p.items()}
    q['debt'][:, 2] *= 3.0                                            # more leverage: less safe
    got = ev(gen.parse(gen.member_dsl('qmj_safety'))[0], q)
    assert got[320, 2] <= signals['qmj_safety'][320, 2]


def test_members_are_causal_and_lookback_is_sufficient(panel, docs):
    _, library, _ = docs
    rng = np.random.default_rng(11)
    t0 = 300
    future = {k: v.copy() for k, v in panel.items()}
    for v in future.values():  # scramble every field strictly after t0
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    for c in library['candidates'][39:]:
        tree, _, _, prior_bars, _, _ = gen.parse(c['dsl'])
        clean = ev(tree, panel)
        assert v4test.same(clean[:t0 + 1], ev(tree, future)[:t0 + 1]), f"{c['id']} reads the future"
        assert np.isfinite(clean[prior_bars + 1:]).any(), f"{c['id']} never defined"
        past = {k: v.copy() for k, v in panel.items()}
        cut = t0 - prior_bars  # data before t0 - prior_bars must not matter at t0
        for v in past.values():
            v[:cut] = v[:cut][::-1]
        assert v4test.same(clean[t0], ev(tree, past)[t0]), f"{c['id']} reads beyond prior_bars={prior_bars}"


def test_fitter_reads_the_labels(docs):
    fcw = _load('_fcw_for_v70', HERE.parent / 'tools' / 'fit_composition_weights.py')
    lib_path, rec_path = HERE / gen.LIBRARY, HERE / gen.RECIPE
    lib_sha, rec_sha = (hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib_path, rec_path))
    rows = fcw.load_library(lib_path, lib_sha)
    priors = fcw.load_priors(lib_path, lib_sha, rec_path, rec_sha, rows)
    assert priors['prior_signs'] == [1] * 44 and priors['source'] == 'library+recipe'
    assert priors['themes'][39:] == ['low_risk', 'earnings_momentum', 'investment_issuance', 'low_risk',
                                     'price_momentum']
    assert priors['tiers'][39:] == ['B', 'C+', 'B', 'B+', 'B'] and set(priors['themes']) <= set(fcw.V4_THEMES)


def test_checker_v70_mode(tmp_path, docs):
    _, library, _ = docs
    names = [f['name'] for f in library['fields'] if f['name'] not in ('close', 'raw_close', 'volume')]
    manifest = tmp_path / 'manifest.json'
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names]}))
    run = lambda lib, *extra: subprocess.run(
        [sys.executable, '-B', str(HERE / 'check_fund_ic_v6.py'), '--manifest', str(manifest), '--library', str(lib),
         '--baseline', str(HERE / gen.V61_LIBRARY), '--max-roster', '56', '--require-baseline-prefix',
         '--expect-added', ','.join(WAVE1), *extra], capture_output=True, text=True, timeout=60)
    out = run(HERE / gen.LIBRARY)
    assert out.returncode == 0, out.stdout
    assert 'baseline prefix: 39 entries identical' in out.stdout and f"added 5 {WAVE1}" in out.stdout
    for op in ('ts_beta_on', 'ts_topk_mean', 'ts_mean_mp', 'ts_std_mp', "'max'", "'sign'"):
        assert op in out.stdout.split('operators used', 1)[1].split('\n', 1)[0], op

    def mutated(tag, cid, fn):
        bad = json.loads((HERE / gen.LIBRARY).read_bytes())
        c = next(c for c in bad['candidates'] if c['id'] == cid)
        c['dsl'] = fn(c['dsl'])
        path = tmp_path / f'{tag}.json'
        path.write_text(json.dumps(bad))
        return run(path)

    out = mutated('unknown', 'smax5', lambda d: d.replace('ts_topk_mean(', 'ts_topk_median('))
    assert out.returncode == 1 and 'smax5: operator ts_topk_median not in the allowlist' in out.stdout
    out = mutated('denied', 'nincr', lambda d: d.replace('rank(', 'rank(hump(', 1) + ')')
    assert out.returncode == 1 and 'nincr: forbidden operator hump' in out.stdout
    out = mutated('extras', 'nincr', lambda d: d.replace('ni_q_lag4))', '(ni_q_lag4 + (0 * (at + (debt + (sue + '
                                                                       'be)))))))', 1))
    assert out.returncode == 1 and 'nincr: 6 extra fields > 5' in out.stdout  # the 6-field ruling is q5_eg's only
    out = mutated('q5_7', 'q5_eg', lambda d: d.replace('(0 * log(be_lag1q))', '(0 * log((be_lag1q + debt)))'))
    assert out.returncode == 1 and 'q5_eg: 7 extra fields > 6' in out.stdout
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names if n != 'grp_ff49']}))
    out = run(HERE / gen.LIBRARY)
    assert out.returncode == 1 and "res_mom_ind: field 'grp_ff49' not in the manifest" in out.stdout
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in names]}))
    out = subprocess.run([sys.executable, '-B', str(HERE / 'check_fund_ic_v6.py'), '--manifest', str(manifest),
                          '--library', str(HERE / gen.LIBRARY), '--baseline', str(HERE / gen.V61_LIBRARY),
                          '--max-roster', '43'], capture_output=True, text=True, timeout=60)
    assert out.returncode == 1 and 'roster 44 > 43' in out.stdout
