import datetime as dt
import hashlib
import json

import pytest

from atx_db.alpha_panel import factors as F


def test_holdout_rejected_before_source_iteration():
    def forbidden():
        raise AssertionError('source read')
        yield
    with pytest.raises(ValueError, match='window'):
        F.factor_period(dt.date(2023, 1, 1), forbidden(), dt.datetime.now(dt.UTC), 0)


def test_visibility_strict_and_cap_uncertainty_excluded():
    cutoff = dt.datetime(2022, 2, 28, 22, tzinfo=dt.UTC)
    rows = [F.Stock(1, 10, .1, True, {}, cutoff, True),
            F.Stock(77159, 10, .1, True, {}, cutoff-dt.timedelta(seconds=1), False),
            F.Stock(2, 10, .2, True, {}, cutoff-dt.timedelta(seconds=1), True)]
    assert [r.cik for r in F.visible(rows, cutoff)] == [2]


def test_duplicate_issuer_rejected():
    t = dt.datetime(2020, 1, 1, tzinfo=dt.UTC)
    r = F.Stock(1, 1, 0, True, {}, t, True)
    with pytest.raises(ValueError, match='consolidated'):
        F.visible([r, r], t+dt.timedelta(days=1))


def test_missing_leg_and_returns_are_not_filled():
    t = dt.datetime(2020, 1, 1, tzinfo=dt.UTC)
    r = F.Stock(1, 1, None, True, {'bm': 1}, t, True)
    assert F.vw([r]) is None
    assert F.sort_2x3([r], 'bm') == (None, None)


def test_snapshot_rejects_stale_output(tmp_path):
    data = tmp_path/'x'
    data.write_bytes(b'abc')
    manifest = tmp_path/'manifest.json'
    manifest.write_text(json.dumps({'stage': 'ratios_v11', 'status': 'complete', 'files': {
        'x': {'bytes': 3, 'sha256': hashlib.sha256(b'abc').hexdigest()}}}))
    sha = hashlib.sha256(manifest.read_bytes()).hexdigest()
    assert F.bind_snapshot(manifest, sha, 'ratios_v11')['stage'] == 'ratios_v11'
    data.write_bytes(b'xyz')
    with pytest.raises(ValueError, match='hash'):
        F.bind_snapshot(manifest, sha, 'ratios_v11')


def test_old_release_defaults_forbidden():
    with pytest.raises(ValueError, match='explicit'):
        F.require_release({})


def test_bab_beta_scaling():
    t = dt.datetime(2020, 1, 1, tzinfo=dt.UTC)
    rows = [F.Stock(1, 1, .03, True, {'beta': .5}, t, True),
            F.Stock(2, 1, .05, True, {'beta': 2}, t, True)]
    assert F.bab(rows, .01) == pytest.approx(.02)


def test_monthly_does_not_compound_long_short_spreads():
    with pytest.raises(NotImplementedError):
        F.monthly_compound([{'session_date': dt.date(2022, 1, 31)}])


def test_benchmark_missing_month_cannot_shrink_window():
    rows = [{'session_date': dt.date(y, m, 1), **{k: m+y/100 for k in F.FACTORS}}
            for y in range(2013, 2023) for m in range(1, 13)]
    result = F.benchmark_gates(rows[:-1], rows)
    assert result['MktRF']['status'] == 'FAIL'
    assert result['MktRF']['required_months'] == 120
    assert result['MktRF']['missing_months'] == 1
    assert result['MktRF']['rho'] is None


def test_benchmark_post_holdout_rejected():
    with pytest.raises(ValueError, match='post-2022'):
        F.benchmark_gates([{'session_date': dt.date(2023, 1, 1)}], [])
