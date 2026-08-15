"""Set-based, revision-complete standardized-fundamentals materialization."""

from __future__ import annotations

import hashlib
import json
import uuid
from collections.abc import Sequence
from contextlib import suppress
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pandas as pd

from .connection import DuckDBStore
from .item_registry import seed_fundamental_item_registry

if TYPE_CHECKING:
    from .standardization import FundamentalStandardizationOptions, StandardizationRule


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
    if symbols:
        symbol_join = "JOIN _std_symbol_filter ssf ON ssf.symbol = src.symbol"
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
            WHERE rule.combination_rule IN ('identity', 'coalesce_priority', 'first_non_null')
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
            WHERE r.combination_rule IN ('identity', 'coalesce_priority', 'first_non_null')
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
            WHERE r.combination_rule IN ('sum', 'difference')
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
        )
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
                    CASE WHEN combination_rule = 'difference' AND input_position = 2 THEN -input_value ELSE input_value END
                ))
                ELSE sum(
                    CASE WHEN combination_rule = 'difference' AND input_position = 2 THEN -input_value ELSE input_value END
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
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _std_output AS
        WITH sequenced AS (
            SELECT
                raw.*,
                sha256(concat_ws('|', ctx.source, raw.security_id, CAST(raw.item_id AS VARCHAR),
                                 raw.basis, CAST(raw.period_end AS VARCHAR), raw.rule_id)) AS revision_group_id,
                row_number() OVER revision_window AS revision_sequence,
                count(*) OVER revision_window AS revision_count,
                lag(raw.value) OVER revision_window AS previous_value,
                lead(raw.available_at) OVER revision_window AS valid_to
            FROM _std_output_raw raw
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
                    WHEN c.item_id IS NULL THEN 'unmapped_concept'
                    ELSE 'no_active_standardization_rule'
                END AS reason
            FROM _std_candidates c
            WHERE c.item_id IS NULL
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
        "_std_output_raw",
        "_std_combinations",
        "_std_combination_candidates",
        "_std_derived_quarters",
        "_std_direct",
        "_std_quarter_inputs",
        "_std_candidates",
        "_std_candidates_all",
    ):
        store.con.execute(f"DROP TABLE IF EXISTS {table_name}")
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
        with store.transaction():
            if symbols:
                for table_name in ("fundamental_standardized", "fundamental_standardization_exception"):
                    store.con.execute(
                        f"""
                        DELETE FROM {table_name}
                        WHERE source = ?
                          AND symbol IN (SELECT symbol FROM _std_symbol_filter)
                        """,
                        [options.source],
                    )
            else:
                store.con.execute(
                    "DELETE FROM fundamental_standardized WHERE source = ?",
                    [options.source],
                )
                store.con.execute(
                    "DELETE FROM fundamental_standardization_exception WHERE source = ?",
                    [options.source],
                )
            store.con.execute(
                """
                INSERT INTO fundamental_standardized (
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
                INSERT INTO fundamental_standardization_exception (
                    exception_id,source,upstream_source,security_id,symbol,cik,basis,
                    period_start,period_end,accession_number,concept,taxonomy,unit,value,
                    reason,as_of_date,available_at,is_latest_revision,run_id
                )
                SELECT * FROM _std_exceptions
                """
            )
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
