"""Synthetic fixture for generate_fund_ic_v4.py: pure Python + numpy, no build, no real data.

Run: python -B -m pytest -q atx-impl/strategies/test_generate_fund_ic_v4.py -p no:cacheprovider

It checks the committed bytes, the prereg R1 roster (themes, member ids, order), the per-candidate
labels the T23 fitter reads (theme, tier, prior_sign, citation), the within-industry rule for
themes 1-3, the static validator's dtype rule for group fields, agreement with the v3 validator
on the shared grammar, and it evaluates every candidate on a random synthetic panel with a small
numpy mirror of the pinned VM semantics (vm.hpp / ts_ops.hpp / cs_ops.hpp / oracle.cpp: IEEE
element-wise ops, full-window any-NaN -> NaN, ts_backfill, average-tie rank r / (n - 1), group
ops over the non-NaN valid set with a NaN label left out) to check causality, the declared
lookback, the embedded prior signs, the domain guards and the earnings-announcement window. The
native VM remains authoritative.
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


gen = _load('_generate_fund_ic_v4_under_test', 'generate_fund_ic_v4.py')

# Prereg R1 (v4-prereg.md), transcribed independently of the generator, with the controller rulings:
# fix round 1 drops iv_change (no unambiguous prior for blended ATM IV); fix round 2 drops mgmt_sy and
# qmj_lite (components already members of the same themes; IC-runner memory cap).
PREREG_ROSTER = {
    'value': ['bm', 'ep', 'cfp', 'fcfp', 'ebit_ev', 'net_payout', 'sp', 'rd_me'],
    'profitability_quality': ['gpa', 'opbe', 'cfoa', 'roe_q', 'roa', 'accruals', 'fscore'],
    'investment_issuance': ['asset_growth', 'noa', 'issuance_xbrl', 'issuance_vendor'],
    'earnings_momentum': ['sue', 'droe', 'chtax', 'ear'],
    'price_momentum': ['mom_12_1', 'ind_mom_12_1', 'within_ind_mom', 'high_52w'],
    'low_risk': ['low_beta', 'low_ivol', 'low_max', 'lowvol_ind'],
    'short_interest': ['si_ratio', 'dtc', 'si_change'],
    'reversal_seasonality': ['ind_adj_rev_5', 'seasonality_same_month'],
    'options_implied': ['iv_rv_spread'],
}
THEMES_1_3 = ('value', 'profitability_quality', 'investment_issuance')


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


def test_roster_matches_prereg_r1(docs):
    _, library, recipe = docs
    expected = [cid for members in PREREG_ROSTER.values() for cid in members]
    assert [c['id'] for c in library['candidates']] == expected and len(expected) == 37
    assert [f['id'] for f in library['families']] == list(PREREG_ROSTER) == gen.THEME_IDS
    for c in library['candidates']:
        assert c['id'] in PREREG_ROSTER[c['family']] and c['theme'] == c['family']
    assert {t['theme']: t['members'] for t in recipe['themes']} == PREREG_ROSTER
    assert [r['roster_order'] for r in recipe['lineage']] == list(range(1, 38))
    assert recipe['trials']['admission_trials'] == 37 and recipe['trials']['variants_per_hypothesis'] == 1
    assert not {'iv_change', 'mgmt_sy', 'qmj_lite'} & {c['id'] for c in library['candidates']}
    assert recipe['generation']['revision'] == 'fix-round-2' and {r['round'] for r in recipe['revisions']} == {1, 2}


def test_labels_for_the_fitter_in_library_and_recipe(docs):
    _, library, recipe = docs
    for c, row in zip(library['candidates'], recipe['lineage'], strict=True):
        assert c['id'] == row['id']
        for key in ('theme', 'tier', 'tier_rank', 'prior_sign', 'citation'):
            assert c[key] == row[key], (c['id'], key)
        assert c['prior_sign'] == 1 and c['tier'] in gen.TIER_RANK and c['tier_rank'] == gen.TIER_RANK[c['tier']]
        assert isinstance(c['citation'], str) and len(c['citation']) > 10
        assert c['horizons'] == [5, 21, 63] and c['sign_policy'] == 'train-rank-ic21'
        assert row['raw_prior_direction'] in (-1, 1)
    assert recipe['labels']['tier_scale'] == gen.TIER_RANK
    assert recipe['orientation']['prior_sign'] == 1 and recipe['orientation']['sign_estimation'] is False


def test_within_industry_rule_for_themes_1_to_3(docs):
    _, library, recipe = docs
    wi = recipe['within_industry']
    assert wi['group_field'] == 'grp_ff12' and wi['exceptions'] == [] == gen.WITHIN_INDUSTRY_EXCEPTIONS
    grouped = []
    for c in library['candidates']:
        if c['family'] in THEMES_1_3:
            grouped.append(c['id'])
            assert c['dsl'].startswith('decay_linear(group_rank(') and c['dsl'].endswith(', grp_ff12), 21)'), c['id']
        else:
            assert not c['dsl'].startswith('decay_linear(group_rank(') and not c['dsl'].startswith('group_rank('), c['id']
    assert grouped == wi['members'] and len(grouped) == 19


def test_smoothing_default_without_exemptions(docs):
    _, library, recipe = docs
    assert recipe['generation']['smoothing_exemptions'] == {}  # fix round 2: ear carries its CAR, no NaN gaps
    for c, row in zip(library['candidates'], recipe['lineage'], strict=True):
        assert row['smoothing_sessions'] == 21 and c['dsl'].startswith('decay_linear(') and c['dsl'].endswith(', 21)')


def test_candidates_parse_against_declared_fields(docs):
    _, library, recipe = docs
    declared = {f['name'] for f in library['fields']}
    assert [f['name'] for f in library['fields']][:3] == ['close', 'raw_close', 'volume']
    referenced = set()
    for candidate, row in zip(library['candidates'], recipe['lineage'], strict=True):
        tree, shape, dtype, prior_bars, native, text = gen.parse(candidate['dsl'])
        fields = gen.fields_of(tree)
        assert shape == 'panel' and dtype == 'f64' and text == candidate['dsl'] and fields <= declared
        assert (prior_bars, native) == (row['prior_bars'], row['native_prior_bars']) and prior_bars <= gen.MAX_PRIOR_BARS
        assert row['dsl_sha256'] == hashlib.sha256(candidate['dsl'].encode()).hexdigest()
        assert sorted(fields) == row['fields']
        referenced |= fields
    assert referenced | {'raw_close', 'volume'} == declared  # every declared extra is used
    extras = declared - set(gen.BASE_FIELDS)
    assert len(extras) <= gen.MAX_EXTRA_FIELDS
    assert {f.name for f in gen.FIELDS if f.group} == {'grp_ff12', 'grp_ff49'}
    assert all(gen.is_group_field(f.name) == f.group for f in gen.FIELDS)
    sv = recipe['static_validation']
    assert sv['max_prior_bars'] <= gen.MAX_PRIOR_BARS and sv['max_estimated_peak_slots'] <= gen.MAX_SLOTS
    assert sv['extra_field_capacity'] == max(len(set(r['fields']) - set(gen.BASE_FIELDS)) for r in recipe['lineage'])


@pytest.mark.parametrize('dsl', [
    'rank(grp_ff12)',                              # a Group classifier is not a numeric primary
    'grp_ff12',                                    # nor a signal root
    '(grp_ff12 + 1)',                              # nor an arithmetic operand
    'delay(grp_ff49, 1)',                          # nor a time-series operand
    'group_rank(be, at)',                          # group operator needs a Group 2nd argument
    'group_rank(grp_ff12, grp_ff12)',              # group operator primary must be numeric
    'group_rank(be, (grp_ff12 * 1))',              # classifier must be a bare group field
    'group_rank(be)',                              # arity
    'group_zscore(be, grp_ff12)',                  # not in the v4 registry table
    'rank(grp_sic2)',                              # producer field deliberately not declared
    'rank(noa_lag4)',                              # producer field deliberately not declared
    'decay_linear(rank(be), 0)',                   # zero window
    'decay_linear(rank(be), 2.5)',                 # fractional window
    'delay(be, -1)',                               # negative shift (future reference)
    'rank( be)',                                   # non-canonical text
    'rank(be) be',                                 # trailing tokens
    'rank(be',                                     # unbalanced: end of input inside a call
    'group_rank(be,',                              # end of input after a comma
    'rank(be) $',                                  # lex error
])
def test_validator_rejects_malformed_dsl(dsl):
    with pytest.raises(ValueError):  # explicit rejection, never an IndexError from running off the tokens
        gen.parse(dsl)


def test_v3_validator_agrees_on_the_shared_grammar(docs):
    """Candidates inside the v3 validator's fields and operators get identical lookbacks from it."""
    _, library, _ = docs
    v3 = _load('_generate_pv_fields_ic121_v3_for_v4', 'generate_pv_fields_ic121_v3.py')
    registry, _, _, v3_fields = v3.tables()
    v3_opcodes = {sig[2] for sig in registry.values()}
    shared = 0
    for c in library['candidates']:
        tree = gen.parse(c['dsl'])[0]
        if gen.fields_of(tree) <= set(v3_fields) and gen.opcodes_of(tree) <= v3_opcodes:
            ours, theirs = gen.parse(c['dsl']), v3.parse(c['dsl'])
            assert (ours[3], ours[4], ours[5]) == (theirs[2], theirs[3], theirs[4]), c['id']
            assert gen.grammar().peak_slots(tree) == gen.grammar().peak_slots(theirs[0])
            shared += 1
    assert shared == 11  # ear, mom_12_1, high_52w, the three plain low-risk, the three SI, seasonality, iv_rv_spread


def test_registry_crosscheck_and_group_typing_status():
    status = gen.registry_crosscheck()
    assert status.startswith('registry cross-check ok') or status.startswith('registry cross-check skipped')
    assert 'grp_ group typing' in status or 'skipped' in status


PRE_T22 = """[[nodiscard]] inline bool is_group_field(std::string_view name) noexcept {
  constexpr std::string_view kPrefix = "IndClass.";
  if (name == "sector") return true;              // gics-derived classifier column
  return name.size() > kPrefix.size() && name.substr(0, kPrefix.size()) == kPrefix;
}
"""
T22 = """[[nodiscard]] inline bool is_group_field(std::string_view name) noexcept {
  constexpr std::string_view kIndClassPrefix = "IndClass.";
  constexpr std::string_view kGrpPrefix = "grp_";
  if (name == "sector") return true; // gics-derived classifier column
  return (name.size() > kIndClassPrefix.size() && name.starts_with(kIndClassPrefix)) ||
         (name.size() > kGrpPrefix.size() && name.starts_with(kGrpPrefix));
}
"""


def test_group_typing_static_check_recognizes_t22():
    assert gen.group_field_status(T22).startswith('grp_ group typing checked')
    assert 'PENDING T22' in gen.group_field_status(PRE_T22)
    assert 'PENDING T22' in gen.group_field_status(T22.replace('"grp_"', '"grp"'))
    assert gen.is_group_field('grp_ff12') and not gen.is_group_field('grp_') and not gen.is_group_field('ff12')


def test_generator_reads_no_v3_performance_output():
    source = (HERE / 'generate_fund_ic_v4.py').read_text(encoding='utf-8')
    for forbidden in ('admission.csv', '_daily_ic', 'summary.json', 'v3-library-dsl', 'validation-2023'):
        assert forbidden not in source, forbidden


# ---- numpy mirror of the pinned VM semantics (subset used by v4) ---------------
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


def _group_row(x: np.ndarray, g: np.ndarray, opcode: str) -> np.ndarray:
    out = np.full(x.shape, np.nan)
    valid = ~np.isnan(x) & ~np.isnan(g)
    for label in np.unique(g[valid]):
        members = valid & (g == label)
        if opcode == 'CsRankG':
            out[members] = _rank_row(np.where(members, x, np.nan))[members]
        elif opcode == 'CsNeutG':
            out[members] = x[members] - x[members].mean()
        elif opcode == 'CsMeanG':
            out[members] = x[members].mean()
        else:
            raise AssertionError(opcode)
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
        if opcode == 'Abs':
            return np.abs(x)
        if opcode == 'Spow':
            return np.sign(x) * np.abs(x) ** evaluate(args[1], panel)
        if opcode == 'CsRank':
            return np.vstack([_rank_row(r) for r in x])
        if opcode in ('CsRankG', 'CsNeutG', 'CsMeanG'):
            g = evaluate(args[1], panel)
            return np.vstack([_group_row(x[t], g[t], opcode) for t in range(shape[0])])
        y = evaluate(args[1], panel) if opcode == 'TsCorr' else None
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
            elif opcode == 'TsStd':
                value = w.std(axis=0, ddof=1)
            elif opcode == 'TsMax':
                value = w.max(axis=0)
            elif opcode == 'TsDecayLinear':
                weights = np.arange(1, d + 1, dtype=float)
                value = (weights[:, None] * w).sum(axis=0) / weights.sum()
            elif opcode == 'TsCorr':
                v = y[t + 1 - d:t + 1]
                ok &= ~np.isnan(v).any(axis=0)
                a, b = w - w.mean(axis=0), v - v.mean(axis=0)
                value = (a * b).sum(axis=0) / np.sqrt((a * a).sum(axis=0) * (b * b).sum(axis=0))
            else:
                raise AssertionError(f'evaluator lacks {opcode}')
            out[t] = np.where(ok, value, np.nan)
        return out


DATES, NAMES = 330, 12


def _steps(rng, low: float, high: float, period: int = 63) -> np.ndarray:
    """Quarterly-step fundamentals: each name refreshes every `period` sessions at its own phase."""
    out = np.empty((DATES, NAMES))
    for j in range(NAMES):
        phase, value = int(rng.integers(0, period)), rng.uniform(low, high)
        for t in range(DATES):
            if (t - phase) % period == 0:
                value = rng.uniform(low, high)
            out[t, j] = value
    return out


def synthetic_panel(seed: int = 7) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    close = 50 * np.exp(np.cumsum(rng.normal(0, 0.02, (DATES, NAMES)), axis=0))
    earn = np.zeros((DATES, NAMES))
    for j in range(NAMES):
        for event in range(int(rng.integers(0, 60)), DATES, 63):
            earn[event:event + 2, j] = 1.0
    earn[:, 0] = 0.0  # name 0 never reports
    si = rng.uniform(1e5, 5e7, (DATES, NAMES))
    for t in range(1, DATES):  # FINRA-like steps
        if t % 10:
            si[t] = si[t - 1]
    ff12 = np.repeat(np.array([[1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 1, np.nan]]), DATES, axis=0)
    ff12[200:, 10] = 2.0  # an industry reclassification
    ff49 = np.repeat(np.array([[10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 10, 11]], dtype=float), DATES, axis=0)
    ff49[260:275, 11] = np.nan  # an unlinked stretch
    at = _steps(rng, 1e9, 5e9)
    panel = dict(close=close, raw_close=close * rng.uniform(0.5, 2.0, NAMES),
                 volume=rng.uniform(1e5, 1e7, (DATES, NAMES)),
                 mkt_ret=np.repeat(rng.normal(0, 0.01, (DATES, 1)), NAMES, axis=1),
                 si_shares=si, si_dtc=np.maximum(1.0, rng.gamma(2.0, 2.0, (DATES, NAMES))),
                 iv_atm_21d=rng.uniform(0.15, 0.9, (DATES, NAMES)), earn_recent=earn,
                 shares_out=np.repeat(rng.uniform(1e7, 1e9, (1, NAMES)), DATES, axis=0) * _steps(rng, 0.97, 1.05, 126),
                 be=_steps(rng, -2e8, 3e9), at=at, at_lag4=at * _steps(rng, 0.8, 1.2), lt=at * _steps(rng, 0.2, 0.8),
                 che=_steps(rng, 1e7, 5e8), debt=_steps(rng, 0.0, 2e9), sale_ttm=_steps(rng, 5e8, 8e9),
                 gp_ttm=_steps(rng, 1e8, 3e9), oi_ttm=_steps(rng, -2e8, 1e9), ni_ttm=_steps(rng, -3e8, 8e8),
                 ni_q=_steps(rng, -1e8, 2e8), ni_q_lag4=_steps(rng, -1e8, 2e8), be_lag1q=_steps(rng, -1e8, 3e9),
                 be_lag1q_lag4=_steps(rng, -1e8, 3e9),
                 cfo_ttm=_steps(rng, -1e8, 1e9), capx_ttm=_steps(rng, 0.0, 5e8), xrd_ttm=_steps(rng, -1e8, 4e8),
                 dvc_ttm=_steps(rng, 0.0, 2e8), prstkc_ttm=_steps(rng, 0.0, 3e8), sstk_ttm=_steps(rng, 0.0, 2e8),
                 txt_q=_steps(rng, -2e7, 8e7), txt_q_lag4=_steps(rng, -2e7, 8e7), shrs_q=_steps(rng, 1e8, 2e8),
                 shrs_q_lag4=_steps(rng, 1e8, 2e8), noa=at * _steps(rng, 0.1, 0.9), sue=_steps(rng, -3.0, 3.0),
                 fscore=np.floor(_steps(rng, 0.0, 9.999)), grp_ff12=ff12, grp_ff49=ff49)
    panel['xrd_ttm'] = np.maximum(panel['xrd_ttm'], 0.0)  # zero R&D present (excluded by the guard)
    panel['me_company'] = panel['shares_out'] * panel['raw_close']
    return panel


def trees(library):
    return [(c['id'], gen.parse(c['dsl'])) for c in library['candidates']]


def same(a: np.ndarray, b: np.ndarray) -> bool:
    return bool(np.array_equal(a, b, equal_nan=True))


def test_candidates_are_causal_and_lookback_is_sufficient(docs):
    _, library, _ = docs
    base = synthetic_panel()
    rng = np.random.default_rng(11)
    t0 = 300
    future = {k: v.copy() for k, v in base.items()}
    for v in future.values():  # scramble every field strictly after t0
        v[t0 + 1:] = rng.permutation(v[t0 + 1:].ravel()).reshape(v[t0 + 1:].shape)
    for cid, (tree, _, _, prior_bars, _, _) in trees(library):
        clean = evaluate(tree, base)
        assert same(clean[:t0 + 1], evaluate(tree, future)[:t0 + 1]), f'{cid} reads the future'
        assert np.isfinite(clean[prior_bars + 1:]).any(), f'{cid} never defined'
        past = {k: v.copy() for k, v in base.items()}
        cut = t0 - prior_bars  # data before t0 - prior_bars must not matter at t0
        for v in past.values():
            v[:cut] = v[:cut][::-1]
        assert same(clean[t0], evaluate(tree, past)[t0]), f'{cid} reads beyond prior_bars={prior_bars}'


def _base(recipe, cid: str, panel) -> np.ndarray:
    template = next(t for t in recipe['templates'] if t['id'] == cid)
    return evaluate(gen.parse(template['base_dsl'])[0], panel)


def test_embedded_prior_signs(docs):
    """base = raw_prior_direction * raw quantity: the higher base value is the literature's long side."""
    _, _, recipe = docs
    p = synthetic_panel()
    c = p['close']
    ret = np.full(c.shape, np.nan)
    ret[1:] = c[1:] / c[:-1] - 1
    t = 290
    ivg = p['iv_atm_21d']  # inside the guard's open interval, so the guard is the identity here
    rv = np.nanstd(ret[t - 20:t + 1], axis=0, ddof=1) * np.sqrt(252)
    si_ratio = p['si_shares'] / p['shares_out']
    r5 = c[t] / c[t - 5] - 1
    r5_adj = r5 - np.array([r5[p['grp_ff49'][t] == g].mean() for g in p['grp_ff49'][t]])
    adj = p['shares_out'] * p['raw_close'] / c  # split-neutral share count (Daniel-Titman composite issuance)
    expected = {
        'si_ratio': -si_ratio[t], 'dtc': -p['si_dtc'][t], 'si_change': -(si_ratio[t] - si_ratio[t - 21]),
        'accruals': -(p['ni_ttm'][t] - p['cfo_ttm'][t]) / ((p['at'][t] + p['at_lag4'][t]) / 2),
        'asset_growth': -(p['at'][t] / p['at_lag4'][t] - 1), 'noa': -p['noa'][t] / p['at_lag4'][t],
        'issuance_xbrl': -np.log(p['shrs_q'][t] / p['shrs_q_lag4'][t]),
        'issuance_vendor': -np.log(adj[t] / adj[t - 252]),
        'mom_12_1': c[t - 21] / c[t - 252] - 1, 'high_52w': c[t] / c[t - 251:t + 1].max(axis=0),
        'seasonality_same_month': c[t - 224] / c[t - 245] - 1, 'low_max': -ret[t - 20:t + 1].max(axis=0),
        'ind_adj_rev_5': -r5_adj, 'iv_rv_spread': ivg[t] - rv,
        'chtax': (p['txt_q'][t] - p['txt_q_lag4'][t]) / p['at_lag4'][t],
        'net_payout': (p['dvc_ttm'][t] + p['prstkc_ttm'][t] - p['sstk_ttm'][t]) / p['me_company'][t],
        'droe': np.where((p['be_lag1q'][t] > 0) & (p['be_lag1q_lag4'][t] > 0),
                         p['ni_q'][t] / p['be_lag1q'][t] - p['ni_q_lag4'][t] / p['be_lag1q_lag4'][t], np.nan),
    }
    for cid, want in expected.items():
        assert np.allclose(_base(recipe, cid, p)[t], want, rtol=1e-9, atol=1e-12, equal_nan=True), cid
    rows = {r['id']: r for r in recipe['templates']}
    negative = {k for k, r in rows.items() if r['raw_prior_direction'] == -1}
    assert negative == {'accruals', 'asset_growth', 'noa', 'issuance_xbrl', 'issuance_vendor', 'low_beta', 'low_ivol',
                        'low_max', 'lowvol_ind', 'si_ratio', 'dtc', 'si_change', 'ind_adj_rev_5'}
    mkt = p['mkt_ret']
    r3 = np.array([ret[s - 2:s + 1].sum(axis=0) for s in range(t - 249, t + 1)])
    m3 = np.array([mkt[s - 2:s + 1].sum(axis=0) for s in range(t - 249, t + 1)])
    rho3 = np.array([np.corrcoef(r3[:, j], m3[:, j])[0, 1] for j in range(NAMES)])
    beta = rho3 * ret[t - 251:t + 1].std(axis=0, ddof=1) / mkt[t - 251:t + 1].std(axis=0, ddof=1)
    assert np.allclose(_base(recipe, 'low_beta', p)[t], -beta, rtol=1e-9)  # Frazzini-Pedersen beta, low beta long
    rho = np.array([np.corrcoef(ret[t - 20:t + 1, j], mkt[t - 20:t + 1, j])[0, 1] for j in range(NAMES)])
    ivol = ret[t - 20:t + 1].std(axis=0, ddof=1) * np.sqrt(np.abs(1 - rho * rho))
    assert np.allclose(_base(recipe, 'low_ivol', p)[t], -ivol, rtol=1e-9)  # residual volatility, low ivol long


def test_domain_guards_exclude_non_positive_denominators(docs):
    _, _, recipe = docs
    p = synthetic_panel()
    t = 250
    for name in ('be', 'ni_ttm', 'cfo_ttm', 'xrd_ttm', 'be_lag1q', 'oi_ttm'):  # force each exclusion at session t
        p[name][t, 3], p[name][t, 5] = -1.0e6, 0.0
    p['che'][t, 7] = p['me_company'][t, 7] + p['debt'][t, 7] + 1.0e6  # net cash above ME + debt: EV < 0
    p['oi_ttm'][t, 7] = abs(p['oi_ttm'][t, 7]) + 1.0
    p['be_lag1q'][t, 8], p['be_lag1q_lag4'][t, 8] = 1.0e9, -1.0e6  # only the year-earlier opening equity fails
    p['at'][t, 9], p['at_lag4'][t, 9] = 0.0, 1.0e9      # current assets zero: average assets stay positive
    p['at'][t, 10], p['at_lag4'][t, 10] = 1.0e9, -2.0e9  # lagged assets negative: average assets negative too
    ev = p['me_company'][t] + p['debt'][t] - p['che'][t]
    avg_at = (p['at'][t] + p['at_lag4'][t]) / 2
    rules = {
        'bm': p['be'][t] > 0, 'ep': p['ni_ttm'][t] > 0, 'cfp': p['cfo_ttm'][t] > 0, 'sp': p['sale_ttm'][t] > 0,
        'rd_me': p['xrd_ttm'][t] > 0, 'opbe': p['be'][t] > 0, 'roe_q': p['be_lag1q'][t] > 0,
        'droe': (p['be_lag1q'][t] > 0) & (p['be_lag1q_lag4'][t] > 0), 'ebit_ev': (p['oi_ttm'][t] > 0) & (ev > 0),
        'gpa': p['at'][t] > 0, 'roa': p['at'][t] > 0, 'cfoa': avg_at > 0, 'accruals': avg_at > 0,
        'asset_growth': p['at_lag4'][t] > 0, 'noa': p['at_lag4'][t] > 0, 'chtax': p['at_lag4'][t] > 0,
    }
    for cid, keep in rules.items():
        assert not keep.all() or cid == 'sp', cid  # the fixture exercises the exclusion (sales are always positive)
        base = _base(recipe, cid, p)[t]
        assert np.array_equal(np.isfinite(base), keep), cid
    for cid in ('fcfp', 'net_payout'):  # negative yields are kept
        assert np.isfinite(_base(recipe, cid, p)[t]).all(), cid


def test_group_rank_is_within_industry_and_nan_label_is_excluded(docs):
    _, library, _ = docs
    p = synthetic_panel()
    p['be'] = np.abs(p['be']) + 1.0
    dsl = next(c['dsl'] for c in library['candidates'] if c['id'] == 'bm')
    inner = dsl[len('decay_linear('):-len(', 21)')]  # group_rank(base, grp_ff12)
    ranked = evaluate(gen.parse(inner)[0], p)
    t = 250
    bm = p['be'][t] / p['me_company'][t]
    labels = p['grp_ff12'][t]
    assert np.isnan(ranked[t, 11]) and np.isnan(labels[11])  # no industry -> out of the signal
    for g in np.unique(labels[~np.isnan(labels)]):
        members = np.flatnonzero(labels == g)
        order = members[np.argsort(bm[members])]
        assert np.allclose(ranked[t, order], np.linspace(0.0, 1.0, len(members)))  # highest B/M ranks 1 in its group
    assert p['grp_ff12'][t, 10] == 2.0 and p['grp_ff12'][150, 10] == 1.0  # reclassification is honored per session
    group_150 = np.flatnonzero(p['grp_ff12'][150] == 1.0)
    assert 10 in group_150 and np.isfinite(ranked[150, group_150]).all()


def test_industry_momentum_members(docs):
    _, _, recipe = docs
    p = synthetic_panel()
    t = 290
    c = p['close']
    mom = c[t - 21] / c[t - 252] - 1
    labels = p['grp_ff49'][t]
    means = np.array([mom[labels == g].mean() for g in labels])
    assert np.allclose(_base(recipe, 'ind_mom_12_1', p)[t], means)
    assert np.allclose(_base(recipe, 'within_ind_mom', p)[t], mom - means)
    unlinked = _base(recipe, 'ind_mom_12_1', p)[260:275]
    assert np.isfinite(unlinked[:, 0]).all() and np.isnan(unlinked[:, 11]).all()  # NaN label -> NaN, others kept


def test_earnings_announcement_return_carries_the_most_recent_three_day_car(docs):
    _, _, recipe = docs
    p = synthetic_panel()
    earn = p['earn_recent']
    earn[:, 1] = 0.0
    earn[100:102, 1] = earn[250:252, 1] = 1.0  # a 149-session gap: longer than the 126-session carry
    c = p['close']
    ret = np.full(c.shape, np.nan)
    ret[1:] = c[1:] / c[:-1] - 1
    excess = ret - p['mkt_ret']
    base = _base(recipe, 'ear', p)
    carry = gen.EAR_CARRY
    for t in range(3, DATES):
        for j in range(NAMES):
            ends = [s for s in range(max(t - carry + 1, 3), t + 1) if earn[s, j] == 1 and earn[s - 1, j] == 1]
            if not ends:  # s = r + 1; none in [t - 125, t]
                assert np.isnan(base[t, j]), (t, j)
            else:
                want = excess[ends[-1] - 2:ends[-1] + 1, j].sum()  # the most recent event: sessions r-1, r, r+1
                assert base[t, j] == pytest.approx(want, rel=1e-12, abs=1e-15), (t, j)
    assert np.isnan(base[:, 0]).all()  # the never-reporting name is excluded, not zero
    assert np.isfinite(base[101:227, 1]).all() and np.isnan(base[227:251, 1]).all()  # carried 126 sessions, then NaN
    assert np.isfinite(base[251:, 1]).all()


def test_issuance_vendor_is_split_neutral(docs):
    """Review I1: a split or consolidation (shares_out and raw_close restated to the session basis) is not issuance."""
    _, _, recipe = docs
    plain = synthetic_panel()
    for factor in (4.0, 0.1):  # a 4:1 forward split and a 1:10 reverse split at session 200
        split = {k: v.copy() for k, v in plain.items()}
        split['shares_out'][200:] *= factor
        split['raw_close'][200:] /= factor
        a, b = _base(recipe, 'issuance_vendor', plain), _base(recipe, 'issuance_vendor', split)
        assert np.allclose(a, b, rtol=1e-12, atol=1e-12, equal_nan=True) and np.isfinite(a[260:]).all(), factor
    issued = {k: v.copy() for k, v in plain.items()}
    issued['shares_out'][200:] *= 1.25  # a genuine 25% issuance (price unchanged) is low-issuance-negative
    t = 300
    delta = _base(recipe, 'issuance_vendor', issued)[t] - _base(recipe, 'issuance_vendor', plain)[t]
    assert np.allclose(delta, -np.log(1.25))
