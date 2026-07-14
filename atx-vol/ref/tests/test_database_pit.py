from datetime import date, datetime, timezone

from atx_earnings import DateStatus, EarningsDatabase, EarningsEvent, MarketSession


def dt(hour: int) -> datetime:
    return datetime(2026, 7, 1, hour, tzinfo=timezone.utc)


def event(day: date) -> EarningsEvent:
    return EarningsEvent(
        symbol="BRK-B",
        event_date=day,
        source_record_key=f"BRK.B:{day}",
        session=MarketSession.AMC,
        date_status=DateStatus.ESTIMATED,
    )


def test_event_move_and_deletion_are_point_in_time(tmp_path):
    database = EarningsDatabase(tmp_path / "pit.sqlite")
    first_day = date(2026, 8, 1)
    moved_day = date(2026, 8, 2)
    database.add_earnings_snapshot(
        source="test",
        calendar_date=first_day,
        events=[event(first_day)],
        requested_at=dt(10),
        observed_at=dt(10),
    )
    database.add_earnings_snapshot(
        source="test",
        calendar_date=first_day,
        events=[],
        requested_at=dt(12),
        observed_at=dt(12),
    )
    database.add_earnings_snapshot(
        source="test",
        calendar_date=moved_day,
        events=[event(moved_day)],
        requested_at=dt(12),
        observed_at=dt(12),
    )

    early = database.events_between(first_day, moved_day, known_at=dt(11))
    late = database.events_between(first_day, moved_day, known_at=dt(13))

    assert [(row["symbol"], row["event_date"]) for row in early] == [
        ("BRK.B", "2026-08-01")
    ]
    assert [(row["symbol"], row["event_date"]) for row in late] == [
        ("BRK.B", "2026-08-02")
    ]


def test_failed_fetch_never_replaces_last_good_snapshot(tmp_path):
    database = EarningsDatabase(tmp_path / "failed.sqlite")
    day = date(2026, 8, 1)
    database.add_earnings_snapshot(
        source="test",
        calendar_date=day,
        events=[event(day)],
        requested_at=dt(10),
        observed_at=dt(10),
    )
    database.add_failed_snapshot(
        source="test",
        scope="earnings_date",
        scope_key=day.isoformat(),
        requested_at=dt(12),
        observed_at=dt(12),
        error="timeout",
    )

    rows = database.events_between(day, day, known_at=dt(13))
    assert len(rows) == 1
    assert rows[0]["event_date"] == day.isoformat()

