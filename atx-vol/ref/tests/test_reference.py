from datetime import date, datetime, timedelta, timezone

from atx_earnings import DateStatus, EarningsDatabase, EarningsEvent, MarketSession
from atx_earnings.models import UniverseMembership
from atx_earnings.pipeline import EarningsService


class UnusedSource:
    name = "unused"


def test_wide_reference_and_source_overlay(tmp_path):
    database = EarningsDatabase(tmp_path / "reference.sqlite")
    known = datetime(2026, 7, 1, 12, tzinfo=timezone.utc)
    as_of = date(2026, 7, 1)
    database.add_universe_snapshot(
        source="test-universe",
        universe="sp500",
        memberships=[
            UniverseMembership(
                symbol="AAPL",
                valid_from=date(2024, 7, 1),
                valid_to=None,
                company_name="Apple Inc.",
                sector="Information Technology",
            )
        ],
        requested_at=known - timedelta(hours=2),
        observed_at=known - timedelta(hours=2),
        coverage_start=date(2024, 7, 1),
    )
    event_dates = [date(2026, 7, 30), date(2026, 10, 30), date(2027, 1, 30), date(2027, 4, 30)]
    for index, event_date in enumerate(event_dates):
        database.add_earnings_snapshot(
            source="nasdaq",
            calendar_date=event_date,
            events=[
                EarningsEvent(
                    symbol="AAPL",
                    event_date=event_date,
                    source_record_key=f"nasdaq:{event_date}",
                    session=MarketSession.UNKNOWN,
                    date_status=DateStatus.ESTIMATED,
                    eps_estimate=1.0 + index,
                )
            ],
            requested_at=known - timedelta(hours=1),
            observed_at=known - timedelta(hours=1),
        )
    # A confirmed overlay within seven days supersedes the estimate in the consumer view.
    confirmed_day = date(2026, 7, 31)
    database.add_earnings_snapshot(
        source="csv-confirmed",
        calendar_date=confirmed_day,
        events=[
            EarningsEvent(
                symbol="AAPL",
                event_date=confirmed_day,
                source_record_key="ir:aapl:q2",
                session=MarketSession.AMC,
                date_status=DateStatus.CONFIRMED,
                eps_estimate=1.42,
            )
        ],
        requested_at=known,
        observed_at=known,
    )
    service = EarningsService(
        database,
        earnings_source=UnusedSource(),
        universe_source=UnusedSource(),
        request_delay_seconds=0,
    )
    build_id = service.materialize_reference(as_of, as_of, known_at=known)
    row = database.connection.execute(
        "SELECT * FROM reference_rows WHERE build_id=?", (build_id,)
    ).fetchone()

    assert row["symbol"] == "AAPL"
    assert row["event_1_date"] == "2026-07-31"
    assert row["event_1_session"] == "AMC"
    assert row["event_1_status"] == "CONFIRMED"
    assert row["event_1_source"] == "csv-confirmed"
    assert row["event_4_date"] == "2027-04-30"


def test_reference_builds_append_versions(tmp_path):
    database = EarningsDatabase(tmp_path / "versions.sqlite")
    day = date(2026, 7, 1)
    t1 = datetime(2026, 7, 1, 10, tzinfo=timezone.utc)
    database.add_universe_snapshot(
        source="universe",
        universe="sp500",
        memberships=[UniverseMembership("AAPL", day, None)],
        requested_at=t1,
        observed_at=t1,
        coverage_start=day,
    )
    event_day = date(2026, 8, 1)
    database.add_earnings_snapshot(
        source="nasdaq",
        calendar_date=event_day,
        events=[EarningsEvent("AAPL", event_day, "one")],
        requested_at=t1,
        observed_at=t1,
    )
    service = EarningsService(
        database,
        earnings_source=UnusedSource(),
        universe_source=UnusedSource(),
        request_delay_seconds=0,
    )
    first = service.materialize_reference(day, day, known_at=t1)
    second = service.materialize_reference(day, day, known_at=t1 + timedelta(hours=1))
    assert first != second
    assert database.connection.execute("SELECT COUNT(*) FROM reference_rows").fetchone()[0] == 2
    latest = database.connection.execute(
        "SELECT build_id FROM earnings_reference_latest WHERE symbol='AAPL'"
    ).fetchone()[0]
    assert latest == second
    pit = list(
        database.iter_reference_as_known_at(
            t1 + timedelta(minutes=30), as_of_date=day, symbol="AAPL"
        )
    )
    assert len(pit) == 1
    assert pit[0]["build_id"] == first
