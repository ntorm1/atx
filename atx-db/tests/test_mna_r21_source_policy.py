import datetime as dt
from decimal import Decimal
from atx_db.alpha_panel import mna_exact_v2 as E
from atx_db.alpha_panel import terminal_mna_v2 as T
from tests.test_mna_exact_v2 import fixture


def test_r21_later_isolated_source_dlret_is_not_a_forward_label():
    args,kw,expected=fixture(year=2023)
    expected['return_policy']='source-only-native-dlret-r21'
    value=E.replay_exact(*args,expected=expected,**kw)
    assert value.dlret==Decimal('.2') and value.dlret_exact
    day=dt.date(2023,1,3)
    assert not T.complete_window_eligible(value,start_mark=dt.date(2023,1,2),entry=day,exit=day,
                                          calendar=(dt.date(2023,1,2),day),residual_treatment_source=None)
    expected['return_policy']='pre2023-native-qualified'
    blocked=E.replay_exact(*args,expected=expected,**kw)
    assert blocked.dlret is None and 'return_holdout' in blocked.reason
