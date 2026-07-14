from datetime import date

from atx_earnings import DateStatus, MarketSession
from atx_earnings.sources.nasdaq import parse_nasdaq_payload
from atx_earnings.sources.wikipedia import reconstruct_memberships


def test_nasdaq_parser_maps_reported_values():
    payload = {
        "data": {
            "rows": [
                {
                    "symbol": "AAPL",
                    "name": "Apple Inc.",
                    "time": "time-after-hours",
                    "fiscalQuarterEnding": "Jun/2026",
                    "epsForecast": "$1.42",
                    "eps": "$1.50",
                    "surprise": "5.63",
                    "noOfEsts": "31",
                    "lastYearRptDt": "7/31/2025",
                    "lastYearEPS": "(1.20)",
                }
            ]
        }
    }
    events = parse_nasdaq_payload(payload, date(2026, 7, 30))
    assert len(events) == 1
    assert events[0].session == MarketSession.AMC
    assert events[0].date_status == DateStatus.REPORTED
    assert events[0].fiscal_quarter_ending == date(2026, 6, 1)
    assert events[0].eps_estimate == 1.42
    assert events[0].reported_eps == 1.5
    assert events[0].surprise_percent == 5.63
    assert events[0].eps_prior_year == -1.2


def test_membership_changes_reconstruct_valid_intervals():
    current = {
        "NEW": {"company_name": "New Co", "sector": "Tech"},
        "STAY": {"company_name": "Stay Co", "sector": "Health"},
    }
    changes = [(date(2026, 3, 1), "NEW", "New Co", "OLD", "Old Co")]
    memberships = reconstruct_memberships(
        current=current,
        changes=changes,
        coverage_start=date(2025, 7, 1),
    )
    indexed = {(item.symbol, item.valid_from, item.valid_to) for item in memberships}
    assert ("NEW", date(2026, 3, 1), None) in indexed
    assert ("OLD", date(2025, 7, 1), date(2026, 3, 1)) in indexed
    assert ("STAY", date(2025, 7, 1), None) in indexed
