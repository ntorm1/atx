"""Public delisting evidence: four independent streams, one precedence fold.

``delisting.refresh_delisting_events`` derives delist dates from
``listing_status_intervals``, i.e. from Nasdaq Trader evidence alone, and can never say
*why* a name stopped trading. This module adds three further public streams -- SEC Form
25 / 25-NSE, SEC Form 15, and a last-trade gap in the ticker-history archive -- attributes
a reason to each, and folds them into ``delisting_events`` under a fixed precedence.

Every timestamp is sourced: SEC acceptance datetimes, the Nasdaq file creation time, or
last ``trade_date`` + 22 hours (the archive's own end-of-day convention). Nothing here
reads a clock.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .delisting import seed_delist_code_dim
from .warehouse import json_dumps, quality_check

DELISTING_EVIDENCE_SOURCE = "atx_delisting_evidence_v1"
DELISTING_EVENT_SOURCE = "atx_delisting_public_evidence_v1"
ARCHIVE_GAP_SESSIONS = 30
MERGER_LOOKBACK_DAYS = 365
END_OF_DAY_HOURS = 22

REASON_CATEGORIES: tuple[str, ...] = (
    "bankruptcy",
    "exchange_delist",
    "merger_acquisition",
    "unknown",
    "voluntary",
)

# Forms whose presence shortly before a Form 25 makes the delist a merger/acquisition.
MERGER_FORMS: tuple[str, ...] = ("425", "DEFM14A", "S-4", "S-4/A", "SC 14D9", "SC TO-T")

# (evidence_kind, evidence_rank, delist_code, default_reason_category, reason_confidence)
EVIDENCE_PRECEDENCE: tuple[tuple[str, int, str, str, str], ...] = (
    ("sec_form_25", 1, "SEC_FORM_25", "exchange_delist", "high"),
    ("nasdaq_delete", 2, "NASDAQ_DELETE", "exchange_delist", "high"),
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
    source: str = DELISTING_EVIDENCE_SOURCE
    event_source: str = DELISTING_EVENT_SOURCE
    archive_gap_sessions: int = ARCHIVE_GAP_SESSIONS
    merger_lookback_days: int = MERGER_LOOKBACK_DAYS
    include_archive_inference: bool = True
    as_of_date: dt.date | None = None
    run_id: str | None = None


def _evidence_id_expression(kind_literal: str) -> str:
    """Deterministic content hash for one evidence row."""

    return f"sha256(concat_ws('|', '{kind_literal}', coalesce(security_id, ''), symbol, CAST(delist_date AS VARCHAR)))"


def build_archive_last_trade_sql() -> str:
    """Last bar per security, more than ``?`` sessions before the archive's last session.

    Placeholder order: ``[archive_gap_sessions]``.
    """

    return f"""
        WITH sessions AS (
            SELECT trade_date,
                   row_number() OVER (ORDER BY trade_date) AS session_rank
            FROM (SELECT DISTINCT trade_date FROM equity_daily_bars WHERE trade_date IS NOT NULL)
        ),
        archive_end AS (SELECT max(session_rank) AS last_rank FROM sessions),
        last_bar AS (
            SELECT
                b.security_id,
                max(b.trade_date) AS delist_date,
                any_value(b.symbol ORDER BY b.trade_date DESC) AS symbol
            FROM equity_daily_bars b
            WHERE b.security_id IS NOT NULL AND b.trade_date IS NOT NULL AND b.close IS NOT NULL
            GROUP BY b.security_id
        )
        SELECT
            last_bar.security_id,
            last_bar.symbol,
            last_bar.delist_date,
            CAST(last_bar.delist_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS available_at,
            'equity_daily_bars' AS evidence_source_table,
            CAST(NULL AS VARCHAR) AS source_event_id,
            archive_end.last_rank - s.session_rank AS gap_sessions
        FROM last_bar
        JOIN sessions s ON s.trade_date = last_bar.delist_date
        CROSS JOIN archive_end
        WHERE archive_end.last_rank - s.session_rank > ?
        ORDER BY last_bar.security_id
    """


def build_nasdaq_delete_sql() -> str:
    """Nasdaq Trader delete actions on any of the three venue columns."""

    return """
        SELECT
            e.security_id,
            e.symbol,
            coalesce(e.effective_date, e.as_of_date) AS delist_date,
            coalesce(
                e.source_file_created_at,
                CAST(coalesce(e.effective_date, e.as_of_date) AS TIMESTAMP) + INTERVAL 22 HOUR
            ) AS available_at,
            'nasdaq_listing_events' AS evidence_source_table,
            e.event_id AS source_event_id
        FROM nasdaq_listing_events e
        WHERE coalesce(e.effective_date, e.as_of_date) IS NOT NULL
          AND (
            upper(coalesce(e.nasdaq_action, '')) = 'D'
            OR upper(coalesce(e.bx_action, '')) = 'D'
            OR upper(coalesce(e.psx_action, '')) = 'D'
          )
        ORDER BY e.security_id, delist_date, e.event_id
    """


def build_sec_form_sql(*, form_kind: str) -> str:
    """SEC Form 25/25-NSE or Form 15 evidence.

    ``form_kind`` is ``"form_25"`` or ``"form_15"``. For ``form_25`` the placeholder
    order is ``[*MERGER_FORMS, merger_lookback_days]``; ``form_15`` takes none.
    """

    if form_kind not in {"form_25", "form_15"}:
        raise ValueError(f"unknown form_kind: {form_kind!r}")
    if form_kind == "form_15":
        return """
            SELECT
                s.security_id,
                coalesce(sec.primary_symbol, s.security_id) AS symbol,
                coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) AS delist_date,
                coalesce(
                    s.acceptance_datetime,
                    CAST(s.filing_date AS TIMESTAMP) + INTERVAL 22 HOUR
                ) AS available_at,
                'sec_submissions' AS evidence_source_table,
                s.accession_number AS source_event_id,
                'voluntary' AS reason_category
            FROM sec_submissions s
            LEFT JOIN securities sec ON sec.security_id = s.security_id
            WHERE s.form LIKE '15-%'
              AND coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) IS NOT NULL
            ORDER BY s.security_id, delist_date, s.accession_number
        """
    placeholders = ", ".join("?" for _ in MERGER_FORMS)
    return f"""
        WITH form25 AS (
            SELECT
                s.security_id,
                s.cik,
                s.form,
                coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) AS delist_date,
                coalesce(
                    s.acceptance_datetime,
                    CAST(s.filing_date AS TIMESTAMP) + INTERVAL 22 HOUR
                ) AS available_at,
                s.accession_number
            FROM sec_submissions s
            WHERE s.form IN ('25', '25-NSE')
              AND coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) IS NOT NULL
        ),
        merger_evidence AS (
            SELECT DISTINCT f.accession_number
            FROM form25 f
            JOIN sec_submissions m
              ON m.cik = f.cik
             AND m.form IN ({placeholders})
             AND coalesce(m.filing_date, CAST(m.acceptance_datetime AS DATE)) <= f.delist_date
             AND coalesce(m.filing_date, CAST(m.acceptance_datetime AS DATE))
                 >= f.delist_date - CAST(? AS INTEGER)
        )
        SELECT
            form25.security_id,
            coalesce(sec.primary_symbol, form25.security_id) AS symbol,
            form25.delist_date,
            form25.available_at,
            'sec_submissions' AS evidence_source_table,
            form25.accession_number AS source_event_id,
            CASE
                WHEN me.accession_number IS NOT NULL THEN 'merger_acquisition'
                WHEN form25.form = '25-NSE' THEN 'exchange_delist'
                ELSE 'voluntary'
            END AS reason_category
        FROM form25
        LEFT JOIN securities sec ON sec.security_id = form25.security_id
        LEFT JOIN merger_evidence me ON me.accession_number = form25.accession_number
        ORDER BY form25.security_id, form25.delist_date, form25.accession_number
    """


def build_bankruptcy_overlay_sql() -> str:
    """Securities whose newest directory snapshot at or before the delist date is bankrupt.

    Nasdaq's ``financial_status`` flag ``Q`` means "bankrupt"; the composite codes (``EQ``,
    ``HQ``, ...) contain it, so a substring test is the right predicate.
    """

    return """
        SELECT DISTINCT e.evidence_id
        FROM delisting_evidence e
        JOIN (
            SELECT
                d.symbol,
                d.as_of_date,
                d.financial_status,
                row_number() OVER (PARTITION BY d.symbol, d.as_of_date ORDER BY d.directory) AS rn
            FROM nasdaq_symbol_directory d
            WHERE d.financial_status IS NOT NULL
        ) dir
          ON dir.rn = 1
         AND dir.symbol = e.symbol
         AND dir.as_of_date <= e.delist_date
        WHERE e.source = ?
          AND contains(upper(dir.financial_status), 'Q')
        ORDER BY e.evidence_id
    """


def _stream_insert_sql(
    kind: str,
    rank: int,
    delist_code: str,
    default_reason: str,
    confidence: str,
    *,
    body: str,
    reason_from_body: bool,
) -> str:
    """Wrap one evidence stream query in the shared INSERT projection."""

    reason = "body.reason_category" if reason_from_body else f"'{default_reason}'"
    return f"""
        INSERT OR REPLACE INTO delisting_evidence (
            {", ".join(EVIDENCE_COLUMNS)}, source_loaded_at
        )
        SELECT
            {_evidence_id_expression(kind)},
            ? AS source,
            body.security_id,
            body.symbol,
            '{kind}' AS evidence_kind,
            {rank} AS evidence_rank,
            body.delist_date,
            {reason} AS reason_category,
            '{confidence}' AS reason_confidence,
            '{delist_code}' AS delist_code,
            body.evidence_source_table,
            body.source_event_id,
            body.delist_date AS as_of_date,
            body.available_at,
            ? AS details_json,
            ? AS run_id,
            now()
        FROM ({body}) AS body
        WHERE body.symbol IS NOT NULL AND body.delist_date IS NOT NULL
        ORDER BY body.security_id, body.delist_date
    """


def refresh_delisting_evidence(
    store: DuckDBStore,
    options: DelistingEvidenceOptions | None = None,
) -> int:
    """Rebuild every public delisting evidence stream for this source."""

    options = options or DelistingEvidenceOptions()
    store.initialize()
    seed_delist_code_dim(store)
    details = json_dumps(
        {
            "archive_gap_sessions": options.archive_gap_sessions,
            "merger_lookback_days": options.merger_lookback_days,
            "merger_forms": list(MERGER_FORMS),
        }
    )
    by_kind = {row[0]: row for row in EVIDENCE_PRECEDENCE}

    with store.transaction():
        store.con.execute("DELETE FROM delisting_evidence WHERE source = ?", [options.source])

        kind, rank, code, reason, confidence = by_kind["sec_form_25"]
        store.con.execute(
            _stream_insert_sql(
                kind,
                rank,
                code,
                reason,
                confidence,
                body=build_sec_form_sql(form_kind="form_25"),
                reason_from_body=True,
            ),
            [options.source, details, options.run_id, *MERGER_FORMS, options.merger_lookback_days],
        )

        kind, rank, code, reason, confidence = by_kind["nasdaq_delete"]
        store.con.execute(
            _stream_insert_sql(
                kind,
                rank,
                code,
                reason,
                confidence,
                body=build_nasdaq_delete_sql(),
                reason_from_body=False,
            ),
            [options.source, details, options.run_id],
        )

        kind, rank, code, reason, confidence = by_kind["sec_form_15"]
        store.con.execute(
            _stream_insert_sql(
                kind,
                rank,
                code,
                reason,
                confidence,
                body=build_sec_form_sql(form_kind="form_15"),
                reason_from_body=True,
            ),
            [options.source, details, options.run_id],
        )

        if options.include_archive_inference:
            kind, rank, code, reason, confidence = by_kind["archive_last_trade"]
            store.con.execute(
                _stream_insert_sql(
                    kind,
                    rank,
                    code,
                    reason,
                    confidence,
                    body=build_archive_last_trade_sql(),
                    reason_from_body=False,
                ),
                [options.source, details, options.run_id, options.archive_gap_sessions],
            )

        store.con.execute(
            f"""
            UPDATE delisting_evidence
            SET reason_category = 'bankruptcy', reason_confidence = 'high'
            WHERE evidence_id IN ({build_bankruptcy_overlay_sql()})
            """,
            [options.source],
        )

        count_row = store.con.execute(
            "SELECT count(*) FROM delisting_evidence WHERE source = ?", [options.source]
        ).fetchone()
        assert count_row is not None
        rows = int(count_row[0])

    by_reason = {
        str(row[0]): int(row[1])
        for row in store.con.execute(
            "SELECT reason_category, count(*) FROM delisting_evidence WHERE source = ? "
            "GROUP BY reason_category ORDER BY reason_category",
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
        details={"source": options.source, "by_reason_category": by_reason},
    )
    return rows


def fold_evidence_into_delisting_events(
    store: DuckDBStore,
    options: DelistingEvidenceOptions | None = None,
) -> int:
    """Materialize one ``delisting_events`` row per (security_id, delist_date).

    The winner is the minimum ``evidence_rank``, ties broken by ``evidence_id`` so the
    result is stable. Rows written by ``delisting.refresh_delisting_events`` are never
    touched: the DELETE is scoped to ``options.event_source``.
    """

    options = options or DelistingEvidenceOptions()
    store.initialize()
    with store.transaction():
        store.con.execute("DELETE FROM delisting_events WHERE source = ?", [options.event_source])
        store.con.execute(
            """
            INSERT OR REPLACE INTO delisting_events (
                delisting_event_id, source, listing_status_source, source_listing_status_id,
                security_id, symbol, delist_date, as_of_date, available_at, delist_code,
                delist_reason, delisting_return, delisting_return_type, is_return_imputed,
                return_policy, return_confidence, evidence_source, evidence_source_table,
                source_event_id, method, evidence_confidence, inferred_from_absence,
                details_json, run_id
            )
            WITH ranked AS (
                SELECT
                    e.*,
                    row_number() OVER (
                        PARTITION BY coalesce(e.security_id, e.symbol), e.delist_date
                        ORDER BY e.evidence_rank, e.evidence_id
                    ) AS rn
                FROM delisting_evidence e
                WHERE e.source = ?
            )
            SELECT
                sha256(concat_ws('|', ?, coalesce(security_id, symbol), CAST(delist_date AS VARCHAR))),
                ? AS source,
                evidence_kind AS listing_status_source,
                evidence_id AS source_listing_status_id,
                security_id,
                symbol,
                delist_date,
                as_of_date,
                available_at,
                delist_code,
                reason_category AS delist_reason,
                CAST(NULL AS DOUBLE) AS delisting_return,
                'UNOBSERVED' AS delisting_return_type,
                false AS is_return_imputed,
                'none' AS return_policy,
                'none' AS return_confidence,
                'public_evidence' AS evidence_source,
                evidence_source_table,
                source_event_id,
                'public_evidence_precedence' AS method,
                reason_confidence AS evidence_confidence,
                evidence_kind = 'archive_last_trade' AS inferred_from_absence,
                details_json,
                ? AS run_id
            FROM ranked
            WHERE rn = 1
            ORDER BY coalesce(security_id, symbol), delist_date
            """,
            [options.source, options.event_source, options.event_source, options.run_id],
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
