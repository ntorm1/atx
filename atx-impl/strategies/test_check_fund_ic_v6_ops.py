"""check_fund_ic_v6.py and the platform-v7 W2 literature DSL ops: pure Python, no data.

The checker must accept a synthetic library that uses every W2 op (W2_OPS) and keep refusing unknown operators,
the denylisted recurrences and a Group builder (bucket / group_cross) that feeds no group operator. The synthetic
candidates are fixtures only: none is added to any library.

Run: "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_check_fund_ic_v6_ops.py -q
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

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


chk = _load('_check_fund_ic_v6_under_test_w2', HERE / 'check_fund_ic_v6.py')

R = 'close / delay(close, 1) - 1'  # daily return
NINCR_X = f'ts_count_increases((ni_q != delay(ni_q, 1)) ? ((ni_q > delay(ni_q, 252)) ? 1 : -1) : 0, 546)'
FF3 = f'ts_resid_on({R}, ff_mkt, ff_smb, ff_hml, 252)'
# One or more candidates per W2 op, written as a library member would be (sign embedded, prior_sign +1).
SYNTHETIC = {
    'w2_max5_smax': f'rank(-1 * ts_topk_mean({R}, 21, 5) / stddev({R}, 21))',
    'w2_bac': f'group_rank(-1 * correlation({R}, ff_mkt, 252), bucket(stddev({R}, 252), 5))',
    'w2_ind_size_mom': 'group_neutralize(close / delay(close, 21) - 1, group_cross(grp_ff12, bucket(me_company, 3)))',
    'w2_resid_mom': f'rank(ts_sum(delay({FF3}, 21), 231) / stddev(delay({FF3}, 21), 231))',
    'w2_low_beta': f'rank(-1 * ts_beta_on({R}, ff_mkt, 252))',
    'w2_resid_mom_pack': f'rank(ts_sum(delay(ts_resid_on({R}, pack2(ff_mkt, ff_smb), 126), 21), 105))',
    'w2_cs_resid': 'rank(cs_resid_on(ni_q / me_company, log(me_company)))',
    'w2_cs_resid_pack': ('rank(cs_resid_on(ni_q / me_company, pack3(log(me_company), be / me_company, sale / at), '
                         'ni_q / at))'),
    'w2_nincr': f'rank(({NINCR_X} > 8) ? 8 : {NINCR_X})',
    'w2_mp_level': 'rank(ts_sum_mp(ni_q, 756, 252) / ts_mean_mp(me_company, 252, 63))',
    'w2_mp_stability': 'rank(-1 * ts_std_mp(ni_q / at, 1260, 504) + ts_zscore_mp(ni_q / at, 1260, 504))',
    'w2_mp_range': 'rank((ts_max_mp(close, 252, 126) - close) / (ts_max_mp(close, 252, 126) - ts_min_mp(close, 252, 126)))',
    'w2_mp_decay_corr': f'rank(decay_linear_mp(ni_q / at, 63, 21) - ts_corr_mp({R}, ff_mkt, 252, 126))',
}
FIELDS = ['close', 'raw_close', 'volume', 'ff_mkt', 'ff_smb', 'ff_hml', 'grp_ff12', 'me_company', 'ni_q', 'be',
          'sale', 'at']
MANIFEST = set(FIELDS) - set(chk.BASE_FIELDS)


def _cand(cid: str, dsl: str) -> dict:
    return dict(id=cid, family='lit', theme='lit', dsl=dsl, tier='B', prior_sign=1, horizons=[21],
                citation='synthetic W2 checker fixture (not a library member)')


def _library(cands: dict[str, str]) -> dict:
    return dict(schema='test', id='w2-synthetic', families=[dict(id='lit', description='W2 fixture')],
                fields=[dict(name=f) for f in FIELDS], candidates=[_cand(k, v) for k, v in cands.items()])


def test_every_w2_op_is_allowlisted_and_none_is_denied():
    assert chk.W2_OPS <= chk.ALLOWED_OPS
    assert not chk.W2_OPS & chk.DENIED_OPS
    assert chk.GROUP_BUILDERS <= chk.W2_OPS


def test_synthetic_library_uses_every_w2_op():
    used = {op for dsl in SYNTHETIC.values() for op in chk.names(dsl)[0]}
    assert chk.W2_OPS <= used, sorted(chk.W2_OPS - used)


def test_synthetic_library_using_every_w2_op_passes():
    assert chk.check(_library(SYNTHETIC), MANIFEST, max_roster=48) == []


@pytest.mark.parametrize('op', ['ts_topk_max', 'pack4', 'ts_resid', 'bucket_rank', 'ts_count_decreases'])
def test_unknown_operator_is_refused(op: str):
    errs = chk.check(_library({'bad': f'rank({op}(close, 21))'}), MANIFEST, max_roster=48)
    assert errs == [f'bad: operator {op} not in the allowlist']


@pytest.mark.parametrize('dsl', ['rank(bucket(me_company, 5))',
                                 'rank(close) + group_cross(grp_ff12, bucket(me_company, 3))'])
def test_group_builder_without_group_operator_is_refused(dsl: str):
    errs = chk.check(_library({'bad': dsl}), MANIFEST, max_roster=48)
    assert any('group builder' in e for e in errs), errs


@pytest.mark.parametrize('op', sorted(chk.DENIED_OPS))
def test_denied_operator_is_still_refused_next_to_w2_ops(op: str):
    errs = chk.check(_library({'bad': f'rank(ts_mean_mp({op}(close, 5), 21, 5))'}), MANIFEST, max_roster=48)
    assert f'bad: forbidden operator {op}' in errs


def test_vec_sum_is_allowed_only_as_an_explicit_policy_entry():
    # vec_sum (W2, q5 FWL slope); sign and max (L7: library v7.0 nincr's indicator max(sign(x), 0))
    assert chk.POLICY_OPS == frozenset({'vec_sum', 'sign', 'max'}) and chk.POLICY_OPS <= chk.ALLOWED_OPS
    assert {'vec_avg', 'min'}.isdisjoint(chk.ALLOWED_OPS)
    resid = 'cs_resid_on(delay(log(ni_q), 252) + 0 * (at - delay(at, 252)), delay(be, 252), delay(sale, 252))'
    slope = f'vec_sum({resid} * (at - delay(at, 252))) / vec_sum(power({resid}, 2))'
    q5_like = f'rank(ts_mean_mp({slope}, 252, 63) * log(ni_q))'
    assert chk.check(_library({'w2_q5_like': q5_like}), MANIFEST, max_roster=48) == []


def test_w2_op_field_outside_manifest_is_refused():
    errs = chk.check(_library({'bad': 'rank(ts_resid_on(close, ff_rmw, 63))'}), MANIFEST, max_roster=48)
    assert "bad: field 'ff_rmw' not in the manifest" in errs
