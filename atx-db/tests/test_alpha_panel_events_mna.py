"""M&A deal text rules (events table ``mna``, S6.1): consideration, filer role, counterparty, names."""

from __future__ import annotations

from atx_db.alpha_panel import events_mna as M

TARGET_CASH = (
    "Item 1.01 Entry into a Material Definitive Agreement. On March 3, 2024, Acme Widgets, Inc., a Delaware "
    "corporation (the “Company”), entered into an Agreement and Plan of Merger (the “Merger Agreement”) "
    "with Globex Holdings Corp., a Delaware corporation (“Parent”), and Globex Merger Sub, Inc. (“Merger "
    "Sub”). Merger Sub will merge with and into the Company, with the Company surviving as a wholly owned "
    "subsidiary of Parent. At the effective time, each share of common stock of the Company (“Company Common "
    "Stock”) issued and outstanding will be converted into the right to receive $45.50 in cash, without interest, "
    "and one contingent value right. The Merger is expected to close in the third quarter.")
ACQUIRER_STOCK = (
    "On May 1, 2023, Big Bank Corp. (the “Company”) entered into an agreement with Small Bank, Inc. "
    "(“Target”). Merger Sub, a wholly owned subsidiary of the Company, will merge with and into Target. Each "
    "share of Target common stock will be converted into the right to receive 0.2800 of a share of Company common "
    "stock, par value $1.00 per share. Cash will be paid in lieu of fractional shares.")
MIXED = ("each outstanding share will be converted into the right to receive (i) $12.00 in cash and (ii) 1.25 shares "
         "of Parent Common Stock, at the election of the holder, subject to proration.")
TENDER = ("This Tender Offer Statement on Schedule TO relates to the offer by Purchaser to purchase all of the "
          "outstanding shares at a price of $30.25 per Share, net to the seller in cash, without interest.")


def test_target_cash_with_cvr() -> None:
    t = M.parse_merger_text(TARGET_CASH)
    assert t is not None
    assert (t.role, t.consideration_type, t.cash_per_share, t.stock_ratio, t.cvr) == ("target", "cash", 45.5, None, True)
    assert t.counterparty_name == "Globex Holdings Corp"


def test_acquirer_stock_ratio_and_target_name() -> None:
    t = M.parse_merger_text(ACQUIRER_STOCK)
    assert t is not None
    assert (t.role, t.consideration_type, t.stock_ratio, t.cash_per_share) == ("acquirer", "stock", 0.28, None)
    assert t.counterparty_name == "Small Bank, Inc"


def test_mixed_election() -> None:
    cash, ratio, cvr, election = M.parse_consideration(MIXED.split("right to receive", 1)[1])
    assert (cash, ratio, cvr, election) == (12.0, 1.25, False, True)


def test_tender_offer_price() -> None:
    t = M.parse_merger_text(TENDER)
    assert t is not None and t.tender_offer and t.cash_per_share == 30.25 and t.consideration_type == "cash"


def test_no_consideration_clause() -> None:
    assert M.parse_merger_text("The Company entered into a credit agreement with its lenders.") is None


def test_norm_name() -> None:
    assert M.norm_name("Globex Holdings Corp.") == M.norm_name("GLOBEX HOLDINGS CORPORATION") == "GLOBEX"
    assert M.norm_name("Procter & Gamble Co") == "PROCTER AND GAMBLE"
