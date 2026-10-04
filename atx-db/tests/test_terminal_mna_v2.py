import datetime as dt
from dataclasses import replace
from decimal import Decimal as D

import pytest

from atx_db.alpha_panel import terminal_mna_v2 as T
from atx_db.alpha_panel import mna_vintages_v2 as M


def fixture():
    day = dt.date(2020, 1, 2)
    ev = T.Evidence("synthetic", "a" * 64, dt.datetime(2020, 1, 3, tzinfo=T.UTC), "fixture-only")
    candidate = T.Candidate(1, day, ev.available_at)
    terms = T.Terms("deal", 1, "common", "share", "unadjusted", "USD", "cash", D(12), None,
                    None, None, None, None, day, day, "completed", ev, ev)
    target = T.Price(1, day, D(10), "USD", "common", "share", "unadjusted", ev)
    return candidate, terms, target


def test_no_caller_boolean_or_hash_certifies():
    c, t, p = fixture()
    result = T.value_terminal(c, t, p)
    assert result.proceeds_per_share == D(12)
    assert result.dlret is None and not result.dlret_exact and not result.dlret_imputed
    assert "native_replay_admission_unavailable" in result.reason


def test_stock_proceeds_and_formula_are_distinct():
    c, t, p = fixture()
    t = replace(t, consideration_type="stock", cash_per_share=None, stock_ratio=D(".5"),
                acquirer_security_id=2, acquirer_share_class="common", acquirer_unit="share",
                acquirer_basis="unadjusted", fractional_treatment="exact_per_target_share")
    a = replace(p, security_id=2, close=D(30))
    result = T.value_terminal(c, t, p, a)
    assert result.proceeds_per_share == D(15)
    assert T.offline_terminal_return(D(15), D(10), last_session=c.last_session,
                                   terminal_date=t.terminal_date, settlement_date=t.settlement_date) == D(".5")


@pytest.mark.parametrize("change,reason", [
    ({"status": "pending"}, "not_source_proven_completed"),
    ({"election": True}, "contingent_or_elected_consideration"),
    ({"cvr": True}, "contingent_or_elected_consideration"),
    ({"target_security_id": 99}, "target_security_mismatch"),
    ({"cash_per_share": None}, "missing_cash_terms"),
    ({"cash_per_share": D("NaN")}, "missing_cash_terms"),
])
def test_unresolved_proceeds_stay_null(change, reason):
    c, t, p = fixture()
    result = T.value_terminal(c, replace(t, **change), p)
    assert result.proceeds_per_share is None and result.dlret is None and reason in result.reason


def test_wrong_target_currency_keeps_proceeds_but_no_return():
    c, t, p = fixture()
    result = T.value_terminal(c, t, replace(p, currency="CAD"))
    assert result.proceeds_per_share == D(12) and result.dlret is None
    assert "target_class_unit_basis_currency_mismatch" in result.reason


def test_post2022_proceeds_kept_and_no_return():
    c, t, p = fixture()
    t = replace(t, settlement_date=dt.date(2023, 1, 2))
    result = T.value_terminal(c, t, p)
    assert result.proceeds_per_share == D(12) and result.dlret is None
    with pytest.raises(ValueError, match="return_holdout"):
        T.offline_terminal_return(D(12), D(10), last_session=c.last_session,
                                  terminal_date=t.terminal_date, settlement_date=t.settlement_date)


def test_population_no_selected_subset_or_ambiguous_matches():
    c, t, p = fixture()
    rows = T.preserve_population([c, replace(c, security_id=2)], {1: [t, t]}, {1: p}, {})
    assert len(rows) == 2 and all(row.dlret is None for row in rows)
    assert "ambiguous_deal_link" in rows[0].reason
    with pytest.raises(ValueError, match="duplicate"):
        T.preserve_population([c, c], {}, {}, {})


def test_gate_full_denominator_and_minimum_handcheck():
    g = T.gates(candidates=2267, exact=1800, deals=100, consideration=80, checked=199, correct=199)
    assert g["exact_dlret"]["status"] == "FAIL" and g["handcheck"]["status"] == "FAIL"
    assert g["consideration"]["status"] == "PASS"
    assert T.gates(candidates=10, exact=10, deals=0, consideration=0, checked=0, correct=0)["exact_dlret"]["status"] == "FAIL"


def test_complete_window_rejects_incomplete_or_holdout_calendar():
    c, t, p = fixture()
    value = T.value_terminal(c, t, p)
    assert not T.complete_window_eligible(value, start_mark=dt.date(2020, 1, 1), entry=c.last_session,
                                          exit=dt.date(2020, 1, 3), calendar=(dt.date(2020, 1, 1), c.last_session),
                                          residual_treatment_source=None)


def test_vintages_keep_later_terms_at_later_clock():
    _, t, _ = fixture()
    first = M.parse_native_candidate("a", "converted into the right to receive $10 in cash.", t.evidence)
    later = M.parse_native_candidate("b", "converted into the right to receive $12 in cash.",
                                    replace(t.evidence, available_at=t.evidence.available_at + dt.timedelta(days=10)))
    assert first.cash_per_share == D(10) and later.cash_per_share == D(12)
    assert first.available_at < later.available_at and first.status == later.status == "unknown"
    M.validate_vintages([first, later])
    with pytest.raises(ValueError, match="unique"):
        M.validate_vintages([first, first])


@pytest.mark.parametrize("text", ["no terms", "converted into the right to receive $10 in cash or $12 in cash",
                                    "converted into the right to receive $10 in cash and one CVR"])
def test_native_parser_preserves_unsupported_candidate(text):
    _, t, _ = fixture()
    row = M.parse_native_candidate("a", text, t.evidence)
    assert row.cash_per_share is None and row.consideration_type is None
