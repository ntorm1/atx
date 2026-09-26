# Clocks: sessions, decision cutoff, entry, filing availability

Tier-1 v2 node 1.11. Code: `atx-db/src/atx_db/calendar.py` (sessions and clocks),
`atx-db/src/atx_db/_fundamental_clock.py` (FC1). Edge-case tests: `atx-db/tests/test_xnys_calendar.py`.

## The rule

> **A feature dated t is known at t 22:00 UTC; positions enter at the close of the next session; FC1 fundamentals
> are usable at SEC filing date + 46 h.**

| Clock | Definition | Function |
|---|---|---|
| Session | an XNYS rule session: a weekday that is not a full-day NYSE closure | `xnys_sessions(start, end)`, `is_session(d)` |
| Decision cutoff | a feature dated `t` is known at `t` 22:00 UTC (every date, session or not) | `decision_cutoff_utc(t)` (timezone-aware UTC) |
| Entry | the close of the first session strictly after `t` | `next_session(t)` |
| FC1 fundamental | SEC filing date 00:00 UTC + 46 h = the next calendar day's 22:00 UTC, i.e. a feature dated `filed + 1 day` | `_fundamental_clock.FUNDAMENTAL_CLOCK_POLICY` (`sec_filed_date_plus_46h_v1`) |

The `t` 22:00 UTC rule is for market-dated features (a bar, a session, a month end). A **date-only SEC stamp** (a
filing date without its acceptance time) always takes FC1 (filing date + 46 h), never the market-date 22:00 UTC rule:
EDGAR dates a filing accepted up to 17:30 ET (22:30 UTC in winter) on that same day, and Section 16 forms up to
22:00 ET, so `filed` 22:00 UTC can precede dissemination.

Consequences:

- **No same-session entry.** 22:00 UTC is after every XNYS close: 16:00 ET is 21:00 UTC in winter and 20:00 UTC in
  summer; an early close (13:00 ET) is 18:00 / 17:00 UTC. A position formed on information dated `t` is never filled
  at `t`'s own close.
- **Weekend and holiday dates.** A feature dated on a non-session (a Saturday filing's FC1 date, a holiday) is known at
  that date's 22:00 UTC and enters at the close of the next session.
- **FC1 composition.** A filing dated Monday is usable Tuesday 22:00 UTC and enters Wednesday's close; a Friday filing
  is usable Saturday 22:00 UTC and enters Monday's close; a filing on the Wednesday before Good Friday is usable
  Thursday 22:00 UTC and enters the following Monday's close.
- **Monthly formations** (research panel): formation = the month's last rule session `M`, cutoff = `M` 22:00 UTC, entry =
  the first observed date after `M` that is a rule session (`research/panel.py:month_end_calendar`, panel v9: a stray
  market_daily date is never an entry; a missing month-end session is never replaced by an earlier one). The price
  wave's provisional labels (`research/spine.py`, node 1.12) enter at the close of `next_session(eom)` and exit at the
  close of `next_session` of the h-th month end after `eom`.

## The session calendar

Rule-based, never derived from price bars (`calendar.py`, calendar basis `xnys_rules_v1`):

- New Year's Day (Sunday -> Monday; a Saturday New Year is **not** observed on the prior Friday), Martin Luther King Jr.
  Day (from 1998), Washington's Birthday, Good Friday, Memorial Day, Juneteenth (from 2022; Saturday -> Friday,
  Sunday -> Monday), Independence Day, Labor Day, Thanksgiving, Christmas (Saturday -> Friday, Sunday -> Monday).
- Unscheduled full-day closures (`NYSE_SPECIAL_CLOSURES`): 1994-04-27, 2001-09-11..14, 2004-06-11, 2007-01-02,
  2012-10-29, 2012-10-30, 2018-12-05, 2025-01-09.
- Early closes at 13:00 ET (`xnys_early_closes`, rules from 2000): the day after Thanksgiving; Christmas Eve on
  Monday-Thursday; July 3 on Monday, Tuesday or Thursday, and on Wednesday from 2013; before 2013 the Friday July 5
  after a Thursday Independence Day. Ad-hoc early closes are not modeled; daily bars cannot verify an early close
  (the 1.11 acceptance shows each listed early close at 0.33-0.83x the surrounding sessions' volume).
- Validity: measured against the retained `TickerHistory3.parquet` bars for 2012-03-26 -> 2026-09-18 (3,642 sessions,
  0 missing, 0 extra). Before 2012 only the closures listed above are known; future unscheduled closures are unknown
  until they happen.

## Bars against the calendar

A bar dated on a closure or a weekend is a **stray bar**: never a session, never a formation, never an entry.
`reconcile_sessions_with_bars(bar_dates, start, end)` returns `{'missing': sessions without bars, 'extra': stray bar
dates}`; the `trading_calendar` loader records it as the `sessions_reconcile_with_bars` quality check (the stray dates
with their row counts). The `trading_calendar` rows (`calendar_id='XNYS'`, `source='equity_daily_bars calendar'`, the
key every reader filters on) are the rule sessions over the `equity_daily_bars` date range, with `close_time` 13:00 on
early closes.

Readers still moving onto this module: `_forward_return_publication` (node 3.5), `identity_reconstruction.py`
session windows (node 3.3), the critical calendar-reconciliation DQC (node 5.1).
