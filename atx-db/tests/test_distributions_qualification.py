"""Offline original-format semantic fixtures, not actual source certification."""
import datetime as dt
import pytest
from atx_db.alpha_panel import distributions as D, distributions_qualification as Q

CLASS = b'''<html><xbrli:context id="c"><xbrli:entity><xbrli:identifier>900</xbrli:identifier></xbrli:entity></xbrli:context><ix:nonNumeric name="dei:EntityCentralIndexKey" contextRef="c">900</ix:nonNumeric><ix:nonNumeric name="dei:TradingSymbol" contextRef="c">ABC</ix:nonNumeric><ix:nonNumeric name="dei:Security12bTitle" contextRef="c">Class A common stock, par value 0.01</ix:nonNumeric></html>'''


def index(acc, stamp):
    return f'<html>{acc}<div>Accepted</div><div class="info">{stamp}</div></html>'.encode()


def inputs(validity=""):
    text = f"The board declared on January 4, 2021 a regular quarterly cash dividend of USD0.60 per share of Class A common stock{validity}."
    event, = D.extract_declarations(text)
    return event, text, dict(declaration_accession="decl", declaration_clock=index("decl", "2021-01-04 10:00:00"),
        class_document=CLASS, class_accession="cover", class_clock=index("cover", "2020-12-15 10:00:00"),
        line_rows=[{"cik": 900, "security_id": 10, "ticker": "ABC", "valid_from": dt.date(2020, 1, 1),
                    "valid_to": dt.date(2022, 1, 1), "available_at": D.clock("2020-12-15T16:00:00Z")}])


def test_positive_native_clock_class_fixed_rate_without_indefinite_carry():
    event, text, args = inputs()
    result = Q.qualify(event, text, **args)
    assert result["indicated_annual_dividend_rate"] == pytest.approx(2.4)
    assert result["available_at"] == D.clock("2021-01-04T15:00:00Z")
    assert result["security_id"] == 10
    assert not result["quote_currency_qualified"]
    assert Q.carry(result, D.clock("2021-02-01T22:00:00Z")) is None


def test_explicit_source_validity_clock_equality_and_expiry():
    event, text, args = inputs(", effective through March 31, 2021")
    result = Q.qualify(event, text, **args)
    assert Q.carry(result, result["available_at"]) is None
    assert Q.carry(result, D.clock("2021-03-31T22:00:00Z")) == pytest.approx(2.4)
    assert Q.carry(result, D.clock("2021-04-01T00:00:00Z")) is None


def test_native_clock_different_dst_seasons_and_ambiguous_hours():
    assert Q.accepted_clock(index("a", "2021-07-01 10:00:00"), "a") == D.clock("2021-07-01T14:00:00Z")
    for value in ("2021-11-07 01:30:00", "2021-03-14 02:30:00"):
        with pytest.raises(ValueError, match="ambiguous/nonexistent"):
            Q.accepted_clock(index("a", value), "a")
    with pytest.raises(ValueError, match="accession"):
        Q.accepted_clock(index("a", "2021-01-01 10:00:00"), "missing")


def test_no_class_expansion_future_link_or_variable_policy():
    event, text, args = inputs()
    args["line_rows"] *= 2
    with pytest.raises(ValueError, match="ambiguous"):
        Q.qualify(event, text, **args)
    event, text, args = inputs()
    args["line_rows"][0]["available_at"] = D.clock("2021-02-01T00:00:00Z")
    with pytest.raises(ValueError, match="future"):
        Q.qualify(event, text, **args)
    event, text, args = inputs()
    with pytest.raises(ValueError, match="variable"):
        Q.qualify(event, text + " This variable dividend policy pays one-third of net income.", **args)
    assert Q.native_classes(CLASS.replace(b"Class A common stock", b"American depositary shares")) == []


def test_policy_tombstones_and_future_declarations_do_not_rewrite_earlier_rate():
    event, text, args = inputs(", effective through March 31, 2021")
    result = {**Q.qualify(event, text, **args), "policy_state": Q.policy_state(text)}
    tombstone = {"security_id": 10, "available_at": D.clock("2021-02-01T15:00:00Z"),
                 "policy_state": Q.policy_state("The company will not pay a dividend.")}
    assert Q.rate_asof([result, tombstone], 10, D.clock("2021-01-31T22:00:00Z")) == pytest.approx(2.4)
    assert Q.rate_asof([result, tombstone], 10, D.clock("2021-02-02T22:00:00Z")) is None
    assert Q.policy_state("Pursuant to its variable dividend policy, it pays one-third of net income.") == "variable"
