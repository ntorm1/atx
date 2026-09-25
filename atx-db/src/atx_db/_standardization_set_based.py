"""Set-based, revision-complete standardized-fundamentals materialization.

Fiscal labels are period-own.  Company Facts ``fy``/``fp`` describe the *filing*
that carried a fact, so a comparative period re-reported in a later 10-K/10-Q
inherits that later filing's labels.  ``fundamental_standardized.fiscal_year`` /
``fiscal_period`` are therefore derived from the fact's own period geometry
against the issuer's observed fiscal-year calendar (see ``_create_fiscal_labels``);
the filing's own labels stay recoverable through ``source_accession``.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import uuid
from collections.abc import Sequence
from contextlib import suppress
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pandas as pd

from ._fundamental_publication import check_publication_session, fundamental_publication
from .calendarization import fiscal_year_label
from .connection import DuckDBStore
from .item_registry import seed_fundamental_item_registry
from .reported_eps_core import reported_eps_conflict_candidates_cte

if TYPE_CHECKING:
    from .standardization import FundamentalStandardizationOptions, StandardizationRule

# A 52/53-week fiscal year ending in the first week of a month belongs to the
# prior month (Saturday-nearest-Dec-31 ending 2021-01-02 is a December year).
FISCAL_YEAR_END_SPILL_DAYS = 7
# A period is positioned by a date inside its final quarter, never on a fiscal
# boundary, so 13/14-week quarters and 52/53-week years cannot straddle a label.
FISCAL_POSITION_LAG_DAYS = 45
# Two observed annual periods overlapping by more than this are rival calendars
# (for example a trailing-twelve-month disclosure); the broader-supported wins.
FISCAL_ANCHOR_OVERLAP_DAYS = 31
FISCAL_YEAR_DAYS = 365.2425


def fiscal_year_end_label(fiscal_year_end: dt.date) -> int:
    """Compustat fiscal-year label of the fiscal year ending on ``fiscal_year_end``.

    Applies ``calendarization.fiscal_year_label`` (Jan-May year ends label the prior
    calendar year) to the 52/53-week-adjusted year-end month.  This is the label the
    set-based SQL assigns to an observed annual period; A7-style consumers matching
    by period geometry can reuse it to name a fiscal year without filing labels.
    """

    return fiscal_year_label(fiscal_year_end - dt.timedelta(days=FISCAL_YEAR_END_SPILL_DAYS))


def _fiscal_year_end_label_sql(date_sql: str) -> str:
    shifted = f"CAST({date_sql} - {FISCAL_YEAR_END_SPILL_DAYS} AS DATE)"
    return (
        f"CAST(year({shifted}) - CASE WHEN month({shifted}) <= 5 THEN 1 ELSE 0 END AS INTEGER)"
    )


@dataclass(frozen=True)
class SetBasedStandardizationOutcome:
    standardized: pd.DataFrame
    exceptions: pd.DataFrame
    standardized_row_count: int
    exception_row_count: int
    input_row_count: int
    build_id: str
    run_id: str
    rule_set_sha256: str
    basis_counts: dict[str, int]


def _rules_frame(rules: Sequence[StandardizationRule]) -> pd.DataFrame:
    return pd.DataFrame.from_records(
        [
            {
                "rule_id": rule.rule_id,
                "item_id": rule.item_id,
                "canonical_code": rule.canonical_code,
                "basis": rule.basis,
                "combination_rule": rule.combination_rule,
                "sign_multiplier": -1.0 if rule.sign_rule == "invert" else 1.0,
                "absolute_value": rule.sign_rule == "absolute",
                "scale_multiplier": {
                    "identity": 1.0,
                    "thousands": 1_000.0,
                    "millions": 1_000_000.0,
                }[rule.scale_rule],
                "missing_policy": rule.missing_policy,
                "valid_from": rule.valid_from,
                "valid_to": rule.valid_to,
                "input_count": len(rule.source_item_ids),
            }
            for rule in rules
            if rule.is_active
        ]
    )


def _rule_inputs_frame(rules: Sequence[StandardizationRule]) -> pd.DataFrame:
    records = [
        {
            "rule_id": rule.rule_id,
            "input_position": position,
            "source_item_id": item_id,
        }
        for rule in rules
        if rule.is_active
        for position, item_id in enumerate(rule.source_item_ids, start=1)
    ]
    if not records:
        return pd.DataFrame(columns=["rule_id", "input_position", "source_item_id"])
    return pd.DataFrame.from_records(records)


def _rule_set_digest(rules: Sequence[StandardizationRule]) -> str:
    payload = [
        {
            "rule_id": rule.rule_id,
            "item_id": rule.item_id,
            "canonical_code": rule.canonical_code,
            "basis": rule.basis,
            "source_aliases": [
                (alias.alias_scheme, alias.alias_code, alias.priority)
                for alias in rule.source_aliases
            ],
            "source_item_ids": rule.source_item_ids,
            "combination_rule": rule.combination_rule,
            "sign_rule": rule.sign_rule,
            "scale_rule": rule.scale_rule,
            "missing_policy": rule.missing_policy,
            "valid_from": None if rule.valid_from is None else rule.valid_from.isoformat(),
            "valid_to": None if rule.valid_to is None else rule.valid_to.isoformat(),
        }
        for rule in sorted(rules, key=lambda value: (value.rule_id, value.item_id, value.basis))
    ]
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _create_candidates(store: DuckDBStore, *, symbols: tuple[str, ...]) -> None:
    symbol_join = ""
    conflict_symbol_join = ""
    if symbols:
        symbol_join = "JOIN _std_symbol_filter ssf ON ssf.symbol = src.symbol"
        conflict_symbol_join = "JOIN _std_symbol_filter ssf ON ssf.symbol = conflict.symbol"
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _std_candidates_all AS
        WITH metric_map AS (
            SELECT
                canonical_metric,
                min(item_id) AS item_id,
                min(concept_priority) AS input_rank
            FROM fundamental_statement_map
            WHERE item_id IS NOT NULL
              AND is_active
            GROUP BY canonical_metric
        ),
        vendor_map AS (
            SELECT lower(vendor) AS vendor, vendor_field, min(item_id) AS item_id
            FROM fundamental_item_vendor_map
            GROUP BY lower(vendor), vendor_field
        ),
        {reported_eps_conflict_candidates_cte()},
        candidate_union AS (
            SELECT
                'fundamental_ttm_points' AS upstream_source,
                10 AS upstream_priority,
                src.ttm_point_id AS upstream_row_id,
                src.source AS upstream_adapter,
                src.security_id,
                src.symbol,
                src.cik,
                coalesce(m.item_id, i.item_id) AS item_id,
                src.canonical_metric,
                src.canonical_metric AS concept,
                'warehouse' AS taxonomy,
                src.unit,
                coalesce(src.unit_type, i.unit_type) AS unit_type,
                'ttm' AS basis,
                src.ttm_start_date AS period_start,
                src.ttm_end_date AS period_end,
                src.fiscal_year,
                src.fiscal_period,
                src.accession_number,
                src.accession_number AS source_accession,
                CAST(NULL AS DATE) AS filed_date,
                src.ttm_value AS value,
                src.available_at,
                coalesce(m.input_rank, 100) AS input_rank,
                src.is_latest_revision AS source_is_latest,
                src.source_loaded_at
            FROM fundamental_ttm_points src
            LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
            LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
            {symbol_join}
            WHERE src.ttm_value IS NOT NULL
              AND src.available_at IS NOT NULL

            UNION ALL

            SELECT
                'fundamental_statement_points' AS upstream_source,
                10 AS upstream_priority,
                src.statement_point_id AS upstream_row_id,
                src.source AS upstream_adapter,
                src.security_id,
                src.symbol,
                src.cik,
                coalesce(src.item_id, m.item_id, i.item_id, a.item_id) AS item_id,
                src.canonical_metric,
                src.concept,
                src.taxonomy,
                src.unit,
                coalesce(src.unit_type, i.unit_type, ai.unit_type) AS unit_type,
                CASE
                    WHEN src.period_type = 'instant' THEN 'instant'
                    WHEN date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 70 AND 120 THEN 'quarterly'
                    WHEN date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 330 AND 380 THEN 'annual'
                    ELSE NULL
                END AS basis,
                src.period_start,
                src.period_end,
                src.fiscal_year,
                src.fiscal_period,
                src.accession_number,
                coalesce(src.source_accession, src.accession_number) AS source_accession,
                src.filed_date,
                src.value,
                src.available_at,
                coalesce(a.coalesce_priority, m.input_rank, 100) AS input_rank,
                src.is_latest_revision AS source_is_latest,
                src.source_loaded_at
            FROM fundamental_statement_points src
            LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
            LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
            LEFT JOIN fundamental_item_alias a
              ON a.alias_scheme = src.taxonomy
             AND a.alias_code = src.concept
             AND coalesce(a.valid_from, DATE '0001-01-01') <= src.period_end
             AND coalesce(a.valid_to, DATE '9999-12-31') > src.period_end
            LEFT JOIN fundamental_item ai ON ai.item_id = a.item_id
            {symbol_join}
            WHERE src.value IS NOT NULL
              AND src.available_at IS NOT NULL
              AND (
                    src.period_type = 'instant'
                 OR date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 70 AND 120
                 OR date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 330 AND 380
              )

            UNION ALL

            SELECT
                'fundamental_xbrl_metric' AS upstream_source,
                20 AS upstream_priority,
                src.metric_id AS upstream_row_id,
                src.source AS upstream_adapter,
                src.security_id,
                src.symbol,
                src.cik,
                coalesce(m.item_id, i.item_id, a.item_id, v.item_id) AS item_id,
                src.canonical_metric,
                src.concept,
                src.taxonomy,
                src.unit,
                coalesce(i.unit_type, ai.unit_type, vi.unit_type) AS unit_type,
                CASE
                    WHEN src.period_type = 'instant' THEN 'instant'
                    WHEN date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 70 AND 120 THEN 'quarterly'
                    WHEN date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 330 AND 380 THEN 'annual'
                    ELSE NULL
                END AS basis,
                src.period_start,
                src.period_end,
                src.fiscal_year,
                src.fiscal_period,
                src.accession_number,
                src.accession_number AS source_accession,
                CAST(src.available_at AS DATE) AS filed_date,
                src.value,
                src.available_at,
                coalesce(a.coalesce_priority, m.input_rank, 100) AS input_rank,
                src.is_latest_revision AS source_is_latest,
                src.source_loaded_at
            FROM fundamental_xbrl_metric src
            LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
            LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
            LEFT JOIN fundamental_item_alias a
              ON a.alias_scheme = src.taxonomy
             AND a.alias_code = src.concept
             AND coalesce(a.valid_from, DATE '0001-01-01') <= src.period_end
             AND coalesce(a.valid_to, DATE '9999-12-31') > src.period_end
            LEFT JOIN fundamental_item ai ON ai.item_id = a.item_id
            LEFT JOIN vendor_map v
              ON v.vendor = lower(src.taxonomy)
             AND v.vendor_field = src.concept
            LEFT JOIN fundamental_item vi ON vi.item_id = v.item_id
            {symbol_join}
            WHERE src.value IS NOT NULL
              AND src.available_at IS NOT NULL
              AND (
                    src.period_type = 'instant'
                 OR date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 70 AND 120
                 OR date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 330 AND 380
              )

            UNION ALL

            SELECT
                conflict.upstream_source,
                conflict.upstream_priority,
                conflict.upstream_row_id,
                conflict.upstream_adapter,
                conflict.security_id,
                conflict.symbol,
                conflict.cik,
                conflict.item_id,
                conflict.canonical_metric,
                conflict.concept,
                conflict.taxonomy,
                conflict.unit,
                conflict.unit_type,
                conflict.basis,
                conflict.period_start,
                conflict.period_end,
                conflict.fiscal_year,
                conflict.fiscal_period,
                conflict.accession_number,
                conflict.source_accession,
                conflict.filed_date,
                conflict.value,
                conflict.available_at,
                conflict.input_rank,
                conflict.source_is_latest,
                conflict.source_loaded_at
            FROM reported_eps_conflict_candidates conflict
            {conflict_symbol_join}
        )
        SELECT *
        FROM candidate_union
        WHERE basis IS NOT NULL
        """
    )
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_candidates AS
        SELECT *
        FROM _std_candidates_all
        QUALIFY item_id IS NULL
             OR upstream_priority = min(upstream_priority) OVER (
                    PARTITION BY security_id, coalesce(item_id, -1), basis, period_start,
                                 period_end, CASE WHEN item_id IS NULL THEN taxonomy ELSE '' END,
                                 CASE WHEN item_id IS NULL THEN concept ELSE '' END
                )
        """
    )


def _create_discrete_quarters(store: DuckDBStore, *, symbols: tuple[str, ...]) -> None:
    """Derive revision-complete discrete quarters from cumulative statement facts."""

    symbol_join = ""
    if symbols:
        symbol_join = "JOIN _std_symbol_filter ssf ON ssf.symbol = src.symbol"
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _std_quarter_inputs AS
        WITH metric_map AS (
            SELECT
                canonical_metric,
                min(item_id) AS item_id,
                min(concept_priority) AS input_rank
            FROM fundamental_statement_map
            WHERE item_id IS NOT NULL
              AND is_active
            GROUP BY canonical_metric
        )
        SELECT
            src.statement_point_id,
            src.source,
            src.security_id,
            src.symbol,
            src.cik,
            coalesce(src.item_id, m.item_id, i.item_id, a.item_id) AS item_id,
            src.canonical_metric,
            src.taxonomy,
            src.concept,
            src.unit,
            coalesce(src.unit_type, i.unit_type, ai.unit_type) AS unit_type,
            src.period_start,
            src.period_end,
            date_diff('day', src.period_start, src.period_end) + 1 AS period_days,
            src.fiscal_year,
            src.fiscal_period,
            src.accession_number,
            coalesce(src.source_accession, src.accession_number) AS source_accession,
            src.filed_date,
            src.as_of_date,
            src.value,
            src.available_at,
            src.revision_sequence,
            src.source_loaded_at,
            coalesce(a.coalesce_priority, m.input_rank, 100) AS input_rank
        FROM fundamental_statement_points src
        LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
        LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
        LEFT JOIN fundamental_item_alias a
          ON a.alias_scheme = src.taxonomy
         AND a.alias_code = src.concept
         AND coalesce(a.valid_from, DATE '0001-01-01') <= src.period_end
         AND coalesce(a.valid_to, DATE '9999-12-31') > src.period_end
        LEFT JOIN fundamental_item ai ON ai.item_id = a.item_id
        {symbol_join}
        WHERE src.period_type = 'duration'
          AND src.period_start IS NOT NULL
          AND src.period_end IS NOT NULL
          AND src.value IS NOT NULL
          AND src.available_at IS NOT NULL
          AND coalesce(src.unit_type, i.unit_type, ai.unit_type) = 'monetary'
          AND date_diff('day', src.period_start, src.period_end) + 1 BETWEEN 70 AND 380
          AND coalesce(src.item_id, m.item_id, i.item_id, a.item_id) IS NOT NULL
        """
    )
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_derived_quarters AS
        WITH logical_periods AS (
            SELECT DISTINCT
                source,
                security_id,
                item_id,
                unit,
                period_start,
                period_end,
                period_days
            FROM _std_quarter_inputs
        ),
        period_pairs AS (
            SELECT
                current_period.source,
                current_period.security_id,
                current_period.item_id,
                current_period.unit,
                current_period.period_start AS cumulative_start,
                current_period.period_end AS current_end,
                current_period.period_days AS current_days,
                prior_period.period_end AS prior_end
            FROM logical_periods current_period
            JOIN logical_periods prior_period
              ON prior_period.source IS NOT DISTINCT FROM current_period.source
             AND prior_period.security_id = current_period.security_id
             AND prior_period.item_id = current_period.item_id
             AND prior_period.unit IS NOT DISTINCT FROM current_period.unit
             AND prior_period.period_start = current_period.period_start
             AND prior_period.period_end < current_period.period_end
             AND prior_period.period_days BETWEEN 70 AND 290
            WHERE current_period.period_days BETWEEN 160 AND 380
            QUALIFY row_number() OVER (
                PARTITION BY current_period.source, current_period.security_id,
                             current_period.item_id, current_period.unit,
                             current_period.period_start, current_period.period_end
                ORDER BY prior_period.period_end DESC
            ) = 1
        ),
        pair_events AS (
            SELECT DISTINCT
                pair.*,
                input.available_at AS event_at
            FROM period_pairs pair
            JOIN _std_quarter_inputs input
              ON input.source IS NOT DISTINCT FROM pair.source
             AND input.security_id = pair.security_id
             AND input.item_id = pair.item_id
             AND input.unit IS NOT DISTINCT FROM pair.unit
             AND input.period_start = pair.cumulative_start
             AND input.period_end IN (pair.current_end, pair.prior_end)
        ),
        visible_inputs AS (
            SELECT
                event.*,
                CASE WHEN input.period_end = event.current_end THEN 1 ELSE 2 END AS input_position,
                input.statement_point_id,
                input.symbol,
                input.cik,
                input.taxonomy,
                input.concept,
                input.unit_type,
                input.fiscal_year,
                input.fiscal_period,
                input.source_accession,
                input.filed_date,
                input.as_of_date,
                input.value,
                input.available_at AS input_available_at,
                row_number() OVER (
                    PARTITION BY event.source, event.security_id, event.item_id, event.unit,
                                 event.cumulative_start, event.current_end, event.prior_end,
                                 event.event_at, input.period_end
                    ORDER BY input.available_at DESC, input.input_rank,
                             input.source_loaded_at DESC NULLS LAST,
                             input.revision_sequence DESC,
                             input.statement_point_id DESC
                ) AS visible_rank
            FROM pair_events event
            JOIN _std_quarter_inputs input
              ON input.source IS NOT DISTINCT FROM event.source
             AND input.security_id = event.security_id
             AND input.item_id = event.item_id
             AND input.unit IS NOT DISTINCT FROM event.unit
             AND input.period_start = event.cumulative_start
             AND input.period_end IN (event.current_end, event.prior_end)
             AND input.available_at <= event.event_at
        ),
        picked AS (
            SELECT *
            FROM visible_inputs
            WHERE visible_rank = 1
        ),
        derived AS (
            SELECT
                'fundamental_statement_points_derived_quarter' AS upstream_source,
                security_id,
                any_value(symbol) FILTER (WHERE input_position = 1) AS symbol,
                any_value(cik) FILTER (WHERE input_position = 1) AS cik,
                item_id,
                'quarterly' AS basis,
                CAST(prior_end + INTERVAL 1 DAY AS DATE) AS period_start,
                current_end AS period_end,
                any_value(fiscal_year) FILTER (WHERE input_position = 1) AS fiscal_year,
                CASE
                    WHEN current_days BETWEEN 160 AND 205 THEN 'Q2_DERIVED'
                    WHEN current_days BETWEEN 250 AND 290 THEN 'Q3_DERIVED'
                    WHEN current_days BETWEEN 330 AND 380 THEN 'Q4_DERIVED'
                    ELSE 'Q_DERIVED'
                END AS fiscal_period,
                max(value) FILTER (WHERE input_position = 1)
                    - max(value) FILTER (WHERE input_position = 2) AS raw_value,
                any_value(unit) AS unit,
                any_value(unit_type) FILTER (WHERE input_position = 1) AS unit_type,
                any_value(source_accession) FILTER (WHERE input_position = 1) AS source_accession,
                max(filed_date) AS filed_date,
                greatest(max(as_of_date), current_end) AS as_of_date,
                event_at AS available_at,
                CAST(to_json(list(concat_ws(':', taxonomy, concept) ORDER BY input_position)) AS VARCHAR)
                    AS input_codes_json,
                CAST(to_json(list(item_id ORDER BY input_position)) AS VARCHAR)
                    AS input_item_ids_json
            FROM picked
            GROUP BY source, security_id, item_id, unit, cumulative_start,
                     current_end, current_days, prior_end, event_at
            HAVING count(DISTINCT input_position) = 2
               AND date_diff('day', CAST(prior_end + INTERVAL 1 DAY AS DATE), current_end) + 1
                   BETWEEN 70 AND 120
        ),
        routed AS (
            SELECT
                derived.*,
                rule.rule_id,
                rule.canonical_code,
                rule.combination_rule,
                CASE WHEN rule.absolute_value THEN abs(derived.raw_value)
                     ELSE derived.raw_value * rule.sign_multiplier END
                    * rule.scale_multiplier AS output_value,
                row_number() OVER (
                    PARTITION BY rule.rule_id, derived.security_id,
                                 derived.period_end, derived.available_at
                    ORDER BY derived.source_accession DESC NULLS LAST,
                             derived.input_codes_json
                ) AS candidate_rank
            FROM derived
            JOIN _std_rules rule
              ON rule.item_id = derived.item_id
             AND rule.basis = 'quarterly'
             AND coalesce(rule.valid_from, DATE '0001-01-01') <= derived.period_end
             AND coalesce(rule.valid_to, DATE '9999-12-31') > derived.period_end
            WHERE rule.combination_rule IN (
                'identity', 'coalesce_priority', 'first_non_null',
                'coalesce_or_sum', 'coalesce_or_difference'
            )
        )
        SELECT
            upstream_source,
            security_id,
            symbol,
            cik,
            item_id,
            canonical_code,
            basis,
            period_start,
            period_end,
            fiscal_year,
            fiscal_period,
            output_value AS value,
            unit,
            unit_type,
            source_accession,
            filed_date,
            as_of_date,
            available_at,
            input_codes_json,
            input_item_ids_json,
            rule_id,
            'discrete_quarter_difference' AS combination_rule
        FROM routed
        WHERE candidate_rank = 1
          AND NOT EXISTS (
                SELECT 1
                FROM _std_direct direct
                WHERE direct.rule_id = routed.rule_id
                  AND direct.security_id = routed.security_id
                  AND direct.period_end = routed.period_end
                  AND direct.available_at <= routed.available_at
          )
        """
    )


def fiscal_period_label_sql(basis: str, upstream_source: str, fiscal_quarter: str) -> str:
    """SQL for a standardized row's ``fiscal_period`` from its period-own fiscal quarter.

    Only a period ending in fiscal Q4 is a fiscal year; an off-cycle twelve-months-ended
    column is labelled by its end quarter, like TTM; YTD-derived quarters are ``Qn_DERIVED``.
    """

    return f"""CASE
                    WHEN {fiscal_quarter} IS NULL THEN NULL
                    WHEN {basis} IN ('annual', 'instant') AND {fiscal_quarter} = 4 THEN 'FY'
                    WHEN {basis} = 'quarterly'
                     AND {upstream_source} LIKE '%fundamental_statement_points_derived_quarter%'
                        THEN 'Q' || {fiscal_quarter} || '_DERIVED'
                    ELSE 'Q' || {fiscal_quarter}
                END"""


def _create_fiscal_labels(store: DuckDBStore) -> None:
    """Label every standardized (security, period_end) by its own fiscal period."""

    has_points = store.con.execute(
        "SELECT count(*) > 0 FROM duckdb_tables() WHERE table_name = 'fundamental_statement_points'"
    ).fetchone()
    create_fiscal_calendar_labels(
        store,
        candidates="_std_candidates_all",
        periods="_std_output_raw",
        prefix="_std_fiscal",
        annual_form_filings=fiscal_year_end_form_filings_sql() if has_points and has_points[0] else None,
    )


FISCAL_ANCHOR_EVIDENCE_TABLES = ("fundamental_statement_points", "fundamental_xbrl_metric")

# Annual-report forms: the filing's own (latest-ending) annual period is its fiscal year,
# whatever fp its facts carry (a 10-K whose facts have a blank fp still declares its year).
FISCAL_YEAR_END_FORMS = tuple("10-K 10-K/A 10-K405 10-K405/A 10-KT 10-KT/A 20-F 20-F/A 40-F 40-F/A".split())


def fiscal_year_end_form_filings_sql() -> str:
    """SELECT ``(security_id, accession_number)`` of the statement-point filings on an annual-report form."""

    forms = ", ".join(f"'{form}'" for form in FISCAL_YEAR_END_FORMS)
    return f"""
            SELECT DISTINCT security_id, accession_number
            FROM fundamental_statement_points
            WHERE security_id IS NOT NULL
              AND accession_number IS NOT NULL
              AND upper(trim(form)) IN ({forms})"""


def fiscal_evidence_basis_sql(instant: str, period_start: str, period_end: str) -> str:
    """The period-geometry basis ``_std_candidates_all`` gives a statement point (NULL = not evidence)."""

    return f"""CASE
            WHEN {instant} THEN 'instant'
            WHEN date_diff('day', {period_start}, {period_end}) + 1 BETWEEN 70 AND 120 THEN 'quarterly'
            WHEN date_diff('day', {period_start}, {period_end}) + 1 BETWEEN 330 AND 380 THEN 'annual'
            ELSE NULL
        END"""


def fiscal_anchor_candidates_sql(tables: Sequence[str] = FISCAL_ANCHOR_EVIDENCE_TABLES) -> str:
    """The fiscal-calendar evidence rows ``create_fiscal_calendar_labels`` reads, from the warehouse.

    Exactly the statement-point and XBRL-metric rows the set-based standardization build
    feeds its fiscal anchors (``_std_candidates_all``: value and ``available_at`` present,
    instant / 70-120 / 330-380-day geometry), with only the columns the anchors use.  Other
    surfaces (``calendarization``) label periods through this relation so their fiscal
    labels are the standardized ones.  ``tables`` limits the union to evidence tables that
    exist (a non-empty subset of :data:`FISCAL_ANCHOR_EVIDENCE_TABLES`).
    """

    unknown = set(tables) - set(FISCAL_ANCHOR_EVIDENCE_TABLES)
    if not tables or unknown:
        raise ValueError(f"fiscal anchor evidence tables must be a subset of {FISCAL_ANCHOR_EVIDENCE_TABLES}")

    basis = fiscal_evidence_basis_sql("src.period_type = 'instant'", "src.period_start", "src.period_end")
    geometry = f"""
          AND {basis} IS NOT NULL"""
    filed_date = {
        "fundamental_statement_points": "src.filed_date",
        "fundamental_xbrl_metric": "CAST(src.available_at AS DATE)",
    }
    return "\n        UNION ALL\n".join(
        f"""
        SELECT
            '{table}' AS upstream_source,
            src.security_id, src.taxonomy, src.concept, {basis} AS basis,
            src.period_start, src.period_end, src.fiscal_period, src.accession_number,
            {filed_date[table]} AS filed_date, src.available_at
        FROM {table} src
        WHERE src.value IS NOT NULL
          AND src.available_at IS NOT NULL{geometry}"""
        for table in FISCAL_ANCHOR_EVIDENCE_TABLES
        if table in tables
    )


def create_fiscal_calendar_labels(
    store: DuckDBStore,
    *,
    candidates: str,
    periods: str,
    prefix: str,
    annual_form_filings: str | None = None,
) -> None:
    """Label every (security, period_end) of ``periods`` by its own fiscal period.

    ``candidates`` is a relation of fiscal-calendar evidence rows shaped like
    ``_std_candidates_all`` (see :func:`fiscal_anchor_candidates_sql`); ``periods`` any
    relation with ``security_id``/``period_end``; ``annual_form_filings`` an optional
    SELECT of the ``(security_id, accession_number)`` filed on an annual-report form
    (:func:`fiscal_year_end_form_filings_sql`; without it only fp tags identify them).
    Builds the temp tables ``{prefix}_anchor_candidates``, ``{prefix}_anchors`` and
    ``{prefix}_labels`` (keyed by ``(security_id, period_end)``: ``fiscal_year``,
    ``fiscal_quarter``, ``fiscal_year_end_month`` of the reference calendar,
    ``reference_kind``).

    Fiscal-year anchors are, first, the fiscal years the issuer itself declared: the
    latest-ending annual (330-380 day) period of each annual filing (an annual-report
    form -- 10-K, 10-KT, 20-F, 40-F -- or every fact carrying fp=FY), ended by its
    filing date.  Comparative, recast and "twelve months ended" columns are never a
    filing's own year, so they cannot become declared anchors.  Every other annual
    period a year-end filing reports is a candidate too (every observed annual period
    for an issuer with no declared year), ranked after every declared one: in the one
    greedy pass it survives only where it overlaps no kept anchor, so a year seen only
    as a comparative (its own 10-K untagged or mis-tagged) still anchors, while recast
    and "twelve months ended" columns, which overlap a declared year, never do -- nor
    does a 10-Q "twelve months ended" column in a year whose 10-K is missing.
    Overlapping candidates (> FISCAL_ANCHOR_OVERLAP_DAYS) are resolved greedily by
    (declared before observed, breadth of support, the primary period of a filing
    declaring the fiscal year end -- all facts fp FY/Q4, which a 10-Q never is --
    then filing support, then earliest availability); only a surviving anchor
    eliminates another.  Each anchor is named by ``fiscal_year_end_label``.  A
    period is positioned at ``period_end - FISCAL_POSITION_LAG_DAYS`` and takes, in order:

    0. the anchor containing that position (quarter = position within the anchor);
    1. else the nearest *following* anchor projected backward whole fiscal years
       (history before the first anchor, and fiscal-year-end transition gaps:
       a transition period ends on the new calendar, as the Compustat FYR rule has it);
    2. else the nearest *preceding* anchor projected forward (the current,
       not-yet-annually-reported fiscal year).

    An issuer with no annual period at all falls back to its latest filing whose
    declared fiscal period positions that filing's own primary (latest-ending)
    period; with neither, labels stay NULL rather than inherit filing labels.

    These are build-time period labels: the anchors are every filing in the
    build, so around a fiscal-year-end change a label can reflect a calendar
    first filed after the row's ``available_at``.  They are not point-in-time
    selectors -- select by period_start/period_end/available_at.  (fy, fp) is
    not unique across a fiscal-year-end change; ``period_end`` is the identity.
    """

    anchor_label = _fiscal_year_end_label_sql("period_end")
    anchor_candidates = f"{prefix}_anchor_candidates"
    anchors = f"{prefix}_anchors"
    labels = f"{prefix}_labels"
    form_filings = annual_form_filings or (
        "SELECT CAST(NULL AS VARCHAR) AS security_id, CAST(NULL AS VARCHAR) AS accession_number WHERE false"
    )
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE {anchor_candidates} AS
        WITH annual_form_filings AS ({form_filings}
        ),
        annual_periods AS (
            SELECT
                security_id,
                period_start,
                period_end,
                count(DISTINCT concat_ws(':', taxonomy, concept)) AS concept_support,
                count(*) AS row_support,
                min(available_at) AS first_seen_at
            FROM {candidates}
            WHERE basis = 'annual'
              AND upstream_source IN ('fundamental_statement_points', 'fundamental_xbrl_metric')
              AND security_id IS NOT NULL
              AND period_start IS NOT NULL
              AND period_end IS NOT NULL
            GROUP BY security_id, period_start, period_end
        ),
        filing_tags AS (
            SELECT
                security_id,
                accession_number,
                count(*) FILTER (WHERE upper(coalesce(fiscal_period, '')) <> 'FY') = 0 AS all_fy,
                count(*) FILTER (WHERE upper(coalesce(fiscal_period, '')) NOT IN ('FY', 'Q4')) = 0 AS all_fy_q4
            FROM {candidates}
            WHERE upstream_source = 'fundamental_statement_points'
              AND security_id IS NOT NULL
              AND accession_number IS NOT NULL
            GROUP BY security_id, accession_number
        ),
        year_end_filings AS (
            -- Filings declaring the fiscal year end: an annual-report form (10-K, 10-KT, 20-F,
            -- 40-F, whatever fp its facts carry) or every fact fp=FY; failing both, fp FY/Q4
            -- (an annual filing whose facts are tagged by quarter).  A 10-Q never declares Q4.
            SELECT
                t.security_id,
                t.accession_number,
                f.accession_number IS NOT NULL OR t.all_fy AS declares_fy
            FROM filing_tags t
            LEFT JOIN annual_form_filings f
              ON f.security_id = t.security_id
             AND f.accession_number = t.accession_number
            WHERE f.accession_number IS NOT NULL OR t.all_fy_q4
        ),
        filing_periods AS (
            SELECT
                c.security_id,
                c.accession_number,
                c.period_start,
                c.period_end,
                any_value(f.declares_fy) AS declares_fy,
                count(DISTINCT concat_ws(':', c.taxonomy, c.concept)) AS filing_concepts,
                min(c.available_at) AS declared_at
            FROM {candidates} c
            JOIN year_end_filings f
              ON f.security_id = c.security_id
             AND f.accession_number = c.accession_number
            WHERE c.upstream_source = 'fundamental_statement_points'
              AND c.basis = 'annual'
              AND c.period_start IS NOT NULL
              AND c.period_end IS NOT NULL
              AND (c.filed_date IS NULL OR c.period_end <= c.filed_date)
            GROUP BY c.security_id, c.accession_number, c.period_start, c.period_end
        ),
        filing_primaries AS (
            SELECT *
            FROM filing_periods
            QUALIFY row_number() OVER (
                PARTITION BY security_id, accession_number
                ORDER BY period_end DESC, filing_concepts DESC, period_start
            ) = 1
        ),
        declared_annual AS (
            SELECT
                p.security_id,
                p.period_start,
                p.period_end,
                'declared_annual_period' AS anchor_basis,
                0 AS anchor_tier,
                any_value(a.concept_support) AS concept_support,
                true AS year_end_primary,
                count(*) AS filing_support,
                min(p.declared_at) AS first_seen_at
            FROM filing_primaries p
            JOIN annual_periods a
              ON a.security_id = p.security_id
             AND a.period_start = p.period_start
             AND a.period_end = p.period_end
            WHERE p.declares_fy
            GROUP BY p.security_id, p.period_start, p.period_end
        ),
        year_end_primaries AS (
            SELECT DISTINCT security_id, period_start, period_end
            FROM filing_primaries
        ),
        year_end_periods AS (
            SELECT DISTINCT security_id, period_start, period_end
            FROM filing_periods
        ),
        declaring_issuers AS (
            SELECT DISTINCT security_id
            FROM declared_annual
        ),
        observed_annual AS (
            -- Every other annual period a year-end filing reports (any observed annual period
            -- for an issuer that declared no year), ranked after every declared anchor: it
            -- survives the greedy pass only where it overlaps no kept anchor.  A period only
            -- 10-Qs report ("twelve months ended") never anchors an issuer with declared years.
            SELECT
                a.security_id,
                a.period_start,
                a.period_end,
                'observed_annual_period' AS anchor_basis,
                1 AS anchor_tier,
                a.concept_support,
                y.security_id IS NOT NULL AS year_end_primary,
                a.row_support AS filing_support,
                a.first_seen_at
            FROM annual_periods a
            LEFT JOIN year_end_primaries y
              ON y.security_id = a.security_id
             AND y.period_start = a.period_start
             AND y.period_end = a.period_end
            WHERE NOT EXISTS (
                SELECT 1
                FROM declared_annual d
                WHERE d.security_id = a.security_id
                  AND d.period_start = a.period_start
                  AND d.period_end = a.period_end
            )
              AND (
                    EXISTS (
                        SELECT 1
                        FROM year_end_periods r
                        WHERE r.security_id = a.security_id
                          AND r.period_start = a.period_start
                          AND r.period_end = a.period_end
                    )
                 OR NOT EXISTS (SELECT 1 FROM declaring_issuers i WHERE i.security_id = a.security_id)
              )
        )
        SELECT
            *,
            row_number() OVER (
                PARTITION BY security_id
                -- Declared before observed; then breadth; then (observed tier) the primary
                -- period of a year-end filing beats a 10-Q "twelve months ended" column of
                -- equal breadth, which is filed -- and so first seen -- earlier.
                ORDER BY anchor_tier, concept_support DESC, year_end_primary DESC,
                         filing_support DESC, first_seen_at, period_end, period_start
            ) AS priority
        FROM (SELECT * FROM declared_annual UNION ALL BY NAME SELECT * FROM observed_annual)
        """
    )
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE {anchors} AS
        WITH RECURSIVE safe_declared AS (
            -- A declared anchor with no overlapping declared rival is kept whatever else
            -- exists: every observed candidate ranks after it.
            SELECT d.security_id, d.period_start, d.period_end
            FROM {anchor_candidates} d
            WHERE d.anchor_tier = 0
              AND NOT EXISTS (
                SELECT 1
                FROM {anchor_candidates} r
                WHERE r.security_id = d.security_id
                  AND r.anchor_tier = 0
                  AND r.priority <> d.priority
                  AND date_diff('day', greatest(d.period_start, r.period_start),
                                least(d.period_end, r.period_end)) + 1 > {FISCAL_ANCHOR_OVERLAP_DAYS}
              )
        ),
        pruned AS (
            -- So an observed candidate overlapping one is eliminated by it in the greedy pass;
            -- dropping it first (same result) keeps recast / twelve-months-ended rivals out of
            -- the recursive walk.
            SELECT c.*
            FROM {anchor_candidates} c
            WHERE c.anchor_tier = 0
               OR NOT EXISTS (
                    SELECT 1
                    FROM safe_declared s
                    WHERE s.security_id = c.security_id
                      AND date_diff('day', greatest(s.period_start, c.period_start),
                                    least(s.period_end, c.period_end)) + 1 > {FISCAL_ANCHOR_OVERLAP_DAYS}
               )
        ),
        conflicted AS (
            -- Only candidates with an overlapping rival need the greedy pass.
            SELECT
                c.security_id,
                c.period_start,
                c.period_end,
                row_number() OVER (PARTITION BY c.security_id ORDER BY c.priority) AS step
            FROM pruned c
            WHERE EXISTS (
                SELECT 1
                FROM pruned r
                WHERE r.security_id = c.security_id
                  AND r.priority <> c.priority
                  AND date_diff('day', greatest(c.period_start, r.period_start),
                                least(c.period_end, r.period_end)) + 1 > {FISCAL_ANCHOR_OVERLAP_DAYS}
            )
        ),
        greedy AS (
            -- Walk each issuer's conflicted candidates in priority order; a candidate
            -- survives only if it does not overlap an already-surviving anchor.
            SELECT security_id, 0 AS step, CAST([] AS STRUCT(s DATE, e DATE)[]) AS kept
            FROM (SELECT DISTINCT security_id FROM conflicted)
            UNION ALL
            SELECT
                g.security_id,
                c.step,
                CASE
                    WHEN len(list_filter(
                        g.kept,
                        k -> date_diff('day', greatest(k.s, c.period_start),
                                       least(k.e, c.period_end)) + 1 > {FISCAL_ANCHOR_OVERLAP_DAYS}
                    )) > 0 THEN g.kept
                    ELSE list_append(g.kept, {{'s': c.period_start, 'e': c.period_end}})
                END
            FROM greedy g
            JOIN conflicted c
              ON c.security_id = g.security_id
             AND c.step = g.step + 1
        ),
        greedy_kept AS (
            SELECT security_id, kept_period.s AS period_start, kept_period.e AS period_end
            FROM (
                SELECT security_id, unnest(kept) AS kept_period
                FROM greedy
                QUALIFY step = max(step) OVER (PARTITION BY security_id)
            )
        ),
        kept AS (
            SELECT c.security_id, c.period_start, c.period_end, c.anchor_basis
            FROM pruned c
            WHERE NOT EXISTS (
                    SELECT 1
                    FROM conflicted x
                    WHERE x.security_id = c.security_id
                      AND x.period_start = c.period_start
                      AND x.period_end = c.period_end
                )
               OR EXISTS (
                    SELECT 1
                    FROM greedy_kept k
                    WHERE k.security_id = c.security_id
                      AND k.period_start = c.period_start
                      AND k.period_end = c.period_end
                )
        ),
        declared_filings AS (
            SELECT
                c.security_id,
                c.accession_number,
                max(c.period_end) AS primary_end,
                min(upper(c.fiscal_period)) AS filing_period
            FROM {candidates} c
            WHERE c.upstream_source = 'fundamental_statement_points'
              AND c.basis IN ('quarterly', 'annual')
              AND c.security_id IS NOT NULL
              AND c.accession_number IS NOT NULL
              AND c.period_end IS NOT NULL
              AND NOT EXISTS (
                    SELECT 1 FROM {anchor_candidates} a WHERE a.security_id = c.security_id
              )
            GROUP BY c.security_id, c.accession_number
            HAVING count(DISTINCT upper(c.fiscal_period)) = 1
               AND count(c.fiscal_period) = count(*)
               AND min(upper(c.fiscal_period)) IN ('Q1', 'Q2', 'Q3', 'Q4', 'FY')
        ),
        declared AS (
            SELECT
                security_id,
                CAST(primary_end + 1 - to_months(3 * position) AS DATE) AS period_start,
                CAST(primary_end + 1 - to_months(3 * position) + INTERVAL 12 MONTH - INTERVAL 1 DAY AS DATE)
                    AS period_end,
                'declared_primary_period' AS anchor_basis
            FROM (
                SELECT
                    *,
                    CASE filing_period WHEN 'Q1' THEN 1 WHEN 'Q2' THEN 2 WHEN 'Q3' THEN 3 ELSE 4 END AS position
                FROM declared_filings
            )
            QUALIFY row_number() OVER (
                PARTITION BY security_id ORDER BY primary_end DESC, accession_number DESC
            ) = 1
        )
        SELECT security_id, period_start, period_end, anchor_basis, {anchor_label} AS fiscal_year
        FROM (SELECT * FROM kept UNION ALL SELECT * FROM declared)
        """
    )
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE {labels} AS
        WITH periods AS (
            SELECT DISTINCT
                security_id,
                period_end,
                CAST(period_end - {FISCAL_POSITION_LAG_DAYS} AS DATE) AS position_date
            FROM {periods}
            WHERE security_id IS NOT NULL
              AND period_end IS NOT NULL
        ),
        bracketed AS (
            -- Anchors overlap by at most FISCAL_ANCHOR_OVERLAP_DAYS and last >= 330
            -- days, so the latest-starting anchor at or before the position is the
            -- only one that can contain it.
            SELECT
                p.security_id,
                p.period_end,
                p.position_date,
                prior.period_start AS prior_start,
                prior.period_end AS prior_end,
                prior.fiscal_year AS prior_fiscal_year,
                following.period_start AS following_start,
                following.period_end AS following_end,
                following.fiscal_year AS following_fiscal_year
            FROM periods p
            ASOF LEFT JOIN {anchors} prior
              ON prior.security_id = p.security_id
             AND prior.period_start <= p.position_date
            ASOF LEFT JOIN {anchors} following
              ON following.security_id = p.security_id
             AND following.period_start > p.position_date
        ),
        references_ AS (
            SELECT
                security_id,
                period_end,
                position_date,
                CASE
                    WHEN prior_end >= position_date THEN 0
                    WHEN following_start IS NOT NULL THEN 1
                    ELSE 2
                END AS reference_kind,
                CASE
                    WHEN prior_end >= position_date THEN prior_start
                    WHEN following_start IS NOT NULL THEN following_start
                    ELSE CAST(prior_end + 1 AS DATE)
                END AS base_start,
                CASE
                    WHEN prior_end >= position_date THEN prior_fiscal_year
                    WHEN following_start IS NOT NULL THEN following_fiscal_year
                    ELSE prior_fiscal_year + 1
                END AS base_fiscal_year,
                CASE
                    WHEN prior_end >= position_date THEN date_diff('day', prior_start, prior_end) + 1
                    ELSE {FISCAL_YEAR_DAYS}
                END AS year_days,
                -- The fiscal-year-end month of the calendar the period is placed on.
                CASE
                    WHEN prior_end >= position_date THEN prior_end
                    WHEN following_start IS NOT NULL THEN following_end
                    ELSE prior_end
                END AS reference_end
            FROM bracketed
            WHERE prior_end IS NOT NULL
               OR following_start IS NOT NULL
        ),
        offsets AS (
            SELECT
                *,
                CASE
                    WHEN reference_kind = 0 THEN 0
                    ELSE CAST(floor(date_diff('day', base_start, position_date) / {FISCAL_YEAR_DAYS}) AS INTEGER)
                END AS year_offset
            FROM references_
        )
        SELECT
            security_id,
            period_end,
            reference_kind,
            base_fiscal_year + year_offset AS fiscal_year,
            CAST(least(4, greatest(1, floor(
                date_diff(
                    'day',
                    CAST(base_start + CAST(round(year_offset * {FISCAL_YEAR_DAYS}) AS INTEGER) AS DATE),
                    position_date
                ) / (year_days / 4.0)
            ) + 1)) AS INTEGER) AS fiscal_quarter,
            CAST(month(CAST(reference_end - {FISCAL_YEAR_END_SPILL_DAYS} AS DATE)) AS INTEGER)
                AS fiscal_year_end_month
        FROM offsets
        """
    )


def _create_output(store: DuckDBStore, *, symbols: tuple[str, ...]) -> None:
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_direct AS
        WITH routed AS (
            SELECT
                c.*,
                r.rule_id,
                r.canonical_code AS output_code,
                r.combination_rule,
                CASE WHEN r.absolute_value THEN abs(c.value) ELSE c.value * r.sign_multiplier END
                    * r.scale_multiplier AS output_value,
                row_number() OVER (
                    PARTITION BY r.rule_id, c.security_id, c.period_end, c.available_at
                    ORDER BY
                        -- The bridge supplies an explicit NULL state first.  At
                        -- an equal visibility clock a direct Company Facts value
                        -- then wins a consistent preliminary release; that order
                        -- is part of the reported-EPS source contract.
                        CASE
                            WHEN c.upstream_source = 'reported_eps_conflict' THEN 0
                            WHEN c.upstream_source = 'fundamental_statement_points'
                             AND c.upstream_adapter = 'SEC companyfacts' THEN 1
                            ELSE 2
                        END,
                        c.input_rank,
                        CASE
                            WHEN c.basis = 'quarterly' THEN abs(date_diff('day', c.period_start, c.period_end) + 1 - 91)
                            WHEN c.basis = 'annual' THEN abs(date_diff('day', c.period_start, c.period_end) + 1 - 365)
                            ELSE 0
                        END,
                        c.upstream_priority,
                        c.source_loaded_at DESC NULLS LAST,
                        c.source_accession DESC NULLS LAST,
                        c.upstream_row_id
                ) AS candidate_rank
            FROM _std_candidates c
            JOIN _std_rules r
              ON r.item_id = c.item_id
             AND r.basis = c.basis
             AND coalesce(r.valid_from, DATE '0001-01-01') <= c.period_end
             AND coalesce(r.valid_to, DATE '9999-12-31') > c.period_end
            WHERE r.combination_rule IN (
                'identity', 'coalesce_priority', 'first_non_null',
                'coalesce_or_sum', 'coalesce_or_difference'
            )
        )
        SELECT
            upstream_source,
            security_id,
            symbol,
            cik,
            item_id,
            output_code AS canonical_code,
            basis,
            period_start,
            period_end,
            fiscal_year,
            fiscal_period,
            output_value AS value,
            unit,
            unit_type,
            source_accession,
            filed_date,
            period_end AS as_of_date,
            available_at,
            CAST(to_json([concat_ws(':', taxonomy, concept)]) AS VARCHAR) AS input_codes_json,
            CAST(to_json([item_id]) AS VARCHAR) AS input_item_ids_json,
            rule_id,
            combination_rule
        FROM routed
        WHERE candidate_rank = 1
        """
    )
    _create_discrete_quarters(store, symbols=symbols)
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_combination_candidates AS
        SELECT *
        FROM _std_candidates

        UNION ALL BY NAME

        SELECT
            derived.upstream_source,
            15 AS upstream_priority,
            sha256(concat_ws('|', derived.rule_id, derived.security_id,
                             CAST(derived.period_end AS VARCHAR),
                             CAST(derived.available_at AS VARCHAR))) AS upstream_row_id,
            'atx_derived' AS upstream_adapter,
            derived.security_id,
            derived.symbol,
            derived.cik,
            derived.item_id,
            derived.canonical_code AS canonical_metric,
            derived.canonical_code AS concept,
            'atx-derived' AS taxonomy,
            derived.unit,
            derived.unit_type,
            derived.basis,
            derived.period_start,
            derived.period_end,
            derived.fiscal_year,
            derived.fiscal_period,
            derived.source_accession AS accession_number,
            derived.source_accession,
            derived.filed_date,
            derived.value,
            derived.available_at,
            100 AS input_rank,
            true AS source_is_latest,
            derived.available_at AS source_loaded_at
        FROM _std_derived_quarters derived
        """
    )
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_combinations AS
        WITH events AS (
            SELECT DISTINCT
                r.rule_id,
                r.item_id,
                r.canonical_code,
                r.basis,
                r.combination_rule,
                r.sign_multiplier,
                r.absolute_value,
                r.scale_multiplier,
                r.missing_policy,
                r.input_count,
                c.security_id,
                c.period_start,
                c.period_end,
                c.available_at AS event_at
            FROM _std_rules r
            JOIN _std_rule_inputs ri ON ri.rule_id = r.rule_id
            JOIN _std_combination_candidates c
              ON c.item_id = ri.source_item_id
             AND c.basis = r.basis
             AND coalesce(r.valid_from, DATE '0001-01-01') <= c.period_end
             AND coalesce(r.valid_to, DATE '9999-12-31') > c.period_end
            WHERE r.combination_rule IN (
                'sum', 'difference', 'coalesce_or_sum', 'coalesce_or_difference'
            )
        ),
        visible_inputs AS (
            SELECT
                e.*,
                ri.input_position,
                c.upstream_source,
                c.symbol,
                c.cik,
                c.fiscal_year,
                c.fiscal_period,
                c.value AS input_value,
                c.unit,
                c.unit_type,
                c.source_accession,
                c.filed_date,
                c.available_at AS input_available_at,
                c.taxonomy,
                c.concept,
                c.item_id AS input_item_id,
                row_number() OVER (
                    PARTITION BY e.rule_id, e.security_id, e.period_start, e.period_end,
                                 e.event_at, ri.input_position
                    ORDER BY c.available_at DESC, c.input_rank, c.upstream_priority,
                             c.source_loaded_at DESC NULLS LAST, c.upstream_row_id
                ) AS input_rank_at_event
            FROM events e
            JOIN _std_rule_inputs ri ON ri.rule_id = e.rule_id
            JOIN _std_combination_candidates c
              ON c.item_id = ri.source_item_id
             AND c.basis = e.basis
             AND c.security_id = e.security_id
             AND c.period_start IS NOT DISTINCT FROM e.period_start
             AND c.period_end = e.period_end
             AND c.available_at <= e.event_at
        ),
        picked AS (
            SELECT * FROM visible_inputs WHERE input_rank_at_event = 1
        ),
        aggregated AS (
        SELECT
            string_agg(DISTINCT upstream_source, '+' ORDER BY upstream_source) AS upstream_source,
            security_id,
            any_value(symbol) AS symbol,
            any_value(cik) AS cik,
            item_id,
            canonical_code,
            basis,
            period_start,
            period_end,
            any_value(fiscal_year) AS fiscal_year,
            any_value(fiscal_period) AS fiscal_period,
            CASE
                WHEN absolute_value THEN abs(sum(
                    CASE WHEN combination_rule IN ('difference', 'coalesce_or_difference') AND input_position = 2 THEN -input_value ELSE input_value END
                ))
                ELSE sum(
                    CASE WHEN combination_rule IN ('difference', 'coalesce_or_difference') AND input_position = 2 THEN -input_value ELSE input_value END
                ) * sign_multiplier
            END * scale_multiplier AS value,
            any_value(unit) AS unit,
            any_value(unit_type) AS unit_type,
            coalesce(
                max(source_accession) FILTER (WHERE input_available_at = event_at),
                max(source_accession)
            ) AS source_accession,
            max(filed_date) AS filed_date,
            period_end AS as_of_date,
            event_at AS available_at,
            CAST(to_json(list(concat_ws(':', taxonomy, concept) ORDER BY input_position)) AS VARCHAR) AS input_codes_json,
            CAST(to_json(list(input_item_id ORDER BY input_position)) AS VARCHAR) AS input_item_ids_json,
            rule_id,
            combination_rule
        FROM picked
        GROUP BY
            rule_id, item_id, canonical_code, basis, combination_rule, sign_multiplier,
            absolute_value, scale_multiplier, missing_policy, input_count, security_id,
            period_start, period_end, event_at
        HAVING count(DISTINCT input_position) = input_count
            OR missing_policy = 'zero_fill'
        )
        SELECT *
        FROM aggregated agg
        WHERE agg.combination_rule IN ('sum', 'difference')
           OR NOT EXISTS (
                SELECT 1
                FROM _std_direct direct
                WHERE direct.rule_id = agg.rule_id
                  AND direct.security_id = agg.security_id
                  AND direct.period_end = agg.period_end
                  AND direct.available_at <= agg.available_at
           )
        """
    )
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_output_raw AS
        SELECT * FROM _std_direct
        UNION ALL BY NAME
        SELECT * FROM _std_derived_quarters
        UNION ALL BY NAME
        SELECT * FROM _std_combinations
        """
    )
    _create_fiscal_labels(store)
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _std_output AS
        WITH sequenced AS (
            SELECT
                raw.* EXCLUDE (fiscal_year, fiscal_period),
                -- Period-own labels; the carrying filing's fy/fp stay recoverable
                -- through source_accession and are never copied here.
                label.fiscal_year,
                {fiscal_period_label_sql("raw.basis", "raw.upstream_source", "label.fiscal_quarter")}
                    AS fiscal_period,
                sha256(concat_ws('|', ctx.source, raw.security_id, CAST(raw.item_id AS VARCHAR),
                                 raw.basis, CAST(raw.period_end AS VARCHAR), raw.rule_id)) AS revision_group_id,
                row_number() OVER revision_window AS revision_sequence,
                count(*) OVER revision_window AS revision_count,
                lag(raw.value) OVER revision_window AS previous_value,
                lead(raw.available_at) OVER revision_window AS valid_to
            FROM _std_output_raw raw
            LEFT JOIN _std_fiscal_labels label
              ON label.security_id = raw.security_id
             AND label.period_end = raw.period_end
            CROSS JOIN _std_context ctx
            WINDOW revision_window AS (
                PARTITION BY raw.security_id, raw.item_id, raw.basis, raw.period_end, raw.rule_id
                ORDER BY raw.available_at, raw.source_accession NULLS FIRST, raw.value
                ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
            )
        )
        SELECT
            sha256(concat_ws('|', ctx.source, security_id, CAST(item_id AS VARCHAR), basis,
                             CAST(period_end AS VARCHAR), CAST(available_at AS VARCHAR), rule_id,
                             coalesce(source_accession, ''))) AS standardized_id,
            ctx.source AS source,
            upstream_source,
            security_id,
            symbol,
            cik,
            item_id,
            canonical_code,
            basis,
            period_start,
            period_end,
            fiscal_year,
            fiscal_period,
            value,
            unit,
            unit_type,
            source_accession,
            filed_date,
            as_of_date,
            available_at,
            input_codes_json,
            input_item_ids_json,
            rule_id,
            combination_rule,
            revision_group_id,
            revision_sequence,
            revision_count,
            CASE WHEN revision_sequence = 1 THEN false ELSE value IS DISTINCT FROM previous_value END AS is_value_changed,
            previous_value,
            CASE WHEN previous_value IS NULL THEN NULL ELSE value - previous_value END AS value_delta,
            CASE
                WHEN previous_value IS NULL OR previous_value = 0 THEN NULL
                ELSE (value - previous_value) / abs(previous_value)
            END AS value_delta_percent,
            CASE WHEN revision_sequence = 1 THEN 'original' ELSE 'restated' END AS update_type,
            valid_to,
            revision_sequence = revision_count AS is_latest_revision,
            ctx.run_id AS run_id
        FROM sequenced
        CROSS JOIN _std_context ctx
        """
    )


def _create_exceptions(store: DuckDBStore) -> None:
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_exceptions AS
        WITH classified AS (
            SELECT
                c.*,
                CASE
                    WHEN c.upstream_source = 'reported_eps_conflict' THEN 'reported_eps_conflict'
                    WHEN c.item_id IS NULL THEN 'unmapped_concept'
                    ELSE 'no_active_standardization_rule'
                END AS reason
            FROM _std_candidates c
            WHERE c.upstream_source = 'reported_eps_conflict'
               OR c.item_id IS NULL
               OR NOT EXISTS (
                    SELECT 1
                    FROM _std_rules r
                    WHERE r.item_id = c.item_id
                      AND r.basis = c.basis
                      AND coalesce(r.valid_from, DATE '0001-01-01') <= c.period_end
                      AND coalesce(r.valid_to, DATE '9999-12-31') > c.period_end
               )
        ),
        sequenced AS (
            SELECT
                classified.*,
                row_number() OVER exception_window AS revision_sequence,
                count(*) OVER exception_window AS revision_count
            FROM classified
            WINDOW exception_window AS (
                PARTITION BY security_id, basis, period_start, period_end, taxonomy, concept
                ORDER BY available_at, source_loaded_at, upstream_row_id
                ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
            )
        )
        SELECT
            sha256(concat_ws('|', ctx.source, upstream_row_id, basis, CAST(period_end AS VARCHAR), reason)) AS exception_id,
            ctx.source AS source,
            upstream_source,
            security_id,
            symbol,
            cik,
            basis,
            period_start,
            period_end,
            accession_number,
            concept,
            taxonomy,
            unit,
            value,
            reason,
            period_end AS as_of_date,
            available_at,
            revision_sequence = revision_count AS is_latest_revision,
            ctx.run_id AS run_id
        FROM sequenced
        CROSS JOIN _std_context ctx
        """
    )


def _drop_temporary_relations(store: DuckDBStore) -> None:
    for table_name in (
        "_std_exceptions",
        "_std_output",
        "_std_fiscal_labels",
        "_std_fiscal_anchors",
        "_std_fiscal_anchor_candidates",
        "_std_output_raw",
        "_std_combinations",
        "_std_combination_candidates",
        "_std_derived_quarters",
        "_std_direct",
        "_std_quarter_inputs",
        "_std_candidates",
        "_std_candidates_all",
    ):
        # Cleanup runs before shadow recycling and again on exit. Qualify temp
        # so a persistent caller table hidden by our temp name is never dropped.
        store.con.execute(f"DROP TABLE IF EXISTS temp.main.{table_name}")
    for relation_name in ("_std_context", "_std_rule_inputs", "_std_rules", "_std_symbol_filter"):
        with suppress(Exception):
            store.con.unregister(relation_name)


def _count(store: DuckDBStore, table_name: str) -> int:
    row = store.con.execute(f"SELECT count(*) FROM {table_name}").fetchone()
    if row is None:
        raise RuntimeError(f"count query returned no row for {table_name}")
    return int(row[0])


def refresh_standardized_set_based(
    store: DuckDBStore,
    options: FundamentalStandardizationOptions,
    rules: Sequence[StandardizationRule],
) -> SetBasedStandardizationOutcome:
    """Materialize all standardized revisions without moving the fact set into Python."""

    check_publication_session(store)
    # The committed registry is authoritative even for an already-populated warehouse.
    # Reseeding is idempotent and prevents additive canonical items/aliases from remaining
    # absent until an operator manually empties the table.
    seed_fundamental_item_registry(store)

    active_rules = tuple(rule for rule in rules if rule.is_active)
    symbols = tuple(sorted({str(value).strip().upper() for value in options.symbols or () if str(value).strip()}))
    run_id = options.run_id or str(uuid.uuid4())
    build_id = str(uuid.uuid4())
    digest = _rule_set_digest(active_rules)
    scope_json = json.dumps(
        {"symbols": list(symbols) if symbols else "ALL_SYMBOLS"},
        sort_keys=True,
        separators=(",", ":"),
    )

    store.con.register("_std_rules", _rules_frame(active_rules))
    store.con.register("_std_rule_inputs", _rule_inputs_frame(active_rules))
    store.con.register(
        "_std_context",
        pd.DataFrame.from_records(
            [{"source": options.source, "run_id": run_id, "build_id": build_id}]
        ),
    )
    if symbols:
        store.con.register("_std_symbol_filter", pd.DataFrame({"symbol": symbols}))

    store.con.execute(
        """
        INSERT INTO fundamental_standardization_builds (
            build_id,source,run_id,rule_set_sha256,rule_count,scope_json,status
        ) VALUES (?,?,?,?,?,?,'running')
        """,
        [build_id, options.source, run_id, digest, len(active_rules), scope_json],
    )
    try:
        _create_candidates(store, symbols=symbols)
        _create_output(store, symbols=symbols)
        _create_exceptions(store)
        input_count = _count(store, "_std_candidates")
        output_count = _count(store, "_std_output")
        exception_count = _count(store, "_std_exceptions")
        basis_counts = {
            str(row[0]): int(row[1])
            for row in store.con.execute(
                "SELECT basis,count(*) FROM _std_output GROUP BY basis ORDER BY basis"
            ).fetchall()
        }
        exception_reason_counts = {
            str(row[0]): int(row[1])
            for row in store.con.execute(
                "SELECT reason,count(*) FROM _std_exceptions GROUP BY reason ORDER BY reason"
            ).fetchall()
        }

        def complete_build() -> None:
            store.con.execute(
                """
                UPDATE fundamental_standardization_builds
                SET status='completed',input_row_count=?,standardized_row_count=?,
                    exception_row_count=?,basis_counts_json=?,exception_reason_counts_json=?,
                    finished_at=now(),updated_at=now()
                WHERE build_id=?
                """,
                [
                    input_count,
                    output_count,
                    exception_count,
                    json.dumps(basis_counts, sort_keys=True, separators=(",", ":")),
                    json.dumps(exception_reason_counts, sort_keys=True, separators=(",", ":")),
                    build_id,
                ],
            )

        with fundamental_publication(
            store,
            ("fundamental_standardized", "fundamental_standardization_exception"),
            replace_where=(
                "source = ? AND symbol IN (SELECT symbol FROM _std_symbol_filter)"
                if symbols else "source = ?"
            ),
            replace_params=(options.source,),
            owned_registrations=(
                "_std_exceptions", "_std_output", "_std_fiscal_labels", "_std_fiscal_anchors",
                "_std_fiscal_anchor_candidates",
                "_std_output_raw", "_std_combinations",
                "_std_combination_candidates", "_std_derived_quarters", "_std_direct",
                "_std_quarter_inputs", "_std_candidates", "_std_candidates_all",
                "_std_context", "_std_rule_inputs", "_std_rules", "_std_symbol_filter",
            ),
            before_build=lambda: _drop_temporary_relations(store),
            before_swap=complete_build,
        ):
            store.con.execute(
                """
                INSERT INTO fundamental_standardized_bulk_stage (
                    standardized_id,source,upstream_source,security_id,symbol,cik,item_id,
                    canonical_code,basis,period_start,period_end,fiscal_year,fiscal_period,
                    value,unit,unit_type,source_accession,filed_date,as_of_date,available_at,
                    input_codes_json,input_item_ids_json,rule_id,combination_rule,
                    revision_group_id,revision_sequence,revision_count,is_value_changed,
                    previous_value,value_delta,value_delta_percent,update_type,valid_to,
                    is_latest_revision,run_id
                )
                SELECT * FROM _std_output
                """
            )
            store.con.execute(
                """
                INSERT INTO fundamental_standardization_exception_bulk_stage (
                    exception_id,source,upstream_source,security_id,symbol,cik,basis,
                    period_start,period_end,accession_number,concept,taxonomy,unit,value,
                    reason,as_of_date,available_at,is_latest_revision,run_id
                )
                SELECT * FROM _std_exceptions
                """
            )

        if output_count <= options.materialize_result_limit:
            standardized = store.con.execute(
                "SELECT * EXCLUDE (source_loaded_at,updated_at) FROM fundamental_standardized WHERE run_id=?",
                [run_id],
            ).df()
        else:
            standardized = pd.DataFrame()
        if exception_count <= options.materialize_result_limit:
            exceptions = store.con.execute(
                "SELECT * EXCLUDE (source_loaded_at,updated_at) FROM fundamental_standardization_exception WHERE run_id=?",
                [run_id],
            ).df()
        else:
            exceptions = pd.DataFrame()
        return SetBasedStandardizationOutcome(
            standardized=standardized,
            exceptions=exceptions,
            standardized_row_count=output_count,
            exception_row_count=exception_count,
            input_row_count=input_count,
            build_id=build_id,
            run_id=run_id,
            rule_set_sha256=digest,
            basis_counts=basis_counts,
        )
    except BaseException as exc:
        store.con.execute(
            """
            UPDATE fundamental_standardization_builds
            SET status='failed',error_message=?,finished_at=now(),updated_at=now()
            WHERE build_id=?
            """,
            [str(exc)[:4000], build_id],
        )
        raise
    finally:
        _drop_temporary_relations(store)
