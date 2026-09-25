"""Monthly survivorship-safe forward-return labels (R3a) and the shared label-validity SQL.

Publication
-----------
:func:`refresh_monthly_forward_labels` publishes source
``atx_forward_returns_survivorship_safe_monthly_v1`` into the warehouse table
``forward_returns_survivorship_safe`` through the same bounded publisher as the daily
``atx_forward_returns_survivorship_safe_v1`` source: same bar selection and price basis
(``adjusted_close`` by default), same observed ``XNYS`` bar calendar, same terminal
stitching and the same ``calculation_version``. Only two things differ:

* anchors (``as_of_date``) are restricted to the **entry session**: the first observed
  session after the last observed session of each closed calendar month
  (``month_end_next_session``). A month-end decision at 22:00 enters at that session's
  close, so a monthly label on anchor E equals the daily label on E (parity);
* horizons are 21/63/126/252 observed sessions (~1/3/6/12 months; formation units
  1/3/6/12, see :data:`HORIZON_FORMATION_UNITS`).

Row economics (inherited, unchanged): ``forward_return = P(E+h)/P(E) - 1``; when a
selected terminal's ``delist_date`` lies in ``(E, E+h]`` the leg ends at the last
positive price strictly before delisting and is compounded with the observed/policy
terminal return; anchors on/after a known terminal are excluded. A label whose window
spans a cessation WITHOUT a terminal row, or whose exact endpoint session has no bar,
is not published: that attrition is measured by :func:`monthly_label_diagnostics`,
never imputed here.

The publisher rebuilds the whole physical table through a validated shadow (other
sources are copied), so a monthly publication costs a full-table copy: heavy slot only.
"""

from __future__ import annotations

import datetime as dt
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

from .._forward_return_publication import CALCULATION_VERSION, refresh_forward_return_publication
from ..connection import DuckDBStore

MONTHLY_LABEL_SOURCE = "atx_forward_returns_survivorship_safe_monthly_v1"
DAILY_LABEL_SOURCE = "atx_forward_returns_survivorship_safe_v1"
MONTHLY_HORIZON_SESSIONS: tuple[int, ...] = (21, 63, 126, 252)
HORIZON_FORMATION_UNITS: dict[int, int] = {21: 1, 63: 3, 126: 6, 252: 12}
MONTHLY_FORMATION_RULE = "month_end_next_session"
LABEL_CALCULATION_VERSION = CALCULATION_VERSION


def label_revision_order_sql(label: str = "l") -> str:
    """ORDER BY list selecting one label revision per (security_id, as_of_date, horizon_days).

    Revision selection happens BEFORE validity: the newest visible row for a key wins,
    and a newer invalid row suppresses an older valid one (is_latest_revision ignored).
    """
    return f"{label}.available_at DESC,{label}.source_loaded_at DESC,{label}.forward_return_id DESC"


def label_status_sql(
    *,
    label: str = "l",
    calculation_version: str = "?",
    expected_end: str = "?",
    entry: str = "?",
    cutoff: str = "?",
) -> str:
    """FQ2 label-validity rules as one SQL CASE expression (no alias).

    Arguments are SQL expressions (column references or ``?``). With the defaults the
    expression binds five parameters in this order: ``calculation_version``,
    ``expected_end``, ``entry``, ``expected_end``, ``cutoff``. ``label`` is the alias of
    the selected (post-revision) label row; a NULL ``forward_return_id`` is ``missing``.

    Status: ``missing`` | ``unsupported_basis`` (not adjusted_close or another
    calculation version) | ``invalid`` (non-finite or < -1 return, wrong endpoint,
    inconsistent stitch/terminal provenance or arithmetic, terminal not in
    ``(entry, expected_end]``, or not visible/matured by ``cutoff``: available_at and
    the endpoint's next-day 12:00 must both be <= cutoff) | ``valid``.
    """
    a = label
    return f"""CASE WHEN {a}.forward_return_id IS NULL THEN 'missing'
                    WHEN {a}.price_basis<>'adjusted_close' OR {a}.price_basis IS NULL
                      OR {a}.calculation_version<>{calculation_version} OR {a}.calculation_version IS NULL
                      THEN 'unsupported_basis'
                    WHEN {a}.forward_return IS NULL OR NOT isfinite({a}.forward_return)
                      OR {a}.forward_return < -1 OR {a}.forward_end_date IS DISTINCT FROM {expected_end}
                      OR ({a}.is_delisted_in_horizon AND
                          (NOT {a}.is_stitched OR {a}.terminal_return IS NULL
                           OR NOT isfinite({a}.terminal_return) OR {a}.terminal_return < -1
                           OR {a}.raw_forward_return IS NULL OR NOT isfinite({a}.raw_forward_return)
                           OR {a}.raw_forward_return < -1 OR {a}.delist_date IS NULL
                           OR {a}.delist_date <= {entry} OR {a}.delist_date > {expected_end}
                           OR {a}.terminal_return_source NOT IN ('observed','policy')
                           OR {a}.terminal_return_source IS NULL
                           OR ({a}.terminal_return_source='observed'
                               AND {a}.return_observation_id IS NULL)
                           OR ({a}.terminal_return_source='policy'
                               AND {a}.return_observation_id IS NOT NULL)
                           OR abs({a}.forward_return -
                             ((1+{a}.raw_forward_return)*(1+{a}.terminal_return)-1))
                              > 1e-10*greatest(1.0,abs({a}.forward_return))))
                      OR (NOT {a}.is_delisted_in_horizon AND
                          ({a}.is_stitched OR {a}.terminal_return IS NOT NULL
                           OR {a}.delist_date IS NOT NULL OR {a}.terminal_return_source IS NOT NULL
                           OR {a}.return_observation_id IS NOT NULL
                           OR {a}.raw_forward_return IS NULL OR NOT isfinite({a}.raw_forward_return)
                           OR {a}.raw_forward_return < -1
                           OR abs({a}.forward_return-{a}.raw_forward_return)
                              > 1e-10*greatest(1.0,abs({a}.forward_return))))
                      OR greatest({a}.available_at,{a}.forward_end_date::TIMESTAMP+INTERVAL 1 DAY+INTERVAL 12 HOUR)>{cutoff}
                      THEN 'invalid'
                    ELSE 'valid' END"""


@dataclass(frozen=True)
class MonthlyForwardLabelOptions:
    source: str = MONTHLY_LABEL_SOURCE
    run_id: str | None = None
    # Same production basis as the daily panel; ``close`` is explicit compatibility.
    price_basis: str = "adjusted_close"
    # Observation vintage (not the formation date): realized-outcome labels.
    observation_cutoff: dt.datetime | None = None
    horizons: tuple[int, ...] = MONTHLY_HORIZON_SESSIONS


def _naive_utc(cutoff: dt.datetime | None) -> dt.datetime | None:
    if cutoff is not None and cutoff.tzinfo is not None:
        return cutoff.astimezone(dt.UTC).replace(tzinfo=None)
    return cutoff


def _calendar_keys() -> tuple[str, str]:
    # The daily panel's calendar identity; imported so the two sources cannot drift.
    from ..delisting import _TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE

    return _TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE


def refresh_monthly_forward_labels(
    store: DuckDBStore, options: MonthlyForwardLabelOptions | None = None,
) -> int:
    """Replace the monthly label source atomically; other sources are retained.

    Returns the number of published rows. Never targets the daily source.
    """
    from ..delisting import FORWARD_RETURN_SS_COLUMNS

    options = options or MonthlyForwardLabelOptions()
    if not options.source or options.source == DAILY_LABEL_SOURCE:
        raise ValueError("monthly labels need their own source; the daily panel is never replaced here")
    if options.price_basis not in {"adjusted_close", "close"}:
        raise ValueError("price_basis must be adjusted_close or close")
    calendar_id, calendar_source = _calendar_keys()
    store.initialize()
    return refresh_forward_return_publication(
        store, source=options.source, run_id=options.run_id,
        price_basis=options.price_basis, cutoff=_naive_utc(options.observation_cutoff),
        columns=FORWARD_RETURN_SS_COLUMNS, horizons=tuple(options.horizons),
        calendar_id=calendar_id, calendar_source=calendar_source,
        formation=MONTHLY_FORMATION_RULE,
    )


_ENTRY_SESSIONS_SQL = """
    calendar AS (
        SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
        FROM (SELECT DISTINCT trade_date FROM trading_calendar
              WHERE calendar_id = ? AND source = ? AND is_open
                AND (?::TIMESTAMP IS NULL OR trade_date <= CAST(? AS DATE)))
    ), entries AS (
        SELECT decision.trade_date AS decision_date, entry.trade_date AS entry_date,
               entry.session_number
        FROM calendar entry
        JOIN calendar decision ON decision.session_number = entry.session_number - 1
        WHERE date_trunc('month', decision.trade_date) <> date_trunc('month', entry.trade_date)
    )"""


def month_end_formations(
    store: DuckDBStore, *, cutoff: dt.datetime | None = None,
) -> list[tuple[dt.date, dt.date]]:
    """``(decision_date, entry_date)`` pairs of the monthly label calendar.

    ``decision_date`` is the last observed session of a closed month and
    ``entry_date`` the next observed session (the label's ``as_of_date``). Feature
    panels formed at ``decision_date`` join monthly labels on ``entry_date``.
    """
    cutoff = _naive_utc(cutoff)
    calendar_id, calendar_source = _calendar_keys()
    rows = store.con.execute(
        f"WITH {_ENTRY_SESSIONS_SQL} SELECT decision_date, entry_date FROM entries ORDER BY entry_date",
        [calendar_id, calendar_source, cutoff, cutoff],
    ).fetchall()
    return [(row[0], row[1]) for row in rows]


def monthly_label_diagnostics(
    store: DuckDBStore,
    *,
    source: str = MONTHLY_LABEL_SOURCE,
    horizons: Sequence[int] = MONTHLY_HORIZON_SESSIONS,
    price_basis: str = "adjusted_close",
    cutoff: dt.datetime | None = None,
) -> dict[str, Any]:
    """Bounded per-horizon counts: published rows, terminal provenance and attrition.

    ``anchors_matured``: entry-session anchors with a selected positive price (the
    publisher's bar pick, restricted to entry dates) whose ``h``-th session exists.
    ``unlabeled_matured`` of those have no published label; ``..._spanning_event``
    is the subset whose window ``(E, E+h]`` contains a visible ``delisting_events``
    cessation - survivorship attrition from a missing terminal, reported not imputed.
    Only aggregates leave DuckDB.
    """
    if price_basis not in {"adjusted_close", "close"}:
        raise ValueError("price_basis must be adjusted_close or close")
    if not horizons or any(int(h) != h or h < 1 for h in horizons):
        raise ValueError("horizons must be positive session counts")
    cutoff = _naive_utc(cutoff)
    calendar_id, calendar_source = _calendar_keys()
    found = store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'delisting_events' AND NOT temporary"
    ).fetchone()
    has_events = bool(found and found[0])
    event_filter = """EXISTS (
                SELECT 1 FROM delisting_events ev
                WHERE ev.security_id = a.security_id
                  AND ev.delist_date > a.entry_date AND ev.delist_date <= ending.trade_date
                  AND (?::TIMESTAMP IS NULL OR ev.available_at <= ?))""" if has_events else "false"
    params: list[Any] = [calendar_id, calendar_source, cutoff, cutoff, cutoff, cutoff,
                         [int(h) for h in horizons], source, cutoff, cutoff]
    if has_events:
        params += [cutoff, cutoff]
    rows = store.con.execute(f"""
        WITH {_ENTRY_SESSIONS_SQL}, eligible AS (
            SELECT b.*, coalesce(b.available_at,
                CAST(b.trade_date AS TIMESTAMP) + INTERVAL '22 hours') AS price_available_at
            FROM equity_daily_bars b JOIN entries en ON en.entry_date = b.trade_date
            WHERE (?::TIMESTAMP IS NULL OR coalesce(b.available_at,
                CAST(b.trade_date AS TIMESTAMP) + INTERVAL '22 hours') <= ?)
        ), chosen AS (
            SELECT security_id, trade_date, {price_basis} AS price,
                   row_number() OVER (
                       PARTITION BY security_id, trade_date
                       ORDER BY price_available_at DESC, source ASC,
                                vendor_security_id ASC NULLS LAST, symbol ASC,
                                adjusted_close DESC NULLS LAST, close DESC NULLS LAST,
                                source_loaded_at DESC
                   ) AS pick
            FROM eligible
        ), anchors AS (
            SELECT c.security_id, en.entry_date, en.session_number
            FROM chosen c JOIN entries en ON en.entry_date = c.trade_date
            WHERE c.pick = 1 AND c.price > 0 AND isfinite(c.price)
        ), horizons AS (SELECT unnest(?::INTEGER[]) AS horizon_days
        ), labels AS (
            SELECT * FROM forward_returns_survivorship_safe
            WHERE source = ? AND (?::TIMESTAMP IS NULL OR available_at <= ?)
        ), attrition AS (
            SELECT h.horizon_days,
                   count(*) FILTER (WHERE ending.trade_date IS NOT NULL) AS anchors_matured,
                   count(*) FILTER (WHERE ending.trade_date IS NOT NULL
                                    AND l.forward_return_id IS NULL) AS unlabeled_matured,
                   count(*) FILTER (WHERE ending.trade_date IS NOT NULL
                                    AND l.forward_return_id IS NULL AND {event_filter})
                       AS unlabeled_matured_spanning_event
            FROM anchors a CROSS JOIN horizons h
            LEFT JOIN calendar ending ON ending.session_number = a.session_number + h.horizon_days
            LEFT JOIN labels l ON l.security_id = a.security_id AND l.as_of_date = a.entry_date
                              AND l.horizon_days = h.horizon_days
            GROUP BY h.horizon_days
        ), published AS (
            SELECT horizon_days, count(*) AS rows, count(DISTINCT as_of_date) AS formations,
                   count(DISTINCT security_id) AS securities,
                   count(*) FILTER (WHERE is_stitched AND terminal_return_source = 'observed') AS observed_terminals,
                   count(*) FILTER (WHERE is_stitched AND terminal_return_source = 'policy') AS policy_terminals,
                   count(*) FILTER (WHERE is_stitched AND terminal_return_source IS DISTINCT FROM 'observed'
                                    AND terminal_return_source IS DISTINCT FROM 'policy') AS other_terminals,
                   count(*) FILTER (WHERE calculation_version IS DISTINCT FROM ?
                                    OR price_basis IS DISTINCT FROM ?) AS off_contract_rows,
                   min(as_of_date) AS first_formation, max(as_of_date) AS last_formation
            FROM labels GROUP BY horizon_days
        )
        SELECT h.horizon_days, coalesce(p.rows, 0), coalesce(p.formations, 0),
               coalesce(p.securities, 0), coalesce(p.observed_terminals, 0),
               coalesce(p.policy_terminals, 0), coalesce(p.other_terminals, 0),
               coalesce(p.off_contract_rows, 0), p.first_formation, p.last_formation,
               coalesce(a.anchors_matured, 0), coalesce(a.unlabeled_matured, 0),
               coalesce(a.unlabeled_matured_spanning_event, 0)
        FROM horizons h
        LEFT JOIN published p USING (horizon_days)
        LEFT JOIN attrition a USING (horizon_days)
        ORDER BY h.horizon_days
    """, [*params, LABEL_CALCULATION_VERSION, price_basis]).fetchall()
    fields = ("horizon_days", "rows", "formations", "securities", "observed_terminals",
              "policy_terminals", "other_terminals", "off_contract_rows", "first_formation",
              "last_formation", "anchors_matured", "unlabeled_matured",
              "unlabeled_matured_spanning_event")
    by_horizon = []
    for row in rows:
        record = dict(zip(fields, row, strict=True))
        for key in ("first_formation", "last_formation"):
            record[key] = None if record[key] is None else record[key].isoformat()
        record["formation_units"] = HORIZON_FORMATION_UNITS.get(record["horizon_days"])
        matured = record["anchors_matured"]
        record["label_coverage"] = (matured - record["unlabeled_matured"]) / matured if matured else None
        by_horizon.append(record)
    return {"source": source, "price_basis": price_basis,
            "cutoff": None if cutoff is None else cutoff.isoformat(),
            "formation_rule": MONTHLY_FORMATION_RULE, "horizons": by_horizon}
