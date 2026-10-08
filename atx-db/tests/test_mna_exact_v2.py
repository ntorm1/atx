import datetime as dt
import hashlib
from dataclasses import replace
from decimal import Decimal

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import mna_exact_v2 as E
from atx_db.alpha_panel import mna_native_v2 as N
from atx_db.alpha_panel import mna_price_native_v2 as P
from atx_db.alpha_panel import terminal_mna_v2 as T


def fixture(stock=False, year=2020):
    day = dt.date(year, 1, 3)
    clock = dt.datetime(year, 1, 3, 17, 30, tzinfo=T.UTC)
    terms = "0.5 shares of Buyer common stock" if stock else "USD12 in cash"
    settlement = "stock consideration was delivered" if stock else "cash consideration was paid"
    document = f'''<ACCEPTANCE-DATETIME>{year}0103123000
ACCESSION NUMBER: 0000000001-20-000001
FILER:
 CENTRAL INDEX KEY: 0000000001
</SEC-HEADER>
<DOCUMENT><TEXT><p>On January 3, {year}, Acme completed the merger with Buyer. each share of Acme common stock was converted into the right to receive {terms}. On January 3, {year}, the {settlement}.</p></TEXT></DOCUMENT>'''.encode()
    docpin = N.NativePin("original-completion", hashlib.sha256(document).hexdigest(), "0000000001-20-000001", 1,
                         clock+dt.timedelta(days=1), clock, "native_sec_acceptance")
    table = pa.Table.from_pylist([{"securityID": 1, "tradingDate": day, "ticker_tk": "ACME", "close": 10.0},
                                 {"securityID": 2, "tradingDate": day, "ticker_tk": "BUY", "close": 30.0}])
    sink = pa.BufferOutputStream(); pq.write_table(table, sink); vendor = sink.getvalue().to_pybytes()
    def economic(name, ticker):
        class_quote = f"{name} common stock trades as {ticker}."
        currency_quote = "Prices are USD per share."
        raw_quote = "close is the unadjusted raw traded close."
        conversion_quote = "conversion terms are unchanged with no split or FX conversion."
        validity_quote = f"Effective from {day.isoformat()} through {day.isoformat()}."
        blob = " ".join((class_quote, currency_quote, raw_quote, conversion_quote, validity_quote)).encode()
        pin = P.EconomicPin("original-authoritative-"+ticker, hashlib.sha256(blob).hexdigest(), name, ticker,
                            "common stock", "USD", "share", "raw-USD-common-share", day, day, clock,
                            class_quote, currency_quote, raw_quote, conversion_quote, validity_quote,
                            "retrieval_floor", clock)
        return blob, pin
    target_blob, target_basis = economic("Acme", "ACME")
    acquirer_blob, acquirer_basis = economic("Buyer", "BUY")
    target = P.VendorPricePin("original-vendor", hashlib.sha256(vendor).hexdigest(), str(table.schema), 1, day,
                              "ACME", clock, target_basis.source_id)
    acquirer = replace(target, security_id=2, ticker="BUY", economic_source_id=acquirer_basis.source_id)
    candidate = T.Candidate(1, day, clock, continued=True)
    args = (candidate, document, docpin, vendor, target, target_blob, target_basis)
    kw = {"acquirer_pin": acquirer, "acquirer_economic": acquirer_blob, "acquirer_economic_pin": acquirer_basis} if stock else {}
    expected = {"candidate": E._canonical(candidate), "document_pin": E._canonical(docpin),
                "target_pin": E._canonical(target), "target_economic_pin": E._canonical(target_basis),
                "acquirer_pin": E._canonical(acquirer) if stock else None,
                "acquirer_economic_pin": E._canonical(acquirer_basis) if stock else None,
                "auxiliary_clock_inventory": {}, "expected_code": E.exact_code_pins(), "return_policy": "pre2023-native-qualified"}
    return args, kw, expected


@pytest.mark.parametrize("stock,proceeds,ret", [(False, Decimal(12), Decimal(".2")), (True, Decimal(15), Decimal(".5"))])
def test_genuine_original_native_conditional_cash_and_stock_returns(stock, proceeds, ret):
    args, kw, expected = fixture(stock)
    row = E.replay_exact(*args, expected=expected, **kw)
    assert row.proceeds_per_share == proceeds and row.dlret == ret and row.dlret_exact
    assert not row.dlret_imputed and not row.reason


def test_value_only_policy_and_holdout_never_compute_return():
    args, kw, expected = fixture()
    expected["return_policy"] = "value-only"
    row = E.replay_exact(*args, expected=expected, **kw)
    assert row.proceeds_per_share == 12 and row.dlret is None and not row.dlret_exact
    args, kw, expected = fixture(year=2023)
    row = E.replay_exact(*args, expected=expected, **kw)
    assert row.proceeds_per_share == 12 and row.dlret is None and "return_holdout" in row.reason


def test_id_hash_or_opaque_basis_boolean_cannot_certify():
    args, kw, expected = fixture()
    with pytest.raises(ValueError, match="independently pinned"):
        E.replay_exact(*args, expected={"admission_id": "yes", "certified": True})
    wrong = list(args); wrong[5] = b"unproved currency class basis"
    with pytest.raises(ValueError, match="economic source identity"):
        E.replay_exact(*wrong, expected=expected)


def test_adjusted_close_or_class_currency_mismatch_refuses():
    args, kw, expected = fixture()
    wrong = list(args); wrong[4] = replace(args[4], ticker="ADR")
    expected["target_pin"] = E._canonical(wrong[4])
    with pytest.raises(ValueError, match="dated native"):
        E.replay_exact(*wrong, expected=expected)
    wrong = list(args); wrong[6] = replace(args[6], currency="CAD")
    expected["target_pin"] = E._canonical(args[4]); expected["target_economic_pin"] = E._canonical(wrong[6])
    with pytest.raises(ValueError, match="unsupported"):
        E.replay_exact(*wrong, expected=expected)


def test_duplicate_original_price_rows_rejected():
    args, kw, expected = fixture()
    table = pq.read_table(pa.BufferReader(args[3])); sink = pa.BufferOutputStream()
    pq.write_table(pa.concat_tables([table, table]), sink); vendor = sink.getvalue().to_pybytes()
    wrong = list(args); wrong[3] = vendor; wrong[4] = replace(args[4], sha256=hashlib.sha256(vendor).hexdigest())
    expected["target_pin"] = E._canonical(wrong[4])
    with pytest.raises(ValueError, match="ambiguous native"):
        E.replay_exact(*wrong, expected=expected)


def test_economic_clock_cannot_be_backdated():
    args, kw, expected = fixture()
    wrong = list(args); wrong[6] = replace(args[6], available_at=args[6].available_at-dt.timedelta(days=1))
    expected['target_economic_pin'] = E._canonical(wrong[6])
    with pytest.raises(ValueError, match='retrieval floor'):
        E.replay_exact(*wrong, expected=expected)


def test_common_stock_words_do_not_certify_an_ads_economic_unit():
    args, kw, expected = fixture()
    wrong = list(args)
    quote = args[6].class_quote + ' These ADSs represent common stock.'
    blob = args[5].replace(args[6].class_quote.encode(), quote.encode())
    wrong[5] = blob
    wrong[6] = replace(args[6], sha256=hashlib.sha256(blob).hexdigest(), class_quote=quote)
    expected['target_economic_pin'] = E._canonical(wrong[6])
    with pytest.raises(ValueError, match='unsupported'):
        E.replay_exact(*wrong, expected=expected)


def test_ticker_reuse_is_not_native_security_identity():
    args, kw, expected = fixture()
    table = pq.read_table(pa.BufferReader(args[3]))
    data = table.to_pylist(); data[1]['ticker_tk'] = 'ACME'
    sink = pa.BufferOutputStream(); pq.write_table(pa.Table.from_pylist(data, schema=table.schema), sink)
    vendor = sink.getvalue().to_pybytes()
    wrong = list(args); wrong[3] = vendor; wrong[4] = replace(args[4], sha256=hashlib.sha256(vendor).hexdigest())
    expected['target_pin'] = E._canonical(wrong[4])
    with pytest.raises(ValueError, match='ticker/date security'):
        E.replay_exact(*wrong, expected=expected)
