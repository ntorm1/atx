"""Buyback authorisation rules (events table ``buyback``, S6.3)."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import events_buyback as B

FILED = dt.date(2024, 10, 30)


def _ev(s: str, filed: dt.date = FILED) -> list[tuple[str, float | None, float | None, float | None]]:
    return [(a.event_type, a.amount_usd, a.amount_shares, a.total_after_usd) for a in B.extract_authorizations(s, filed)]


def test_new_program_amounts() -> None:
    assert _ev("In October, Abbott's board of directors authorized a new share repurchase program of up to $7 billion "
               "of the company's common shares.") == [("new", 7e9, None, None)]
    assert _ev("AMD announced that its board of directors approved a new $8 billion share repurchase program.") == [
        ("new", 8e9, None, None)]
    assert _ev("On September 29, 2024, the Board approved a new share repurchase program authorizing the repurchase of "
               "up to an aggregate of $2.0 billion of its Class B common stock.") == [("new", 2e9, None, None)]


def test_increase_and_totals() -> None:
    assert _ev("Board of Directors Authorized Additional $1.5 Billion Share Repurchase Program") == [
        ("increase", 1.5e9, None, None)]
    assert _ev("The Board also increased the Company's stock buyback authorization by $193 million to a total of "
               "$250 million.") == [("increase", 193e6, None, 250e6)]
    assert _ev("Company Increases Share Repurchase Authorization From $150M to $400M") == [
        ("increase", 250e6, None, 400e6)]
    assert _ev("In October, the Company increased its share repurchase authorization to $200 million.") == [
        ("increase", None, None, 200e6)]


def test_shares_authorization() -> None:
    assert _ev("Our board of directors approved a plan to repurchase an additional 300,000 shares.") == [
        ("increase", None, 300_000.0, None)]


def test_not_authorizations() -> None:
    for s in (
        "The total remaining authorization for future repurchases was $1.4 billion at the end of the quarter.",
        "The company repurchased 2.0 million shares for $124 million under the $3.0 billion stock repurchase program "
        "authorized by the Board of Directors.",
        "Announced a $250 million accelerated share repurchase program under the Company's $2.5 billion share "
        "repurchase authorization.",
        "Shareholders' equity of $27.864 billion increased 12%, primarily due to net income of $4.999 billion, "
        "partially offset by common share repurchases.",
        "The Company plans to repurchase up to $200 million of its shares in 2025.",
        "In December 2020, the Board authorized a new share repurchase program for up to $300 million.",
        "This program is in addition to the $4 billion share repurchase program announced last year.",
        "Returned $12 million to shareholders, leaving approximately 7.3 million shares available for repurchase "
        "under its authorized share repurchase program.",
        "The Company approved the repurchase of $500 million of its senior notes due 2026.",
    ):
        assert _ev(s) == [], s


def test_headline_and_body_merge() -> None:
    text = "\n".join(["Board Authorizes Additional $200 Million Share Repurchase Program",
                      "The Board of Directors authorized an additional $200 million share repurchase program."])
    assert _ev(text) == [("increase", 200e6, None, None)]
