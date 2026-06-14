from atxpy import _core


def test_cost_cfg_defaults_and_kwargs():
    s = _core.SlippageCfg()
    assert s.k == 0.1 and s.bps == 5.0
    s.k = 0.2
    assert s.k == 0.2
    c = _core.CommissionCfg()
    assert c.per_share == 0.005
    f = _core.FillCfg()
    assert f.allow_same_bar_fill is False


def test_weight_policy_defaults():
    wp = _core.WeightPolicy()
    assert wp.transform == _core.Transform.Rank
    assert wp.dollar_neutral is True
    assert wp.gross_leverage == 1.0
    wp.transform = _core.Transform.ZScore
    assert wp.transform == _core.Transform.ZScore


def test_instrument_stats():
    st = _core.InstrumentStats()
    st.adv = 1e6
    assert st.adv == 1e6
