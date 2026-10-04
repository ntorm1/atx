import datetime as dt
import numpy as np
import pytest
from atx_db.alpha_panel import risk_inputs as R


def test_ols_recovers_exposure_and_missing_rows_do_not_fill():
    x = np.linspace(-.03, .03, 252)
    y = .002 + 1.5*x
    result = R.regress(y, x[:, None], np.zeros(252))
    assert result['betas'][0] == pytest.approx(1.5)
    assert result['idio_vol'] < 1e-15
    x[:60] = np.nan
    assert R.regress(y, x[:, None], np.zeros(252))['betas'] == [None]


def test_future_and_duplicate_observations_rejected():
    day = dt.date(2022, 1, 1)
    with pytest.raises(ValueError):
        R.exposure(day, [{'session_date': day+dt.timedelta(days=1)}])
    with pytest.raises(ValueError):
        R.exposure(day, [{'session_date': day}, {'session_date': day}])


def test_latest_missing_factor_history_remains_missing():
    result = R.exposure(dt.date(2025, 1, 1), [])
    assert result['beta_252'] is None
    assert result['risk_missing_reason'] == 'missing_qualified_trailing_history'


def test_r21_later_exposure_uses_strictly_available_trailing_observations():
    day=dt.date(2023,12,31);cutoff=dt.datetime(2023,12,29,22,tzinfo=dt.UTC)
    x=np.linspace(-.03,.03,252)
    history=[{'session_date':dt.date(2023,1,1)+dt.timedelta(days=i),'ret':.002+1.5*v,'RF':0,
              'MktRF':v,'SMB':np.sin(i),'HML':np.cos(i),'RMW':np.sin(i*.3),'CMA':np.cos(i*.2),
              'factor_available_at':cutoff-dt.timedelta(days=1)} for i,v in enumerate(x)]
    assert R.exposure(day,history,cutoff=cutoff)['beta_252']==pytest.approx(1.5)
    with pytest.raises(ValueError,match='unavailable'):R.exposure(day,[{**history[0],'factor_available_at':cutoff}],cutoff=cutoff)
    with pytest.raises(ValueError,match='future'):R.exposure(day,[{**history[0],'session_date':day}],cutoff=cutoff)
    unknown=[{**r,'factor_available_at':None} for r in history]
    assert R.exposure(day,unknown,cutoff=cutoff)['beta_252'] is None
    assert R.exposure(day,history[:199],cutoff=cutoff)['beta_252'] is None


def test_styles_keep_missing_values():
    assert R.zscores([1, 3, None]) == pytest.approx([-.70710678, .70710678, None])


def test_pack_requires_strict_primitive_clock():
    row = {'session_date': '2022-01-04', 'security_id': 1, 'cutoff': '2022-01-03T22:00:00+00:00',
           'observations': [], 'primitives': {'ff49': {'value': 1, 'available_at': '2022-01-03T22:00:00+00:00'}}}
    with pytest.raises(ValueError, match='unavailable primitive'):
        R.pack(row)
