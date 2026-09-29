"""Library v7.1 (generate_fund_ic_v71.py) and check_fund_ic_v6.py on v7.1: pure Python + numpy, no data.

The four wave-2 formulas are checked on the v4 synthetic panel, extended by synthetic stand-ins for the four fields-v9
fields, against independent numpy transcriptions of the registered definitions (library-v7-wave2-prereg.md section 1;
Sign / MaxP semantics from atx-engine alpha, as in the v7.0 tests).

Run: "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v71.py -q
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.dont_write_bytecode = True


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


gen = _load('_generate_fund_ic_v71_under_test', HERE / 'generate_fund_ic_v71.py')
v70test = _load('_test_generate_fund_ic_v70_helpers_for_v71', HERE / 'test_generate_fund_ic_v70.py')
v4test = v70test.v4test
WAVE1 = ['qmj_safety', 'nincr', 'q5_eg', 'smax5', 'res_mom_ind']
WAVE2 = ['ins_opp', 'inst_best_ideas', 'ftd_fail', 'ea_overdue']
NEW_FIELDS = ['ins_opportunistic_net', 'inst_best_ideas', 'ftd_shares_ratio21', 'ea_days_to_expected']
# The 63 rows of lo1-fields-v9 (manifest sha256 8fd00e9f...d7b8769b), in manifest order (names only).
FIELDS_V9 = [
    'si_shares', 'si_dtc', 'iv_atm_21d', 'iv_atm_63d', 'iv_atm_126d', 'earn_recent', 'shares_out', 'mkt_ret', 'be',
    'at', 'at_lag4', 'lt', 'che', 'debt', 'sale_ttm', 'gp_ttm', 'oi_ttm', 'ni_ttm', 'ni_q', 'ni_q_lag4', 'be_lag1q',
    'be_lag1q_lag4', 'cfo_ttm', 'capx_ttm', 'xrd_ttm', 'dvc_ttm', 'prstkc_ttm', 'sstk_ttm', 'txt_q', 'txt_q_lag4',
    'shrs_q', 'shrs_q_lag4', 'noa', 'noa_lag4', 'sue', 'fscore', 'me_company', 'grp_sic2', 'grp_ff12', 'grp_ff49',
    'sv_ratio126', 'ea_days_to_expected', 'ea_days_since', 'ea_window_pre5', 'ea_window_post3', 'ea_delay_days',
    'ea_time_of_day', 'ins_net_buy_ratio', 'ins_n_buyers', 'ins_n_sellers', 'ins_opportunistic_net', 'ins_cluster_buy',
    'k8_count_63', 'k8_item_material_21', 'k8_days_since_any', 'inst_own_share', 'inst_breadth_chg', 'inst_own_chg_q',
    'inst_best_ideas', 'inst_n_holders', 'ftd_shares_ratio21', 'regsho_threshold_days63', 'sv_offexchange_share126']
FIELDS_V9_LIVE = ROOT.parent / 'pool-2' / 'build-equity' / 'recent-fast-train-2020-2022-v2-lo1-fields-v9' / 'manifest.json'


@pytest.fixture(scope='module')
def docs():
    out = gen.documents()
    return out, json.loads(out[gen.LIBRARY]), json.loads(out[gen.RECIPE])


def git_text(commit: str, rel: str) -> str:
    """A file at a commit (git), else the working copy; skip when neither is available."""
    try:
        return subprocess.run(['git', 'show', f'{commit}:{rel}'], capture_output=True, check=True,
                              cwd=HERE).stdout.decode('utf-8')
    except (OSError, subprocess.CalledProcessError):
        path = ROOT / rel
        if not path.is_file():
            pytest.skip(f'{rel} is unavailable')
        return path.read_text(encoding='utf-8')


# ---------------------------------------------------------------- identity, determinism, pins
def test_documents_are_deterministic_committed_and_pinned(docs):
    raw, _, recipe = docs
    assert gen.documents() == raw
    for name, blob in raw.items():
        assert (HERE / name).read_bytes() == blob, name
    assert recipe['library']['sha256'] == hashlib.sha256(raw[gen.LIBRARY]).hexdigest()
    assert hashlib.sha256((HERE / gen.V70_LIBRARY).read_bytes()).hexdigest() == gen.V70_LIBRARY_SHA256
    assert hashlib.sha256((HERE / gen.V70_RECIPE).read_bytes()).hexdigest() == gen.V70_RECIPE_SHA256
    lf = (HERE / 'generate_fund_ic_v71.py').read_bytes().replace(b'\r\n', b'\n')
    assert recipe['generation']['generator_sha256'] == hashlib.sha256(lf).hexdigest()
    out = subprocess.run([sys.executable, '-B', str(HERE / 'generate_fund_ic_v71.py'), '--check'],
                         capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr
    assert "v7.0 44 byte-identical + ['ins_opp', 'inst_best_ideas', 'ftd_fail', 'ea_overdue']" in out.stdout


def test_v70_entries_are_a_byte_identical_prefix_and_wave2_is_appended(docs):
    raw, library, recipe = docs
    v70_bytes = (HERE / gen.V70_LIBRARY).read_bytes()
    v70, r70 = json.loads(v70_bytes), json.loads((HERE / gen.V70_RECIPE).read_bytes())
    assert library['candidates'][:44] == v70['candidates']          # same objects, same order (all 44, the
    region = lambda blob: blob.split(b'"candidates": [\n', 1)[1]   # non-admitted v7.0 members included)
    assert region(raw[gen.LIBRARY]).startswith(region(v70_bytes)[:-len(b'\n  ]\n}\n')] + b',\n')
    assert (library['schema'], library['id']) == (v70['schema'], 'fund_industry_ic_v71')
    assert [c['id'] for c in library['candidates'][44:]] == WAVE2 and len(library['candidates']) == 48 <= 56
    assert [c['id'] for c in library['candidates'][39:44]] == WAVE1
    # prereg section 5 identity (i): only two theme descriptions, the ownership_flow family and 4 field declarations
    assert library['fields'][:len(v70['fields'])] == v70['fields']
    assert [f['name'] for f in library['fields'][len(v70['fields']):]] == NEW_FIELDS
    assert [f['id'] for f in library['families']] == [f['id'] for f in v70['families']] + ['ownership_flow']
    changed = {f['id'] for f, g in zip(library['families'], v70['families']) if f != g}
    assert changed == {'short_interest', 'earnings_momentum'}
    assert library['families'][-1]['description'].startswith('Informed-owner flows from SEC ownership filings')
    assert set(library) == set(v70)
    want = {'ins_opp': ('ownership_flow', 'B-', 5), 'inst_best_ideas': ('ownership_flow', 'C+', 6),
            'ftd_fail': ('short_interest', 'C+', 6), 'ea_overdue': ('earnings_momentum', 'C+', 6)}
    for c in library['candidates'][44:]:
        assert (c['theme'], c['tier'], c['tier_rank']) == want[c['id']] and c['family'] == c['theme']
        assert c['prior_sign'] == 1 and c['citation'] and c['horizons'] == [5, 21, 63]
        assert c['sign_policy'] == 'train-rank-ic21' and list(c) == list(v70['candidates'][0])
    for key in ('templates', 'lineage'):
        assert recipe[key][:44] == r70[key], key
    rows = {r['id']: r for r in recipe['lineage'][44:]}
    assert [rows[i]['roster_order'] for i in WAVE2] == [45, 46, 47, 48]
    assert [rows[i]['raw_prior_direction'] for i in WAVE2] == [1, 1, -1, -1]
    assert [rows[i]['smoothing_form'] for i in WAVE2] == ['R(x)', 'R(decay_linear(x, 21))', 'R(x)', 'R(x)']
    assert all(rows[i]['ranking'] == 'cross_section' for i in WAVE2)
    members = {t['theme']: t['members'] for t in recipe['themes']}
    assert list(members) == [f['id'] for f in library['families']]
    assert members['ownership_flow'] == ['ins_opp', 'inst_best_ideas']
    assert members['short_interest'] == ['si_ratio', 'dtc', 'si_change', 'sv_flow', 'ftd_fail']
    assert members['earnings_momentum'] == ['sue', 'droe', 'chtax', 'ear', 'nincr', 'ea_overdue']
    unchanged = [t for t in recipe['themes'][:-1] if t['theme'] not in ('short_interest', 'earnings_momentum')]
    assert unchanged == [t for t in r70['themes'] if t['theme'] not in ('short_interest', 'earnings_momentum')]
    assert recipe['within_industry'] == r70['within_industry']  # no wave-2 member is within-industry (rule R1)
    t = recipe['trials']
    assert (t['admission_trials'], t['generated_candidates'], t['unchanged_candidates'], t['dsr_n']) == (4, 48, 44, 35)
    assert recipe['generation']['max_roster'] == 56 and recipe['generation']['appended'] == WAVE2
    assert recipe['generation']['parent_v70']['library_sha256'] == gen.V70_LIBRARY_SHA256
    assert recipe['data']['requires']['fields_v9'].count(gen.FIELDS_V9_MANIFEST_SHA256) == 1
    assert list(recipe['data']['wave2_fields']) == NEW_FIELDS
    for key in ('v6_changes', 'v61_changes', 'v70_changes', 'orientation', 'needs_new_field'):
        assert recipe[key] == r70[key], key


def test_dsl_strings_are_the_registered_text(docs):
    _, library, recipe = docs
    got = {c['id']: c['dsl'] for c in library['candidates'][44:]}
    assert got == gen.DSL
    # v7-prereg.md "Library v7.1" (as committed at the registration): the four backticked strings, in order
    prereg = git_text(gen.PREREG_COMMIT, gen.PREREG_PATH)
    section = prereg.split('## Library v7.1', 1)[1].split('\n## ', 1)[0]
    assert re.findall(r'`(rank\(.*?\))`', section) == [gen.DSL[i] for i in WAVE2]
    # the P4 text's table (section 1): the same strings, ids, themes and tiers
    wave2 = git_text(gen.PREREG_COMMIT, gen.WAVE2_PATH)
    rows = re.findall(r'^\| (\d) \| (\w+) \| (\w+)[^|]*\| ([A-C][+-]?) \| ([+-]1) \| `([^`]+)` \|', wave2, re.M)
    assert [(r[1], r[3], int(r[4]), r[5]) for r in rows] == [
        (x['id'], x['tier'], x['raw_prior_direction'], gen.DSL[x['id']]) for x in gen.MEMBERS]
    assert [r[2] for r in rows] == [x['theme'] for x in gen.MEMBERS]
    # the draft (section 2 at f43e54d9): five blocks = the four draft strings with eap_8k (dropped) third
    draft = git_text(gen.DRAFT_COMMIT, gen.DRAFT_PATH)
    blocks = re.findall(r'```\n(.*?)\n```', draft.split('## 2.', 1)[1].split('## 3.', 1)[0], re.S)
    assert len(blocks) == 5 and 'ea_days_to_expected' in blocks[2]
    assert blocks[:2] + blocks[3:] == [gen.DRAFT_DSL[i] for i in WAVE2]
    # exactly the two registered mechanical respellings; the other two strings are the draft's verbatim
    assert {i for i in WAVE2 if gen.DSL[i] != gen.DRAFT_DSL[i]} == set(gen.RESPELLINGS) == {'ins_opp', 'ea_overdue'}
    resp = recipe['literature']['respellings_v71']
    assert {k: (v['draft'], v['registered']) for k, v in resp.items()} == {
        'ins_opp': ('rank((ins_opportunistic_net / shares_out))', 'rank(ins_opportunistic_net)'),
        'ea_overdue': ('rank((-1 * max(sign(((ea_days_since + ea_days_to_expected) - 94.5)), 0)))',
                       'rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))')}


def test_static_budget_is_the_house_budget_and_matches_the_prereg_table(docs):
    _, library, recipe = docs
    sv = recipe['static_validation']
    got = {i: (len(s['extra_fields']), s['estimated_peak_slots'], s['dag_nodes'], s['prior_bars'], s['dsl_bytes'])
           for i, s in sv['v71'].items()}
    # library-v7-wave2-prereg.md section 2 (extra fields, peak slots, DAG nodes, prior bars, bytes)
    assert got == {'ins_opp': (1, 2, 2, 0, 27), 'inst_best_ideas': (1, 3, 4, 20, 39), 'ftd_fail': (1, 3, 4, 0, 31),
                   'ea_overdue': (1, 4, 8, 0, 53)}
    assert {i: s['extra_fields'] for i, s in sv['v71'].items()} == dict(zip(WAVE2, ([f] for f in NEW_FIELDS)))
    assert not hasattr(gen, 'SLOTS_EXCEPTIONS') and not hasattr(gen, 'EXTRAS_EXCEPTIONS')  # no ruling needed
    assert sv['max_estimated_peak_slots_exceptions'] == {'qmj_safety': 8}  # v7.0's, unchanged
    assert (sv['declared_extra_fields'], sv['referenced_extra_fields'], sv['max_estimated_peak_slots'],
            sv['extra_field_capacity']) == (40, 40, 8, 6)  # resident capacity 6 and max slots 8 unchanged
    m, table = gen.v4(), gen.field_table()
    for c in library['candidates'][:44]:  # the extended field table changes no parent parse
        assert gen.parse(c['dsl']) == gen.v70().parse(c['dsl']), c['id']
    for name in NEW_FIELDS:
        assert table[name].prior_bars == 0 and not table[name].group
    with pytest.raises(ValueError):
        gen.parse('rank(ins_net_buy_ratio)')  # a fields-v9 field v7.1 does not declare
    with pytest.raises(AssertionError):  # the house budget: 6 extra fields
        gen.validate('ins_opp', 'rank((((((ins_opportunistic_net + inst_best_ideas) + ftd_shares_ratio21) + '
                                'ea_days_to_expected) + at) + debt))')


# ---------------------------------------------------------------- formulas on the extended synthetic panel
DATES = (290, 305, 318, 329)
STALE = slice(310, 326)  # an all-NaN FTD stretch (the 16-session stale window)


@pytest.fixture(scope='module')
def panel():
    p = v4test.synthetic_panel()
    rng = np.random.default_rng(71)
    shape = p['close'].shape
    ins = np.where(rng.uniform(size=shape) < 0.6, 0.0, -rng.uniform(1e-5, 2e-3, shape))
    ins[rng.uniform(size=shape) < 0.05] = rng.uniform(1e-5, 5e-4)  # a few net buyers
    ins[:200, 0] = np.nan                                            # before classification is possible
    ins[:, 5] = np.nan                                               # no Form 4 (e.g. a foreign private issuer)
    best = v4test._steps(rng, 0.05, 9.0)
    best[280:284, 3] = np.nan                                        # a NaN inside the decay window
    ftd = np.where(rng.uniform(size=shape) < 0.1, 0.0, rng.uniform(1e-6, 3e-2, shape))
    ftd[STALE] = np.nan
    ftd[:, 9] = np.nan
    ea = np.floor(rng.uniform(-12, 63, shape))
    ea[DATES[1], :4] = [-1.0, 0.0, 1.0, -30.0]                        # -1 flagged, 0 not
    ea[:, 7] = np.nan
    return dict(p, ins_opportunistic_net=ins, inst_best_ideas=best, ftd_shares_ratio21=ftd, ea_days_to_expected=ea)


@pytest.fixture(scope='module')
def signals(panel, docs):
    _, library, _ = docs
    return {c['id']: v70test.ev(gen.parse(c['dsl'])[0], panel) for c in library['candidates'][44:]}


def rank_rows(x: np.ndarray, t: int) -> np.ndarray:
    return v4test._rank_row(x[t])


def test_ins_opp_ranks_the_net_opportunistic_ratio_with_ties_at_zero(panel, signals):
    x = panel['ins_opportunistic_net']
    for t in DATES:
        assert v70test.close_enough(signals['ins_opp'][t], rank_rows(x, t)), t
        zero = x[t] == 0
        assert len(set(signals['ins_opp'][t][zero])) == 1                  # zeros tie at one average rank
        assert np.isnan(signals['ins_opp'][t, 5])
    assert np.isnan(signals['ins_opp'][100, 0]) and np.isfinite(signals['ins_opp'][300, 0])


def test_inst_best_ideas_is_the_ranked_21_session_decay(panel, signals):
    x = panel['inst_best_ideas']
    for t in DATES:
        assert v70test.close_enough(signals['inst_best_ideas'][t], v4test._rank_row(v70test.decay21(x, t))), t
    assert np.isnan(signals['inst_best_ideas'][300, 3])            # the NaN blanks the decay for 21 sessions
    assert np.isfinite(signals['inst_best_ideas'][305, 3])


def test_ftd_fail_ranks_minus_the_fail_ratio_and_the_stale_window_stays_nan(panel, signals):
    x = panel['ftd_shares_ratio21']
    for t in DATES:
        assert v70test.close_enough(signals['ftd_fail'][t], rank_rows(-x, t)), t
    assert np.isnan(signals['ftd_fail'][STALE]).all()              # NaN, never 0: no fill
    t = DATES[0]
    hi, lo = np.nanargmax(x[t]), np.nanargmin(x[t])  # most fails: bottom; no fail (ties at 0): top
    assert signals['ftd_fail'][t, hi] == 0.0 and signals['ftd_fail'][t, lo] == np.nanmax(signals['ftd_fail'][t])


def test_ea_overdue_flags_a_negative_days_to_expected(panel, signals):
    x = panel['ea_days_to_expected']
    flag = np.where(np.isnan(x), np.nan, -(x < 0).astype(float))
    for t in DATES:
        assert v70test.close_enough(signals['ea_overdue'][t], rank_rows(flag, t)), t
    t = DATES[1]
    s = signals['ea_overdue'][t]
    assert s[0] == s[3] == np.nanmin(s) < s[1] == s[2] == np.nanmax(s)  # -1 and -30 overdue; 0 and 1 not
    assert np.isnan(s[7])
    base = v70test.ev(gen.parse(gen.base_of(gen.MEMBERS[3], gen.DSL['ea_overdue']))[0], panel)
    assert set(np.unique(base[np.isfinite(base)])) == {-1.0, 0.0}


def test_members_are_causal_and_lookback_is_sufficient(panel, docs):
    _, library, _ = docs
    rng = np.random.default_rng(13)
    t0 = 300
    future = {k: v.copy() for k, v in panel.items()}
    for v in future.values():  # scramble every field strictly after t0
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    for c in library['candidates'][44:]:
        tree, _, _, prior_bars, _, _ = gen.parse(c['dsl'])
        clean = v70test.ev(tree, panel)
        assert v4test.same(clean[:t0 + 1], v70test.ev(tree, future)[:t0 + 1]), f"{c['id']} reads the future"
        assert np.isfinite(clean[prior_bars + 1:]).any(), f"{c['id']} never defined"
        past = {k: v.copy() for k, v in panel.items()}
        cut = t0 - prior_bars
        for v in past.values():
            v[:cut] = v[:cut][::-1]
        assert v4test.same(clean[t0], v70test.ev(tree, past)[t0]), f"{c['id']} reads beyond prior_bars={prior_bars}"


# ---------------------------------------------------------------- consumers: fitter, checker
def test_fitter_reads_the_labels_and_ew_theme_goes_to_ten_themes(docs):
    fcw = _load('_fcw_for_v71', HERE.parent / 'tools' / 'fit_composition_weights.py')
    lib_path, rec_path = HERE / gen.LIBRARY, HERE / gen.RECIPE
    lib_sha, rec_sha = (hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib_path, rec_path))
    rows = fcw.load_library(lib_path, lib_sha)
    priors = fcw.load_priors(lib_path, lib_sha, rec_path, rec_sha, rows)
    assert priors['prior_signs'] == [1] * 48 and priors['source'] == 'library+recipe'
    assert priors['themes'][44:] == ['ownership_flow', 'ownership_flow', 'short_interest', 'earnings_momentum']
    assert priors['tiers'][44:] == ['B-', 'C+', 'C+', 'C+'] and set(priors['themes']) <= set(fcw.PRIOR_THEMES)
    # prereg section 5 "Theme weights" on a synthetic admitted set (no data): the 9 v7.0 themes, one member each,
    # plus both ownership_flow members -> 1/10 per theme, .05 per ownership_flow member; without them, 1/9
    nine = ['value', 'profitability_quality', 'investment_issuance', 'earnings_momentum', 'price_momentum',
            'low_risk', 'short_interest', 'reversal_seasonality', 'options_implied']
    w, table = fcw.ew_theme_weights(nine + ['ownership_flow', 'ownership_flow'])
    assert np.allclose(w, [0.1] * 9 + [0.05, 0.05]) and len(table) == 10
    w, table = fcw.ew_theme_weights(nine)
    assert np.allclose(w, [1 / 9] * 9) and len(table) == 9
    w, _ = fcw.ew_theme_weights(nine + ['short_interest'])  # an admitted ftd_fail: 1/(n+1) of short_interest
    assert np.isclose(w[6], 1 / 18) and np.isclose(w[-1], 1 / 18)


def run_checker(manifest: Path, library: Path, *extra):
    return subprocess.run(
        [sys.executable, '-B', str(HERE / 'check_fund_ic_v6.py'), '--manifest', str(manifest), '--library',
         str(library), '--baseline', str(HERE / gen.V70_LIBRARY), '--max-roster', '56', '--require-baseline-prefix',
         '--expect-added', ','.join(WAVE2), *extra], capture_output=True, text=True, timeout=60)


def test_checker_accepts_v71_against_the_fields_v9_rows(tmp_path, docs):
    manifest = tmp_path / 'manifest.json'
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in FIELDS_V9]}))
    out = run_checker(manifest, HERE / gen.LIBRARY)
    assert out.returncode == 0, out.stdout
    assert 'baseline prefix: 44 entries identical' in out.stdout and f'added 4 {WAVE2}' in out.stdout
    assert '(63 fields,' in out.stdout and 'roster 48 <= 56' in out.stdout
    ops = out.stdout.split('operators used', 1)[1].split('\n', 1)[0]
    assert "'max'" in ops and "'sign'" in ops and "'decay_linear'" in ops
    for missing in NEW_FIELDS:  # every wave-2 field must be a fields-v9 row
        manifest.write_text(json.dumps({'fields': [{'name': n} for n in FIELDS_V9 if n != missing]}))
        out = run_checker(manifest, HERE / gen.LIBRARY)
        assert out.returncode == 1 and f"declared field '{missing}' is not in the manifest" in out.stdout
    manifest.write_text(json.dumps({'fields': [{'name': n} for n in FIELDS_V9]}))
    out = run_checker(manifest, HERE / gen.LIBRARY, '--expect-added', 'ins_opp,inst_best_ideas,ftd_fail')
    assert out.returncode == 1 and 'appended ids' in out.stdout
    bad = json.loads((HERE / gen.LIBRARY).read_bytes())
    bad['candidates'][0]['dsl'] += ' '  # a changed parent entry breaks the prefix
    path = tmp_path / 'prefix.json'
    path.write_text(json.dumps(bad))
    out = run_checker(manifest, path)
    assert out.returncode == 1 and 'baseline candidate 0 (value_composite) is not identical' in out.stdout


@pytest.mark.skipif(not FIELDS_V9_LIVE.is_file() and not os.environ.get('ATX_FIELDS_V9_MANIFEST'),
                    reason='the lo1-fields-v9 manifest (pool-2 build-equity) is not present')
def test_checker_accepts_v71_against_the_real_fields_v9_manifest():
    path = Path(os.environ.get('ATX_FIELDS_V9_MANIFEST') or FIELDS_V9_LIVE)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == gen.FIELDS_V9_MANIFEST_SHA256
    assert [r['name'] for r in json.loads(path.read_bytes())['fields']] == FIELDS_V9
    out = run_checker(path, HERE / gen.LIBRARY)
    assert out.returncode == 0 and 'check: ok' in out.stdout, out.stdout


def test_generator_reads_no_run_output():
    source = (HERE / 'generate_fund_ic_v71.py').read_text(encoding='utf-8')
    for forbidden in ('admission.json', 'train_daily_ic.csv', 'summary.json', 'orientations.json', 'trials.jsonl',
                      'open(', 'read_text'):
        assert forbidden not in source, forbidden
