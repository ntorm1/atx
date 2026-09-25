"""Public delisting evidence anchored to each security's own observed cessation.

A delisting is a fact about one trading security: its price series stops. This module
anchors every delisting event to that observed cessation -- the security's last bar in the
ticker-history archive -- and treats public notices as corroboration, never as events on
their own:

* SEC Form 25 / 25-NSE and Form 15 are *issuer-level* documents: the filer's CIK owns one
  mapped security here, while the notice may concern notes, preferred shares or another class.
* Nasdaq Trader delete actions are *trading-system* actions: a venue deletion is not proof
  that the mapped security stopped trading everywhere.

A notice explains a cessation only when that security's own last bar lies in a declared
session window around the notice (``[notice - before, notice + after]`` observed sessions)
and precedes the archive end. Every other notice stays in ``delisting_evidence`` with
``details_json.disposition`` in {``issuer_notice_security_continues_trading``,
``notice_without_price_series``} and never reaches ``delisting_events``.

One event per cessation cluster per security. Its ``delist_date`` is the first observed
session after the last observed trade (``details_json.effective_date_basis =
'last_observed_trade'``); filing / acceptance / file clocks are retained on the evidence rows
and listed in the event's ``details_json.evidence``. Reason attribution uses only the
corroborating cluster, in fixed precedence: bankruptcy overlay > merger (a same-CIK
*target-side* merger form -- DEFM14A, SC 14D9, SC TO-T, 425 -- filed within
[last trade - 365 days, last trade + 30 days]; an S-4/S-4/A never qualifies on its own) >
25-NSE exchange delist > Form 15 voluntary > unknown. A bare Form 25 or a Nasdaq delete
explains *that* the security left, not *why*: both stay ``unknown``.

Clocks. Every timestamp is sourced: SEC acceptance datetimes, Nasdaq publication times, or
``trade_date`` + 22 hours (the archive's own end-of-day convention); nothing reads a wall
clock. An evidence row's ``available_at`` covers everything that row asserts, and so does an
event row. Its *existence* clock is the earliest moment the cessation qualified (the
gap-establishing session, or a corroborating notice together with the first absent session),
recorded as ``details_json.existence_available_at``; reason evidence never moves it. The row
asserts the latest visible reason, so its ``available_at`` is the existence clock or -- when
that reason became public later (a reason revision, e.g. a Form 15 filed days after the
cessation) -- the reason's clock. A reason is never visible before it was public; the delay
is bounded by the merger look-ahead (30 days) and the notice window (10 sessions).
``reason_at_existence`` records what was known at the existence clock.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from uuid import uuid4

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .delisting import seed_delist_code_dim
from .warehouse import quality_check

DELISTING_EVIDENCE_SOURCE = "atx_delisting_evidence_v1"
DELISTING_EVENT_SOURCE = "atx_delisting_public_evidence_v1"
ARCHIVE_GAP_SESSIONS = 30
# Merger window around the security's last observed trade, in calendar days: target-side
# merger forms precede closing, so the window looks back a year and forward only far enough
# to catch closing-day filings. Post-cessation M&A by a still-filing CIK (acquirer deals,
# shell reverse mergers, sales out of distress) is not evidence that a merger ended trading.
MERGER_LOOKBACK_DAYS = 365
MERGER_LOOKAHEAD_DAYS = 30
# Declared corroboration window, in observed archive sessions, around a notice: the
# security's own last bar must lie in [notice - BEFORE, notice + AFTER] sessions.
NOTICE_WINDOW_SESSIONS_BEFORE = 10
NOTICE_WINDOW_SESSIONS_AFTER = 30
END_OF_DAY_HOURS = 22

EFFECTIVE_DATE_BASIS = "last_observed_trade"
# Securities are the warehouse's mapped IDs (current-ticker price-line mapping), not
# authenticated historical share-class identities.
IDENTITY_BASIS = "current_ticker_unverified"

DISPOSITION_ARCHIVE_CESSATION = "archive_cessation"
DISPOSITION_CORROBORATED = "corroborated_cessation"
DISPOSITION_CONTINUES_TRADING = "issuer_notice_security_continues_trading"
DISPOSITION_WITHOUT_PRICE_SERIES = "notice_without_price_series"
# Only these dispositions may form delisting_events.
EVENT_DISPOSITIONS: tuple[str, ...] = (DISPOSITION_ARCHIVE_CESSATION, DISPOSITION_CORROBORATED)
NOTICE_DISPOSITIONS: tuple[str, ...] = (
    DISPOSITION_CORROBORATED,
    DISPOSITION_CONTINUES_TRADING,
    DISPOSITION_WITHOUT_PRICE_SERIES,
)

REASON_CATEGORIES: tuple[str, ...] = (
    "bankruptcy",
    "exchange_delist",
    "merger_acquisition",
    "unknown",
    "voluntary",
)
# Event-level attribution: the first reason present in the corroborating cluster wins.
EVENT_REASON_PRECEDENCE: tuple[str, ...] = (
    "bankruptcy",
    "merger_acquisition",
    "exchange_delist",
    "voluntary",
    "unknown",
)
_REASON_CONFIDENCE = {
    "bankruptcy": "high",
    "merger_acquisition": "medium",
    "exchange_delist": "high",
    "voluntary": "medium",
    "unknown": "low",
}

# Target-side merger forms establish a merger on their own. S-4/S-4/A are also filed by
# acquirers, 144A exchange offers, distressed debt exchanges and holdco reorganizations, so
# they only count alongside a target-side form in the window -- which then qualifies by
# itself; S-4s are therefore recorded as supporting evidence and never change the outcome.
TARGET_SIDE_MERGER_FORMS: tuple[str, ...] = ("425", "DEFM14A", "SC 14D9", "SC TO-T")
SUPPORTING_MERGER_FORMS: tuple[str, ...] = ("S-4", "S-4/A")
MERGER_FORMS: tuple[str, ...] = (*TARGET_SIDE_MERGER_FORMS, *SUPPORTING_MERGER_FORMS)

# Nasdaq Trader action spellings (A1 normalizes to add/delete; legacy files carry A/D).
NASDAQ_DELETE_ACTIONS: tuple[str, ...] = ("D", "DELETE")
NASDAQ_ADD_ACTIONS: tuple[str, ...] = ("A", "ADD")

# (evidence_kind, evidence_rank, delist_code, default_reason_category, reason_confidence)
# evidence_rank orders the *primary* evidence row inside one cluster; the event's reason
# comes from EVENT_REASON_PRECEDENCE over the whole corroborating cluster.
EVIDENCE_PRECEDENCE: tuple[tuple[str, int, str, str, str], ...] = (
    ("sec_form_25", 1, "SEC_FORM_25", "exchange_delist", "high"),
    ("nasdaq_delete", 2, "NASDAQ_DELETE", "unknown", "low"),
    ("sec_form_15", 3, "SEC_FORM_15", "voluntary", "medium"),
    ("archive_last_trade", 4, "ARCHIVE_LAST_TRADE", "unknown", "low"),
)

EVIDENCE_COLUMNS: tuple[str, ...] = (
    "evidence_id",
    "source",
    "security_id",
    "symbol",
    "evidence_kind",
    "evidence_rank",
    "delist_date",
    "reason_category",
    "reason_confidence",
    "delist_code",
    "evidence_source_table",
    "source_event_id",
    "as_of_date",
    "available_at",
    "details_json",
    "run_id",
)


@dataclass(frozen=True)
class DelistingEvidenceOptions:
    """Rebuild a source using inputs known by ``as_of_date`` at 22:00, if supplied."""

    source: str = DELISTING_EVIDENCE_SOURCE
    event_source: str = DELISTING_EVENT_SOURCE
    archive_gap_sessions: int = ARCHIVE_GAP_SESSIONS
    merger_lookback_days: int = MERGER_LOOKBACK_DAYS
    include_archive_inference: bool = True
    as_of_date: dt.date | None = None
    run_id: str | None = None
    notice_window_sessions_before: int = NOTICE_WINDOW_SESSIONS_BEFORE
    notice_window_sessions_after: int = NOTICE_WINDOW_SESSIONS_AFTER
    merger_lookahead_days: int = MERGER_LOOKAHEAD_DAYS


@dataclass(frozen=True)
class _Counts:
    gap: int
    merger_lookback: int
    merger_lookahead: int
    before: int
    after: int


def _validated_counts(options: DelistingEvidenceOptions) -> _Counts:
    values = (
        options.archive_gap_sessions,
        options.merger_lookback_days,
        options.merger_lookahead_days,
        options.notice_window_sessions_before,
        options.notice_window_sessions_after,
    )
    if any(isinstance(value, bool) or not isinstance(value, int) or value < 0 for value in values):
        raise ValueError(
            "archive_gap_sessions, merger window days and notice window sessions must be non-negative integers"
        )
    return _Counts(*values)


def _evidence_id_expression(kind_literal: str) -> str:
    """Deterministic content hash scoped by the bound source parameter.

    The source event (accession / Nasdaq event id) is part of the key: two notices of one
    kind on one day for one mapped security (e.g. 25-NSE for notes and for preferred) are
    distinct evidence and must not overwrite each other.
    """

    return (
        f"sha256(concat_ws('|', ?, '{kind_literal}', coalesce(security_id, ''), symbol, "
        "CAST(delist_date AS VARCHAR), coalesce(source_event_id, '')))"
    )


def _cutoff(as_of_date: dt.date) -> dt.datetime:
    return dt.datetime.combine(as_of_date, dt.time(END_OF_DAY_HOURS))


def _as_of_filter(date_sql: str, available_sql: str, as_of_date: dt.date | None) -> str:
    """Render a typed date cutoff without changing the builders' bound parameters."""

    if as_of_date is None:
        return "true"
    return (
        f"({date_sql}) <= DATE '{as_of_date.isoformat()}' "
        f"AND ({available_sql}) <= TIMESTAMP '{_cutoff(as_of_date).isoformat(sep=' ')}'"
    )


def _date_filter(date_sql: str, as_of_date: dt.date | None) -> str:
    return "true" if as_of_date is None else f"({date_sql}) <= DATE '{as_of_date.isoformat()}'"


def _clock_filter(available_sql: str, as_of_date: dt.date | None) -> str:
    if as_of_date is None:
        return "true"
    return f"({available_sql}) <= TIMESTAMP '{_cutoff(as_of_date).isoformat(sep=' ')}'"


def _bar_available_sql(alias: str = "b") -> str:
    return f"coalesce({alias}.available_at, CAST({alias}.trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR)"


def _sec_date_sql(alias: str) -> str:
    return f"coalesce({alias}.filing_date, CAST({alias}.acceptance_datetime AS DATE))"


def _sec_available_sql(alias: str) -> str:
    return f"coalesce({alias}.acceptance_datetime, CAST({alias}.filing_date AS TIMESTAMP) + INTERVAL 22 HOUR)"


def _forms_sql(forms: tuple[str, ...]) -> str:
    return ", ".join(f"'{form}'" for form in forms)


def _action_in(column: str, actions: tuple[str, ...]) -> str:
    values = ", ".join(f"'{action}'" for action in actions)
    return f"upper(trim(coalesce({column}, ''))) IN ({values})"


def build_sessions_sql(*, as_of_date: dt.date | None = None) -> str:
    """Observed archive sessions: every latest-revision bar date visible at the cutoff.

    A session is known once its first bar is visible (``min`` bar clock), matching what an
    as-of rebuild at that clock would observe. No placeholders.
    """

    available = _bar_available_sql("b")
    return f"""
        SELECT trade_date,
               row_number() OVER (ORDER BY trade_date) AS session_rank,
               min(input_available_at) AS available_at
        FROM (
            SELECT b.trade_date, {available} AS input_available_at
            FROM equity_daily_bars b
            WHERE b.is_latest_revision = true
              AND b.trade_date IS NOT NULL
              AND {_as_of_filter("b.trade_date", available, as_of_date)}
        ) eligible_bars
        GROUP BY trade_date
    """


def build_cessations_sql(*, sessions: str, as_of_date: dt.date | None = None) -> str:
    """One row per security with a visible price series: its last observed trade.

    ``delist_date`` is the first observed session after the last trade (NULL while the
    security still trades at the archive end). ``gap_session_available_at`` is the clock of
    the session that first makes the absence exceed ``archive_gap_sessions`` -- the absence is
    established then, not at the (much later) archive end. The bar scan is inlined (``NOT
    MATERIALIZED``) so the two aggregate passes never buffer the full bar history.

    CIK links come from the security's own SEC filings and CIK identifier rows visible at the
    cutoff. Merger evidence is the earliest-visible same-CIK *target-side* merger form filed in
    [last trade - lookback, last trade + lookahead] days; supporting S-4/S-4/A filings in the
    window are counted but never qualify alone.

    Placeholder order: ``[archive_gap_sessions, merger_lookback_days, merger_lookahead_days]``.
    """

    available = _bar_available_sql("b")
    merger_date = _sec_date_sql("m")
    merger_available = _sec_available_sql("m")
    target_forms = _forms_sql(TARGET_SIDE_MERGER_FORMS)
    return f"""
        WITH eligible_bars AS NOT MATERIALIZED (
            SELECT b.security_id, b.symbol, b.trade_date, {available} AS input_available_at
            FROM equity_daily_bars b
            WHERE b.is_latest_revision = true
              AND b.trade_date IS NOT NULL
              AND b.security_id IS NOT NULL
              AND b.close IS NOT NULL
              AND {_as_of_filter("b.trade_date", available, as_of_date)}
        ),
        last_bar AS (
            SELECT security_id,
                   max(trade_date) AS last_trade_date,
                   max(input_available_at) AS last_trade_available_at
            FROM eligible_bars
            GROUP BY security_id
        ),
        last_symbol AS (
            SELECT b.security_id,
                   any_value(b.symbol ORDER BY b.input_available_at DESC, b.symbol) AS symbol
            FROM eligible_bars b
            JOIN last_bar l ON l.security_id = b.security_id AND l.last_trade_date = b.trade_date
            GROUP BY b.security_id
        ),
        archive_end AS (
            SELECT max(session_rank) AS last_rank FROM {sessions}
        ),
        base AS (
            SELECT l.security_id, s.symbol, l.last_trade_date, l.last_trade_available_at,
                   ls.session_rank AS last_rank,
                   ae.last_rank AS archive_last_rank,
                   ae.last_rank - ls.session_rank AS gap_sessions,
                   nxt.trade_date AS delist_date,
                   nxt.available_at AS delist_session_available_at,
                   gap.available_at AS gap_session_available_at
            FROM last_bar l
            JOIN last_symbol s ON s.security_id = l.security_id
            JOIN {sessions} ls ON ls.trade_date = l.last_trade_date
            CROSS JOIN archive_end ae
            LEFT JOIN {sessions} nxt ON nxt.session_rank = ls.session_rank + 1
            LEFT JOIN {sessions} gap ON gap.session_rank = ls.session_rank + CAST(? AS BIGINT) + 1
        ),
        security_ciks AS (
            SELECT DISTINCT s.security_id, lpad(trim(s.cik), 10, '0') AS cik
            FROM sec_submissions s
            WHERE s.security_id IN (SELECT security_id FROM base)
              AND nullif(trim(s.cik), '') IS NOT NULL
              AND {_sec_date_sql("s")} IS NOT NULL
              AND {_as_of_filter(_sec_date_sql("s"), _sec_available_sql("s"), as_of_date)}
            UNION
            SELECT DISTINCT h.security_id, lpad(trim(h.id_value), 10, '0') AS cik
            FROM security_identifier_history h
            WHERE upper(h.id_type) = 'CIK'
              AND h.security_id IN (SELECT security_id FROM base)
              AND nullif(trim(h.id_value), '') IS NOT NULL
              AND {_as_of_filter("h.as_of_date", "coalesce(h.available_at, h.source_loaded_at)", as_of_date)}
        ),
        cik_summary AS (
            SELECT security_id, min(cik) AS cik, count(*) AS cik_count
            FROM security_ciks
            GROUP BY security_id
        ),
        merger_window AS (
            SELECT c.security_id, m.form, m.accession_number,
                   {merger_date} AS filing_date, {merger_available} AS available_at
            FROM base c
            JOIN security_ciks sc ON sc.security_id = c.security_id
            JOIN sec_submissions m
              ON lpad(trim(m.cik), 10, '0') = sc.cik
             AND m.form IN ({_forms_sql(MERGER_FORMS)})
             AND {merger_date} >= c.last_trade_date - CAST(? AS INTEGER)
             AND {merger_date} <= c.last_trade_date + CAST(? AS INTEGER)
             AND {_as_of_filter(merger_date, merger_available, as_of_date)}
            WHERE c.last_rank < c.archive_last_rank
        ),
        merger_hits AS (
            SELECT security_id,
                   min(struct_pack(
                       available_at := available_at,
                       accession_number := accession_number,
                       form := form,
                       filing_date := filing_date
                   )) FILTER (WHERE form IN ({target_forms})) AS evidence,
                   count(*) FILTER (WHERE form IN ({target_forms})) AS target_forms,
                   count(*) FILTER (WHERE form NOT IN ({target_forms})) AS supporting_forms
            FROM merger_window
            GROUP BY security_id
            HAVING count(*) FILTER (WHERE form IN ({target_forms})) > 0
        )
        SELECT base.*,
               cik_summary.cik,
               coalesce(cik_summary.cik_count, 0) AS cik_count,
               merger_hits.evidence.accession_number AS merger_accession,
               merger_hits.evidence.form AS merger_form,
               merger_hits.evidence.filing_date AS merger_filing_date,
               merger_hits.evidence.available_at AS merger_available_at,
               merger_hits.target_forms AS merger_target_forms,
               merger_hits.supporting_forms AS merger_supporting_forms
        FROM base
        LEFT JOIN cik_summary ON cik_summary.security_id = base.security_id
        LEFT JOIN merger_hits ON merger_hits.security_id = base.security_id
    """


def build_nasdaq_delete_sql(*, as_of_date: dt.date | None = None) -> str:
    """Nasdaq Trader delete actions (``D``/``Delete``, any case) on any venue column.

    A row that also *adds* the symbol on another venue is a venue move, not a removal, and
    is excluded. Output columns follow the notice contract (see :func:`_notice_insert_sql`).

    Availability is the row's receipt clock (``available_at``; A1 stamps pinned snapshots with
    the retained file's original receipt time). The file's ``File Creation Time`` precedes
    receipt, so it is never used as availability -- only as a floor, with the modeled
    ``as_of_date`` 22:00, when a legacy row carries no receipt clock at all.
    """

    available = (
        "coalesce(e.available_at, greatest(e.source_file_created_at, "
        "CAST(e.as_of_date AS TIMESTAMP) + INTERVAL 22 HOUR))"
    )
    columns = ("e.nasdaq_action", "e.bx_action", "e.psx_action")
    any_delete = " OR ".join(_action_in(column, NASDAQ_DELETE_ACTIONS) for column in columns)
    any_add = " OR ".join(_action_in(column, NASDAQ_ADD_ACTIONS) for column in columns)
    return f"""
        SELECT
            e.security_id,
            e.symbol,
            coalesce(e.effective_date, e.as_of_date) AS notice_date,
            {available} AS available_at,
            'nasdaq_listing_events' AS evidence_source_table,
            e.event_id AS source_event_id,
            'unknown' AS reason_category,
            'low' AS reason_confidence,
            CAST(NULL AS VARCHAR) AS cik,
            CAST(NULL AS VARCHAR) AS form
        FROM nasdaq_listing_events e
        WHERE coalesce(e.effective_date, e.as_of_date) IS NOT NULL
          AND e.is_latest_revision = true
          AND {_as_of_filter("coalesce(e.effective_date, e.as_of_date)", available, as_of_date)}
          AND ({any_delete})
          AND NOT ({any_add})
    """


def build_sec_form_sql(*, form_kind: str, as_of_date: dt.date | None = None) -> str:
    """SEC Form 25/25-NSE or Form 15 notices, in the notice contract. No placeholders.

    ``form_kind`` is ``"form_25"`` or ``"form_15"``. A notice carries only what it says:
    25-NSE -> exchange_delist; a bare 25 -> unknown (issuer-requested removal says that, not
    why); 15-* -> voluntary. Merger attribution is a fact about the *cessation* (see
    :func:`build_cessations_sql`), attached to corroborated notices and resolved at the event.
    """

    if form_kind not in {"form_25", "form_15"}:
        raise ValueError(f"unknown form_kind: {form_kind!r}")
    available = _sec_available_sql("s")
    date_sql = _sec_date_sql("s")
    if form_kind == "form_15":
        forms = "s.form LIKE '15-%'"
        reason = "'voluntary'"
        confidence = "'medium'"
    else:
        forms = "s.form IN ('25', '25-NSE')"
        reason = "CASE WHEN s.form = '25-NSE' THEN 'exchange_delist' ELSE 'unknown' END"
        confidence = "CASE WHEN s.form = '25-NSE' THEN 'high' ELSE 'low' END"
    return f"""
        SELECT
            s.security_id,
            coalesce(sec.primary_symbol, s.security_id) AS symbol,
            {date_sql} AS notice_date,
            {available} AS available_at,
            'sec_submissions' AS evidence_source_table,
            s.accession_number AS source_event_id,
            {reason} AS reason_category,
            {confidence} AS reason_confidence,
            lpad(trim(s.cik), 10, '0') AS cik,
            s.form
        FROM sec_submissions s
        LEFT JOIN securities sec ON sec.security_id = s.security_id
        WHERE {forms}
          AND {date_sql} IS NOT NULL
          AND {_as_of_filter(date_sql, available, as_of_date)}
    """


def build_bankruptcy_overlay_sql(*, as_of_date: dt.date | None = None) -> str:
    """Newest eligible snapshot per evidence row, used to assess bankruptcy.

    Nasdaq's ``financial_status`` flag ``Q`` means "bankrupt"; the composite codes (``EQ``,
    ``HQ``, ...) contain it, so a substring test is the right predicate. Cleared/null
    statuses remain in the result so their availability also constrains the reason.
    """

    available = "coalesce(d.available_at, CAST(d.as_of_date AS TIMESTAMP) + INTERVAL 22 HOUR)"
    return f"""
        WITH ranked AS (
            SELECT
                e.evidence_id,
                d.financial_status,
                greatest(e.available_at, {available}) AS available_at,
                row_number() OVER (
                    PARTITION BY e.evidence_id
                    ORDER BY d.as_of_date DESC, d.directory, {available} DESC,
                             d.financial_status NULLS LAST
                ) AS rn
            FROM delisting_evidence e
            JOIN nasdaq_symbol_directory d
              ON d.symbol = e.symbol
             AND d.as_of_date <= e.delist_date
             AND d.is_latest_revision = true
             AND {_as_of_filter("d.as_of_date", available, as_of_date)}
            WHERE e.source = ? AND e.is_latest_revision = true
        )
        SELECT evidence_id, available_at, contains(upper(financial_status), 'Q') AS is_bankrupt
        FROM ranked
        WHERE rn = 1
        ORDER BY evidence_id
    """


def _params_json_fields(options: DelistingEvidenceOptions) -> str:
    """Build parameters as literal JSON fields (validated integers / fixed vocabulary)."""

    counts = _validated_counts(options)
    as_of = f"DATE '{options.as_of_date.isoformat()}'" if options.as_of_date is not None else "NULL::DATE"
    return (
        f"'archive_gap_sessions', {counts.gap}, "
        f"'merger_window_days', [{-counts.merger_lookback}, {counts.merger_lookahead}], "
        f"'merger_window_anchor', 'last_observed_trade', "
        f"'target_side_merger_forms', [{_forms_sql(TARGET_SIDE_MERGER_FORMS)}], "
        f"'supporting_merger_forms', [{_forms_sql(SUPPORTING_MERGER_FORMS)}], "
        f"'notice_window_sessions', [{-counts.before}, {counts.after}], 'as_of_date', {as_of}, "
        f"'effective_date_basis', '{EFFECTIVE_DATE_BASIS}', 'identity_basis', '{IDENTITY_BASIS}'"
    )


def _merger_json(prefix: str) -> str:
    return (
        f"CASE WHEN {prefix}merger_accession IS NULL THEN NULL ELSE json_object("
        f"'accession_number', {prefix}merger_accession, 'form', {prefix}merger_form, "
        f"'filing_date', {prefix}merger_filing_date, 'available_at', {prefix}merger_available_at, "
        f"'target_side_forms_in_window', {prefix}merger_target_forms, "
        f"'supporting_forms_in_window', {prefix}merger_supporting_forms) END"
    )


def _notice_insert_sql(
    kind: str,
    rank: int,
    delist_code: str,
    *,
    body: str,
    sessions: str,
    cessations: str,
    options: DelistingEvidenceOptions,
) -> str:
    """Insert one notice stream, classifying each notice against the security's cessation.

    ``body`` yields ``security_id, symbol, notice_date, available_at, evidence_source_table,
    source_event_id, reason_category, reason_confidence, cik, form``. The notice's session
    rank is the number of observed sessions on or before its date; outside the archive's
    session range it is extrapolated at 5 sessions per 7 calendar days, so a 1990s Form 25
    can never corroborate a cessation early in a 2012-start archive. The notice corroborates
    only if the security's last bar lies in the declared window and precedes the archive
    end; the evidence row keeps the notice's own date and clocks either way. A corroborating
    notice establishes existence at max(notice clock, first-absent-session clock) and carries
    the cessation's merger evidence for event-level attribution.
    """

    counts = _validated_counts(options)
    return f"""
        INSERT OR REPLACE INTO delisting_evidence (
            {", ".join(EVIDENCE_COLUMNS)}, source_loaded_at
        )
        WITH notices AS ({body}),
        bounds AS (
            SELECT min(trade_date) AS first_date, max(trade_date) AS last_date, max(session_rank) AS last_rank
            FROM {sessions}
        ),
        ranked AS (
            SELECT
                n.*,
                CASE
                    WHEN b.first_date IS NULL THEN NULL
                    WHEN n.notice_date < b.first_date
                        THEN 1 - CAST(ceil(datediff('day', n.notice_date, b.first_date) * 5.0 / 7) AS BIGINT)
                    WHEN n.notice_date > b.last_date
                        THEN b.last_rank + CAST(floor(datediff('day', b.last_date, n.notice_date) * 5.0 / 7) AS BIGINT)
                    ELSE s.session_rank
                END AS notice_rank,
                CASE
                    WHEN n.notice_date < b.first_date OR n.notice_date > b.last_date THEN 'extrapolated_weekdays'
                    ELSE 'observed_sessions'
                END AS notice_rank_basis
            FROM notices n
            ASOF LEFT JOIN {sessions} s ON n.notice_date >= s.trade_date
            CROSS JOIN bounds b
        ),
        classified AS (
            SELECT
                r.*,
                c.symbol AS last_observed_symbol,
                c.last_trade_date,
                c.last_rank,
                c.archive_last_rank,
                c.delist_date AS cessation_delist_date,
                greatest(c.last_trade_available_at, c.delist_session_available_at) AS cessation_available_at,
                c.merger_accession,
                c.merger_form,
                c.merger_filing_date,
                c.merger_available_at,
                c.merger_target_forms,
                c.merger_supporting_forms,
                CASE
                    WHEN c.security_id IS NULL THEN 'absent'
                    WHEN c.last_rank >= c.archive_last_rank THEN 'trading_at_archive_end'
                    WHEN c.last_rank > r.notice_rank + {counts.after} THEN 'traded_past_notice_window'
                    WHEN c.last_rank < r.notice_rank - {counts.before} THEN 'ended_before_notice_window'
                    ELSE 'ceased_in_notice_window'
                END AS price_series_status
            FROM ranked r
            LEFT JOIN {cessations} c ON c.security_id = r.security_id
        ),
        body AS (
            SELECT
                *,
                notice_date AS delist_date,
                price_series_status = 'ceased_in_notice_window' AS corroborates,
                CASE price_series_status
                    WHEN 'ceased_in_notice_window' THEN '{DISPOSITION_CORROBORATED}'
                    WHEN 'absent' THEN '{DISPOSITION_WITHOUT_PRICE_SERIES}'
                    WHEN 'ended_before_notice_window' THEN '{DISPOSITION_WITHOUT_PRICE_SERIES}'
                    ELSE '{DISPOSITION_CONTINUES_TRADING}'
                END AS disposition
            FROM classified
        )
        SELECT
            {_evidence_id_expression(kind)},
            ? AS source,
            body.security_id,
            body.symbol,
            '{kind}' AS evidence_kind,
            {rank} AS evidence_rank,
            body.delist_date,
            body.reason_category,
            body.reason_confidence,
            '{delist_code}' AS delist_code,
            body.evidence_source_table,
            body.source_event_id,
            body.notice_date AS as_of_date,
            body.available_at,
            json_object(
                {_params_json_fields(options)},
                'disposition', body.disposition,
                'price_series_status', body.price_series_status,
                'notice_date', body.notice_date,
                'notice_available_at', body.available_at,
                'notice_session_rank', body.notice_rank,
                'notice_session_rank_basis', body.notice_rank_basis,
                'form', body.form,
                'cik', body.cik,
                'last_observed_trade_date', body.last_trade_date,
                'last_observed_symbol', body.last_observed_symbol,
                'last_trade_session_rank', body.last_rank,
                'archive_last_session_rank', body.archive_last_rank,
                'gap_qualified', false,
                'cessation_delist_date', CASE WHEN body.corroborates THEN body.cessation_delist_date END,
                'cessation_available_at', CASE WHEN body.corroborates THEN body.cessation_available_at END,
                'existence_available_at',
                    CASE WHEN body.corroborates THEN greatest(body.available_at, body.cessation_available_at) END,
                'merger_evidence', CASE WHEN body.corroborates THEN {_merger_json("body.")} END
            )::VARCHAR AS details_json,
            ? AS run_id,
            now()
        FROM body
        WHERE body.symbol IS NOT NULL AND body.notice_date IS NOT NULL
        ORDER BY body.security_id, body.notice_date, body.source_event_id
    """


def _archive_insert_sql(*, cessations: str, options: DelistingEvidenceOptions) -> str:
    """The cessation base row: the security's last observed trade, emitted when the absence
    exceeds the gap threshold or when a notice corroborates it.

    ``delist_date`` is the first observed session after the last trade. The row's
    ``available_at`` covers what it asserts: the last-bar clock, the session establishing the
    absence (the gap-qualifying session, or for a notice-only cessation the delist session),
    and the merger form when it supplies the reason. ``existence_available_at`` (gap-qualified
    rows only) excludes the merger clock: reason evidence never delays existence.
    Placeholders: ``[source, source, source, run_id]``.
    """

    counts = _validated_counts(options)
    kind, rank, code, _reason, _confidence = next(row for row in EVIDENCE_PRECEDENCE if row[0] == "archive_last_trade")
    gap_qualified = f"(c.gap_sessions > {counts.gap})"
    return f"""
        INSERT OR REPLACE INTO delisting_evidence (
            {", ".join(EVIDENCE_COLUMNS)}, source_loaded_at
        )
        WITH body AS (
            SELECT
                c.*,
                {gap_qualified} AS gap_qualified,
                'equity_daily_bars' AS evidence_source_table,
                CAST(NULL AS VARCHAR) AS source_event_id,
                greatest(c.last_trade_available_at, c.delist_session_available_at) AS cessation_available_at
            FROM {cessations} c
            WHERE c.last_rank < c.archive_last_rank
              AND c.delist_date IS NOT NULL
              AND (
                {gap_qualified}
                OR EXISTS (
                    SELECT 1 FROM delisting_evidence n
                    WHERE n.source = ?
                      AND n.security_id = c.security_id
                      AND json_extract_string(n.details_json, '$.disposition') = '{DISPOSITION_CORROBORATED}'
                )
              )
        )
        SELECT
            {_evidence_id_expression(kind)},
            ? AS source,
            body.security_id,
            body.symbol,
            '{kind}' AS evidence_kind,
            {rank} AS evidence_rank,
            body.delist_date,
            CASE WHEN body.merger_accession IS NOT NULL THEN 'merger_acquisition' ELSE 'unknown' END,
            CASE WHEN body.merger_accession IS NOT NULL THEN 'medium' ELSE 'low' END,
            '{code}' AS delist_code,
            body.evidence_source_table,
            body.source_event_id,
            body.delist_date AS as_of_date,
            greatest(
                body.last_trade_available_at,
                CASE WHEN body.gap_qualified THEN body.gap_session_available_at
                     ELSE body.delist_session_available_at END,
                body.merger_available_at
            ) AS available_at,
            json_object(
                {_params_json_fields(options)},
                'disposition', '{DISPOSITION_ARCHIVE_CESSATION}',
                'gap_qualified', body.gap_qualified,
                'gap_sessions', body.gap_sessions,
                'cik', body.cik,
                'cik_count', body.cik_count,
                'merger_evidence', {_merger_json("body.")},
                'last_observed_trade_date', body.last_trade_date,
                'last_observed_trade_available_at', body.last_trade_available_at,
                'last_observed_symbol', body.symbol,
                'last_trade_session_rank', body.last_rank,
                'archive_last_session_rank', body.archive_last_rank,
                'cessation_delist_date', body.delist_date,
                'cessation_available_at', body.cessation_available_at,
                'existence_available_at',
                    CASE WHEN body.gap_qualified
                         THEN greatest(body.last_trade_available_at, body.gap_session_available_at) END
            )::VARCHAR AS details_json,
            ? AS run_id,
            now()
        FROM body
        WHERE body.symbol IS NOT NULL
        ORDER BY body.security_id
    """


def refresh_delisting_evidence(
    store: DuckDBStore,
    options: DelistingEvidenceOptions | None = None,
) -> int:
    """Rebuild this source from latest revisions visible at the optional cutoff.

    Every notice is retained with its disposition; only corroborated notices and archive
    cessations are eligible to form events (see :func:`fold_evidence_into_delisting_events`).
    """

    options = options or DelistingEvidenceOptions()
    counts = _validated_counts(options)
    store.initialize()
    seed_delist_code_dim(store)
    by_kind = {row[0]: row for row in EVIDENCE_PRECEDENCE}
    token = uuid4().hex
    sessions = f"_delisting_sessions_{token}"
    cessations = f"_delisting_cessations_{token}"

    try:
        with store.transaction():
            store.con.execute("DELETE FROM delisting_evidence WHERE source = ?", [options.source])
            store.con.execute(f"CREATE TEMP TABLE {sessions} AS {build_sessions_sql(as_of_date=options.as_of_date)}")
            store.con.execute(
                f"CREATE TEMP TABLE {cessations} AS "
                f"{build_cessations_sql(sessions=sessions, as_of_date=options.as_of_date)}",
                [counts.gap, counts.merger_lookback, counts.merger_lookahead],
            )

            streams = (
                ("sec_form_25", build_sec_form_sql(form_kind="form_25", as_of_date=options.as_of_date)),
                ("nasdaq_delete", build_nasdaq_delete_sql(as_of_date=options.as_of_date)),
                ("sec_form_15", build_sec_form_sql(form_kind="form_15", as_of_date=options.as_of_date)),
            )
            for kind, body in streams:
                _kind, rank, code, _reason, _confidence = by_kind[kind]
                store.con.execute(
                    _notice_insert_sql(
                        kind, rank, code, body=body, sessions=sessions, cessations=cessations, options=options
                    ),
                    [options.source, options.source, options.run_id],
                )

            if options.include_archive_inference:
                store.con.execute(
                    _archive_insert_sql(cessations=cessations, options=options),
                    [options.source, options.source, options.source, options.run_id],
                )

            store.con.execute(
                f"""
                UPDATE delisting_evidence AS e
                SET reason_category = CASE WHEN overlay.is_bankrupt THEN 'bankruptcy' ELSE e.reason_category END,
                    reason_confidence = CASE WHEN overlay.is_bankrupt THEN 'high' ELSE e.reason_confidence END,
                    available_at = overlay.available_at
                FROM ({build_bankruptcy_overlay_sql(as_of_date=options.as_of_date)}) overlay
                WHERE e.evidence_id = overlay.evidence_id
                """,
                [options.source],
            )

            count_row = store.con.execute(
                "SELECT count(*) FROM delisting_evidence WHERE source = ?", [options.source]
            ).fetchone()
            assert count_row is not None
            rows = int(count_row[0])
    finally:
        for table in (cessations, sessions):
            store.con.execute(f"DROP TABLE IF EXISTS {table}")

    by_reason = {
        str(row[0]): int(row[1])
        for row in store.con.execute(
            "SELECT reason_category, count(*) FROM delisting_evidence WHERE source = ? "
            "GROUP BY reason_category ORDER BY reason_category",
            [options.source],
        ).fetchall()
    }
    by_disposition = {
        f"{row[0]}:{row[1]}": int(row[2])
        for row in store.con.execute(
            "SELECT evidence_kind, json_extract_string(details_json, '$.disposition'), count(*) "
            "FROM delisting_evidence WHERE source = ? GROUP BY 1, 2 ORDER BY 1, 2",
            [options.source],
        ).fetchall()
    }
    merger_forms = {
        str(row[0]): int(row[1])
        for row in store.con.execute(
            "SELECT json_extract_string(details_json, '$.merger_evidence.form'), count(*) "
            "FROM delisting_evidence WHERE source = ? AND evidence_kind = 'archive_last_trade' "
            "AND json_extract_string(details_json, '$.merger_evidence.form') IS NOT NULL GROUP BY 1 ORDER BY 1",
            [options.source],
        ).fetchall()
    }
    quality_check(
        store,
        dataset_id="delisting_evidence",
        table_name="delisting_evidence",
        check_name="rows_loaded",
        status="passed" if rows > 0 else "warning",
        observed_value=float(rows),
        threshold_value=1.0,
        details={
            "source": options.source,
            "by_reason_category": by_reason,
            "by_kind_disposition": by_disposition,
            "merger_attribution_first_form": merger_forms,
            "notice_window_sessions": [-counts.before, counts.after],
            "merger_window_days": [-counts.merger_lookback, counts.merger_lookahead],
        },
    )
    return rows


def _reason_rank_sql(column: str) -> str:
    cases = " ".join(f"WHEN '{reason}' THEN {rank}" for rank, reason in enumerate(EVENT_REASON_PRECEDENCE, start=1))
    return f"CASE {column} {cases} ELSE {len(EVENT_REASON_PRECEDENCE)} END"


def _reason_text_sql(rank_sql: str) -> str:
    cases = " ".join(f"WHEN {rank} THEN '{reason}'" for rank, reason in enumerate(EVENT_REASON_PRECEDENCE, start=1))
    return f"CASE {rank_sql} {cases} END"


def _reason_confidence_sql(rank_sql: str) -> str:
    cases = " ".join(
        f"WHEN {rank} THEN '{_REASON_CONFIDENCE[reason]}'"
        for rank, reason in enumerate(EVENT_REASON_PRECEDENCE, start=1)
    )
    return f"CASE {rank_sql} {cases} END"


def fold_evidence_into_delisting_events(
    store: DuckDBStore,
    options: DelistingEvidenceOptions | None = None,
) -> int:
    """Materialize one ``delisting_events`` row per security cessation cluster.

    A cluster is the security's visible cessation evidence: its archive cessation row and
    every corroborated notice. It becomes an event only once it *exists*: the absence passed
    the gap threshold, or a corroborated notice is visible together with the first absent
    session. Uncorroborated notices never form events.

    * ``delist_date``: the first session after the last observed trade.
    * existence clock (``details_json.existence_available_at``): the earliest qualifying
      member's clock (gap-establishing session, or max(notice, first absent session)); reason
      evidence and additional notices never move it.
    * ``delist_reason``: the first of :data:`EVENT_REASON_PRECEDENCE` among visible members
      and the cessation's visible merger evidence.
    * ``available_at``: max(existence clock, clock at which that reason became public). A
      reason first public after existence is a *revision* (``reason_revised_after_existence``,
      ``reason_at_existence``): this single-row table then becomes visible only once the
      revised reason was public, so no consumer filtering on ``available_at`` sees a reason
      -- and the terminal-policy decision it drives -- before it existed. A revision chain
      would publish the pre-revision row from the existence clock (A3 report follow-up 7).
    * Primary row (``source_listing_status_id`` / ``delist_code`` / ``source_event_id``): the
      best-reason member with the best ``evidence_rank``.

    Rows written by ``delisting.refresh_delisting_events`` are never touched: the DELETE is
    scoped to ``options.event_source``. The optional as-of bound also applies when folding
    already-materialized evidence without refreshing it; an exact point-in-time rebuild
    refreshes the evidence at the same cutoff first (activation does).
    """

    options = options or DelistingEvidenceOptions()
    store.initialize()
    as_of = options.as_of_date
    as_of_text = as_of.isoformat() if as_of is not None else None
    event_dispositions = ", ".join(f"'{value}'" for value in EVENT_DISPOSITIONS)
    precedence = ", ".join(f"'{reason}'" for reason in EVENT_REASON_PRECEDENCE)
    unknown_rank = len(EVENT_REASON_PRECEDENCE)
    merger_rank = EVENT_REASON_PRECEDENCE.index("merger_acquisition") + 1
    rank_at_existence = (
        "CASE "
        + " ".join(f"WHEN c.reason_{rank}_at <= c.existence_at THEN {rank}" for rank in range(1, unknown_rank))
        + f" ELSE {unknown_rank} END"
    )
    reason_available_at = (
        "greatest(c.existence_at, CASE c.reason_rank "
        + " ".join(f"WHEN {rank} THEN c.reason_{rank}_at" for rank in range(1, unknown_rank))
        + " ELSE c.existence_at END)"
    )
    per_rank_clocks = ",\n".join(
        f"min(available_at) FILTER (WHERE row_visible AND member_rank = {rank}) AS member_reason_{rank}_at"
        for rank in range(1, unknown_rank)
    )
    reason_clock_columns = ",\n".join(
        (
            f"least(member_reason_{rank}_at, merger_at) AS reason_{rank}_at"
            if rank == merger_rank
            else f"member_reason_{rank}_at AS reason_{rank}_at"
        )
        for rank in range(1, unknown_rank)
    )
    with store.transaction():
        store.con.execute("DELETE FROM delisting_events WHERE source = ?", [options.event_source])
        store.con.execute(
            f"""
            INSERT OR REPLACE INTO delisting_events (
                delisting_event_id, source, listing_status_source, source_listing_status_id,
                security_id, symbol, delist_date, as_of_date, available_at, delist_code,
                delist_reason, delisting_return, delisting_return_type, is_return_imputed,
                return_policy, return_confidence, evidence_source, evidence_source_table,
                source_event_id, method, evidence_confidence, inferred_from_absence,
                details_json, run_id
            )
            WITH members AS (
                SELECT
                    e.*,
                    json_extract_string(e.details_json, '$.disposition') AS disposition,
                    TRY_CAST(json_extract_string(e.details_json, '$.cessation_delist_date') AS DATE)
                        AS cessation_delist_date,
                    TRY_CAST(json_extract_string(e.details_json, '$.cessation_available_at') AS TIMESTAMP)
                        AS cessation_available_at,
                    TRY_CAST(json_extract_string(e.details_json, '$.existence_available_at') AS TIMESTAMP)
                        AS existence_clock,
                    TRY_CAST(json_extract_string(e.details_json, '$.merger_evidence.available_at') AS TIMESTAMP)
                        AS merger_clock,
                    json_extract_string(e.details_json, '$.merger_evidence') AS merger_evidence,
                    TRY_CAST(json_extract_string(e.details_json, '$.last_observed_trade_date') AS DATE)
                        AS last_observed_trade_date,
                    json_extract_string(e.details_json, '$.last_observed_symbol') AS last_observed_symbol,
                    coalesce(TRY_CAST(json_extract(e.details_json, '$.gap_qualified') AS BOOLEAN), false)
                        AS gap_qualified,
                    json_extract_string(e.details_json, '$.cik') AS cik
                FROM delisting_evidence e
                WHERE e.source = ? AND e.is_latest_revision = true
                  AND e.security_id IS NOT NULL
                  AND json_extract_string(e.details_json, '$.disposition') IN ({event_dispositions})
                  AND {_date_filter("e.delist_date", as_of)}
            ),
            flagged AS (
                SELECT
                    *,
                    existence_clock IS NOT NULL AND {_clock_filter("existence_clock", as_of)} AS existence_visible,
                    {_clock_filter("available_at", as_of)} AS row_visible,
                    merger_clock IS NOT NULL AND {_clock_filter("merger_clock", as_of)} AS merger_visible
                FROM members
                WHERE cessation_delist_date IS NOT NULL
                  AND cessation_available_at IS NOT NULL
                  AND {_as_of_filter("cessation_delist_date", "cessation_available_at", as_of)}
            ),
            visible AS (
                SELECT
                    *,
                    -- A member whose own row is not yet visible can still establish existence,
                    -- but it asserts no reason yet.
                    CASE WHEN row_visible THEN {_reason_rank_sql("reason_category")} ELSE {unknown_rank} END
                        AS member_rank
                FROM flagged
                WHERE existence_visible OR row_visible
            ),
            cluster_facts AS (
                SELECT
                    security_id,
                    cessation_delist_date AS delist_date,
                    min(existence_clock) FILTER (WHERE existence_visible) AS existence_at,
                    min(merger_clock) FILTER (WHERE merger_visible) AS merger_at,
                    min(merger_evidence) FILTER (WHERE merger_visible) AS merger_evidence,
                    least(min(member_rank), CASE WHEN bool_or(merger_visible) THEN {merger_rank} END) AS reason_rank,
                    {per_rank_clocks},
                    coalesce(bool_or(evidence_kind <> 'archive_last_trade' AND existence_visible), false)
                        AS notice_corroborated,
                    coalesce(bool_or(evidence_kind = 'archive_last_trade' AND gap_qualified AND existence_visible), false)
                        AS gap_qualified,
                    max(cessation_available_at) AS cessation_available_at,
                    max(last_observed_trade_date) AS last_observed_trade_date,
                    max(last_observed_symbol) AS last_observed_symbol,
                    min(cik) AS cik,
                    count(*) AS evidence_count,
                    list(struct_pack(
                        evidence_id := evidence_id,
                        evidence_kind := evidence_kind,
                        source_event_id := source_event_id,
                        evidence_date := delist_date,
                        available_at := available_at,
                        existence_available_at := existence_clock,
                        reason_category := reason_category,
                        disposition := disposition
                    ) ORDER BY evidence_rank, evidence_id) AS evidence
                FROM visible
                GROUP BY security_id, cessation_delist_date
                HAVING min(existence_clock) FILTER (WHERE existence_visible) IS NOT NULL
            ),
            clusters AS (
                SELECT *, {reason_clock_columns}
                FROM cluster_facts
            ),
            primary_members AS (
                SELECT
                    v.*,
                    row_number() OVER (
                        PARTITION BY v.security_id, v.cessation_delist_date
                        ORDER BY v.member_rank, v.evidence_rank, v.evidence_id
                    ) AS rn
                FROM visible v
            )
            SELECT
                sha256(concat_ws('|', ?, c.security_id, CAST(c.delist_date AS VARCHAR))),
                ? AS source,
                p.evidence_kind AS listing_status_source,
                p.evidence_id AS source_listing_status_id,
                c.security_id,
                coalesce(c.last_observed_symbol, p.symbol) AS symbol,
                c.delist_date,
                greatest(c.delist_date, CAST({reason_available_at} AS DATE)) AS as_of_date,
                {reason_available_at} AS available_at,
                p.delist_code,
                {_reason_text_sql("c.reason_rank")} AS delist_reason,
                CAST(NULL AS DOUBLE) AS delisting_return,
                'UNOBSERVED' AS delisting_return_type,
                false AS is_return_imputed,
                'none' AS return_policy,
                'none' AS return_confidence,
                'public_evidence' AS evidence_source,
                p.evidence_source_table,
                p.source_event_id,
                'public_evidence_cessation_cluster' AS method,
                CASE WHEN c.notice_corroborated THEN 'high' ELSE 'low' END AS evidence_confidence,
                NOT c.notice_corroborated AS inferred_from_absence,
                json_object(
                    'effective_date_basis', '{EFFECTIVE_DATE_BASIS}',
                    'identity_basis', '{IDENTITY_BASIS}',
                    'last_observed_trade_date', c.last_observed_trade_date,
                    'last_observed_symbol', c.last_observed_symbol,
                    'cessation_available_at', c.cessation_available_at,
                    'existence_available_at', c.existence_at,
                    'gap_qualified', c.gap_qualified,
                    'notice_corroborated', c.notice_corroborated,
                    'reason_basis', CASE WHEN p.member_rank = c.reason_rank THEN p.evidence_kind
                                         ELSE 'cessation_merger_evidence' END,
                    'reason_confidence', {_reason_confidence_sql("c.reason_rank")},
                    'reason_available_at', {reason_available_at},
                    'reason_at_existence', {_reason_text_sql(rank_at_existence)},
                    'reason_revised_after_existence', ({rank_at_existence}) <> c.reason_rank,
                    'reason_precedence', [{precedence}],
                    'merger_evidence', json(c.merger_evidence),
                    'cik', c.cik,
                    'cik_linked', c.cik IS NOT NULL,
                    'evidence_count', c.evidence_count,
                    'evidence', c.evidence,
                    'fold_as_of_date', ?::DATE
                )::VARCHAR AS details_json,
                ? AS run_id
            FROM clusters c
            JOIN primary_members p
              ON p.security_id = c.security_id
             AND p.cessation_delist_date = c.delist_date
             AND p.rn = 1
            ORDER BY c.security_id, c.delist_date
            """,
            [options.source, options.event_source, options.event_source, as_of_text, options.run_id],
        )
        event_count_row = store.con.execute(
            "SELECT count(*) FROM delisting_events WHERE source = ?", [options.event_source]
        ).fetchone()
        assert event_count_row is not None
        rows = int(event_count_row[0])
    return rows


class DelistingEvidenceDataset(Dataset):
    dataset_id = "delisting_evidence"
    source_name = DELISTING_EVIDENCE_SOURCE

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: DelistingEvidenceOptions) -> DatasetLoadResult:
        evidence_rows = refresh_delisting_evidence(store, options)
        event_rows = fold_evidence_into_delisting_events(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=evidence_rows,
            source=options.source,
            details={"evidence_rows": evidence_rows, "delisting_events": event_rows},
            run_id=options.run_id,
        )
