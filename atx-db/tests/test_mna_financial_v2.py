import datetime as dt
import hashlib
from dataclasses import asdict
from decimal import Decimal

import pytest

from atx_db.alpha_panel import mna_financial_v2 as F
from atx_db.alpha_panel import mna_native_v2 as N
from atx_db.alpha_panel.terminal_mna_v2 import UTC


def original(text):
    blob = f"<html><body>{text}</body></html>".encode()
    clock = dt.datetime(2020, 1, 3, 22, tzinfo=UTC)
    return blob, N.NativePin("synthetic-original", hashlib.sha256(blob).hexdigest(), "0000000001-20-000001", 1,
                            clock, clock, "retrieval_floor")


def test_named_unconditional_cash_and_native_dates():
    blob, pin = original("On January 3, 2020, Acme completed the merger with Buyer. "
                         "each share of Acme common stock was converted into the right to receive USD12 in cash. "
                         "On January 3, 2020, the cash consideration was paid.")
    rows = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())
    row = rows[0]
    assert row.cash_per_share == Decimal(12) and row.cash_currency == "USD"
    assert row.target_name == "Acme" and row.target_share_class == "common stock"
    assert row.completion_date == row.settlement_date == dt.date(2020, 1, 3)
    assert "historical_public_clock_unqualified_retrieval_floor" in row.reasons
    F.validate_financial_rows([asdict(row)], {pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())


def test_named_stock_clause_extracts_proceeds_ratio_not_return():
    blob, pin = original("On January 3, 2020, Acme completed the merger with Buyer. "
                         "each share of Acme common stock was converted into the right to receive 0.5 shares of Buyer common stock. "
                         "On January 3, 2020, the stock consideration was delivered.")
    row = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())[0]
    assert row.stock_ratio == Decimal(".5") and row.acquirer_name == "Buyer"
    assert row.consideration_type == "stock" and row.cash_per_share is None


def test_avon_source_shaped_alternatives_never_choose_ratio_or_par_value():
    # Generated root audit lead, not an actual source/clock certificate.
    blob, pin = original("On January 3, 2020, the Company consummated the previously announced business combination pursuant to the Agreement. "
                         'each share of common stock, par value $0.25 per share, of the Company was automatically converted into the ultimate right to receive '
                         '0.300 American Depositary Shares of HoldCo against the deposit of two HoldCo Shares, subject to adjustment, '
                         'and cash in lieu of fractional HoldCo ADSs or 0.600 HoldCo Shares for holders who have elected shares. '
                         'each share of Series C Preferred Stock was converted into the right to receive cash; no Preferred Stock Consideration will be payable.')
    row = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())[0]
    assert row.cash_per_share is None and row.stock_ratio is None and row.consideration_type is None
    assert len(row.consideration_clauses) == 2 and "missing_or_ambiguous_native_share_clauses" in row.reasons
    assert "ads_or_unit_conversion_unqualified" in row.reasons
    assert "elected_or_alternative_consideration" in row.reasons
    assert "preferred_or_compensation_class" in row.reasons


@pytest.mark.parametrize("clause", [
    "each share of Acme common stock was converted into the right to receive $12 in cash",
    "each share of Acme common stock was converted into the right to receive USD12 in cash or 0.5 shares of Buyer common stock",
    "each share of Acme common stock was converted into the right to receive USD12 in cash and a CVR",
    "each share of Acme common stock was converted into the right to receive 0.5 shares of Buyer common stock with cash in lieu of fractional shares",
])
def test_ambiguous_currency_contingencies_and_fractional_basis_stay_null(clause):
    blob, pin = original("On January 3, 2020, Acme completed the merger with Buyer. " + clause +
                         ". On January 3, 2020, the cash consideration was paid.")
    row = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())[0]
    assert row.consideration_type is None and row.cash_per_share is None and row.stock_ratio is None


def test_item_codes_are_not_native_completion_or_settlement():
    blob, pin = original("Item 2.01 Completion of Acquisition. each share of Acme common stock was converted into the right to receive USD12 in cash.")
    row = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())[0]
    assert row.status == "unknown" and row.completion_date is None and row.settlement_date is None
    assert row.cash_per_share == Decimal(12)  # announced value does not certify a terminal event


def test_public_financial_validator_rejects_manufactured_values_and_missing_inventory():
    blob, pin = original("No qualified financial terms")
    rows = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())
    manufactured = asdict(rows[0]); manufactured["cash_per_share"] = Decimal(999)
    with pytest.raises(ValueError, match="replay mismatch"):
        F.validate_financial_rows([manufactured], {pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())


def test_native_announcement_and_termination_are_distinct_document_vintages():
    blob, pin = original("On January 3, 2020, Acme announced the merger with Buyer. "
                         "each share of Acme common stock will be converted into the right to receive USD12 in cash.")
    row = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())[0]
    assert row.announce_date == dt.date(2020, 1, 3) and row.status == 'pending' and row.cash_per_share == 12
    blob, pin = original("On January 3, 2020, Acme terminated the merger with Buyer.")
    row = F.replay_financial({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code=F.financial_code_pins())[0]
    assert row.termination_date == dt.date(2020, 1, 3) and row.status == 'terminated'
