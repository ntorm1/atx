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
    assert result['risk_missing_reason'] == 'own_factor_history_ends_2022'


def test_styles_keep_missing_values():
    assert R.zscores([1, 3, None]) == pytest.approx([-.70710678, .70710678, None])


def test_pack_requires_strict_primitive_clock():
    row = {'session_date': '2022-01-04', 'security_id': 1, 'cutoff': '2022-01-03T22:00:00+00:00',
           'observations': [], 'primitives': {'ff49': {'value': 1, 'available_at': '2022-01-03T22:00:00+00:00'}}}
    with pytest.raises(ValueError, match='unavailable primitive'):
        R.pack(row)
