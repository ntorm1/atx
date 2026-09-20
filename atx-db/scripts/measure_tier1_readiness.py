#!/usr/bin/env python
"""Read-only, bounded evidence inventory; never a Tier-1 release certification.

Use the project's locked Python runtime in the controller's serialized memory
guard slot. This module imports no atx_db code, starts no jobs, and runs no DQC.
"""

from __future__ import annotations

import argparse
import datetime as dt
import decimal
import json
import math
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

ROW_LIMIT = 1000
DETAIL_LIMIT = 2048
ANNUAL_UNIVERSE = "us_common_equity_top3000_market_cap_annual_v1"
ITEM_SOURCE = "fundamental_standardization_v1"
# Kept explicit to avoid importing activation and its package dependency tree.
STAGES = (
    "migrate", "security_master", "symbol_directory", "ticker_history_extract",
    "ticker_history_publish", "sec_bulk_download", "submissions_load",
    "companyfacts_load", "statement_points", "periods", "ttm", "calendarization",
    "standardized", "industry_templates", "reconciliation", "derived_metrics",
    "market_daily", "legacy_liquid_universe", "factor_projections",
    "delisting_evidence", "universe_us_listed", "delisting_terminal_returns",
    "trading_calendar", "survivorship_forward_returns", "item_coverage",
    "provider_coverage", "equity_price_metrics", "quality",
)
# table, date, distinct dimensions, numeric fields, activation stage, option suffix
SURFACES = (
    ("sec_company_facts", "period_end", ("cik", "concept"), ("value",), "companyfacts_load", "-companyfacts"),
    ("fundamental_standardized", "period_end", ("item_id", "basis"), ("value",), "standardized", "-standardized"),
    ("derived_metric_values", "period_end", ("metric_code", "metric_window"), ("value",), "derived_metrics", ""),
    ("equity_daily_bars", "trade_date", ("vendor_security_id",), ("close", "adjusted_close", "split_factor"), "ticker_history_publish", "-broad-bars"),
    ("market_daily_metrics", "trade_date", (), ("market_cap", "shares_outstanding", "adj_close", "pe_ttm", "total_return_1m", "realized_vol_60d", "realized_vol_252d"), "market_daily", ""),
    ("equity_price_metrics", "trade_date", (), ("adjusted_close", "daily_return", "realized_vol_20d", "realized_vol_60d", "beta_60d", "market_correlation_60d", "idiosyncratic_vol_60d", "max_drawdown_126d"), "equity_price_metrics", ""),
    ("exchange_listings", "valid_from", (), (), None, ""),
    ("listing_status_intervals", "valid_from", ("status",), (), None, ""),
    ("universe_us_listed_membership", "valid_from", ("universe_id", "security_type"), (), "universe_us_listed", ""),
    ("delisting_events", "delist_date", ("delist_code",), (), "delisting_evidence", ""),
    ("delisting_terminal_returns", "delist_date", ("terminal_return_source",), ("terminal_return",), "delisting_terminal_returns", ""),
    ("forward_returns_survivorship_safe", "as_of_date", ("horizon_days",), ("forward_return",), "survivorship_forward_returns", ""),
)


def quote(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def safe(value: Any) -> Any:
    """Redact email addresses in strings and sensitive keys in structured objects.

    Plaintext and truncated JSON receive only email-address redaction; arbitrary
    secrets in those strings are not sanitized.
    """
    if isinstance(value, dict):
        return {
            str(k): ("[redacted]" if re.search(r"email|contact|user.?agent|password|secret|token|api.?key", str(k), re.I)
                     else safe(v)) for k, v in value.items()
        }
    if isinstance(value, (tuple, list)):
        return [safe(v) for v in value]
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    if isinstance(value, decimal.Decimal):
        return str(value)
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    if isinstance(value, str):
        value = re.sub(r"[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}", "[redacted-email]", value, flags=re.I)
        return value
    return value


def detail(text: str | None, original_length: int | None) -> dict[str, Any]:
    truncated = (original_length or 0) > DETAIL_LIMIT
    if text is None:
        return {"value": None, "truncated": False}
    try:
        value = json.loads(text) if not truncated else text
    except json.JSONDecodeError:
        value = text
    return {"value": safe(value), "truncated": truncated}


class Measurement:
    def __init__(self, con: Any, as_of: dt.date):
        self.con = con
        self.as_of = as_of
        self.cutoff = dt.datetime.combine(as_of, dt.time(22), tzinfo=dt.UTC)
        self.latest: dict[str, dict[str, Any]] = {}
        self._columns: dict[str, set[str]] = {}

    def rows(self, sql: str, params: list[Any] | None = None) -> dict[str, Any]:
        # Every variable-size query must be SELECT/CTE and receives an outer LIMIT.
        cursor = self.con.execute(f"SELECT * FROM ({sql}) AS report_rows LIMIT {ROW_LIMIT + 1}", params or [])
        names = [column[0] for column in cursor.description]
        values = cursor.fetchmany(ROW_LIMIT + 1)
        return {"rows": [dict(zip(names, row, strict=True)) for row in values[:ROW_LIMIT]],
                "truncated": len(values) > ROW_LIMIT, "row_limit": ROW_LIMIT}

    def one(self, sql: str, params: list[Any] | None = None) -> dict[str, Any]:
        result = self.rows(sql, params)
        if len(result["rows"]) != 1 or result["truncated"]:
            raise RuntimeError("Expected exactly one aggregate row")
        return result["rows"][0]

    def columns(self, table: str) -> set[str]:
        if table not in self._columns:
            result = self.rows("SELECT column_name FROM information_schema.columns "
                               "WHERE table_catalog=current_database() AND table_schema='main' AND table_name=? "
                               "ORDER BY ordinal_position", [table])
            if result["truncated"]:
                raise RuntimeError(f"Too many columns to inspect safely: {table}")
            self._columns[table] = {r["column_name"] for r in result["rows"]}
        return self._columns[table]

    def missing(self, requirements: dict[str, tuple[str, ...]]) -> dict[str, Any] | None:
        absent = {}
        for table, required in requirements.items():
            columns = self.columns(table)
            if not columns:
                absent[table] = "absent relation"
            elif missing := sorted(set(required) - columns):
                absent[table] = {"missing_columns": missing}
        return {"status": "unmeasured", "schema_gaps": absent} if absent else None

    def activation(self) -> dict[str, Any]:
        gap = self.missing({"activation_stage_runs": ("stage", "run_id", "status", "started_at", "finished_at", "rows", "params_json", "error")})
        if gap:
            return gap
        result = self.rows(f"""
            SELECT stage,run_id,status,started_at,finished_at,"rows" AS rows_reported,
                count(*) OVER (PARTITION BY stage) AS retained_attempts,
                CASE WHEN json_valid(params_json) THEN json_extract_string(params_json,'$.as_of_date') END AS requested_as_of_date,
                left(error,{DETAIL_LIMIT}) AS error_excerpt,length(error) AS error_length
            FROM main.activation_stage_runs
            QUALIFY row_number() OVER (PARTITION BY stage ORDER BY started_at DESC,run_id DESC)=1
            ORDER BY stage
        """)
        self.latest = {r["stage"]: r for r in result["rows"]}
        known = set(STAGES)
        ordered = []
        for index, stage in enumerate(STAGES):
            row = self.latest.get(stage)
            if row is None:
                ordered.append({"stage": stage, "status": "not_run", "retained_attempts": 0})
                continue
            earlier = [self.latest[s] for s in STAGES[:index] if s in self.latest]
            row["newer_upstream_attempts"] = [
                {"stage": r["stage"], "run_id": r["run_id"], "status": r["status"]}
                for r in earlier if r["started_at"] > row["started_at"]
            ]
            row["freshness"] = (
                "newer_upstream_attempt_requires_review" if row["newer_upstream_attempts"]
                else "different_observation_date" if row["requested_as_of_date"] != self.as_of.isoformat()
                else "latest_recorded_attempt"
            )
            row["error_excerpt"] = detail(row.pop("error_excerpt"), row.pop("error_length"))
            ordered.append(row)
        ordered.extend(r for r in result["rows"] if r["stage"] not in known)
        result["rows"] = ordered
        result["status"] = "measured" if self.latest else "empty"
        result["note"] = "Retained ledger keys are (stage,run_id); reusing a run ID overwrites that attempt. Completion records execution, not quality. Upstream order is a conservative review signal, not a dependency proof."
        result["attempt_outcomes"] = self.rows("""
            SELECT stage,status,count(*) AS retained_attempts,min(started_at) AS first_started_at,
                   max(started_at) AS last_started_at
            FROM main.activation_stage_runs GROUP BY stage,status ORDER BY stage,status
        """)
        return result

    def lineage(self, stage: str | None, suffix: str = "") -> tuple[str, list[Any], dict[str, Any]]:
        latest = self.latest.get(stage or "")
        if not latest:
            return "false", [], {"status": "unverified", "stage": stage, "reason": "no applicable activation attempt"}
        expected = latest["run_id"] + suffix
        predicate = "run_id=?"
        params: list[Any] = [expected]
        dataset_gap = self.missing({"dataset_runs": ("run_id", "status", "params_json", "started_at", "finished_at")})
        if not dataset_gap:
            predicate += """ OR run_id IN (
                SELECT run_id FROM main.dataset_runs
                WHERE status='succeeded' AND started_at>=?
                  AND finished_at<=? AND CASE WHEN json_valid(params_json)
                    THEN json_extract_string(params_json,'$.run_id') END=?
            )"""
            params += [latest["started_at"], latest["finished_at"], expected]
        return f"({predicate})", params, {
            "status": "link_count_measured", "activation_stage": stage,
            "activation_run_id": latest["run_id"], "activation_status": latest["status"],
            "requested_output_run_id": expected, "stage_freshness": latest.get("freshness"),
            "dataset_lineage": "unmeasured" if dataset_gap else "succeeded UUID runs linked by params_json.run_id within attempt interval",
            "note": "Unlinked rows are not automatically stale; lineage is not proof of semantic correctness or source vintage.",
        }

    def surface(self, spec: tuple[Any, ...]) -> dict[str, Any]:
        table, date_column, dimensions, numeric, stage, suffix = spec
        gap = self.missing({table: (date_column, "security_id")})
        if gap:
            return gap
        columns = self.columns(table)
        selections = ["count(*) AS row_count", "count(DISTINCT security_id) AS distinct_security_ids",
                      "count(*) FILTER (WHERE security_id IS NULL OR trim(security_id)='') AS missing_security_ids",
                      f"min({quote(date_column)}) AS minimum_date", f"max({quote(date_column)}) AS maximum_date",
                      f"count(*) FILTER (WHERE {quote(date_column)} IS NULL) AS missing_dates",
                      f"count(*) FILTER (WHERE {quote(date_column)}>?) AS rows_dated_after_report_date"]
        params: list[Any] = [self.as_of]
        for name in dimensions:
            if name in columns:
                selections.append(f"count(DISTINCT {quote(name)}) AS {quote('distinct_' + name)}")
        if "is_latest_revision" in columns:
            selections.append("count(*) FILTER (WHERE is_latest_revision) AS latest_revision_rows")
        if "available_at" in columns:
            selections.extend(["min(available_at) AS first_available_at", "max(available_at) AS last_available_at",
                               "count(*) FILTER (WHERE available_at IS NULL) AS missing_available_at",
                               "count(*) FILTER (WHERE available_at>?) AS rows_recorded_available_after_cutoff"])
            params.append(self.cutoff)
        if "source_loaded_at" in columns:
            selections.append("max(source_loaded_at) AS last_source_loaded_at")
        for name in numeric:
            if name in columns:
                selections.extend([
                    f"count(*) FILTER (WHERE {quote(name)} IS NULL) AS {quote(name + '__null_rows')}",
                    f"count(*) FILTER (WHERE NOT isfinite({quote(name)})) AS {quote(name + '__nonfinite_rows')}",
                ])
        predicate, lineage_params, lineage = self.lineage(stage, suffix)
        if "run_id" in columns:
            selections.extend(["count(DISTINCT run_id) AS distinct_output_run_ids", "count(*) FILTER (WHERE run_id IS NULL) AS missing_run_ids",
                               f"count(*) FILTER (WHERE {predicate}) AS rows_linked_to_latest_attempt"])
            params.extend(lineage_params)
        aggregate = self.one(f"SELECT {', '.join(selections)} FROM main.{quote(table)}", params)
        return {"status": "measured" if aggregate["row_count"] else "empty", **aggregate,
                "unmeasured_columns": sorted(set((*dimensions, *numeric, "run_id", "available_at")) - columns),
                "lineage": lineage,
                "scope": "All currently stored rows, including revisions. Post-cutoff records are inventory diagnostics, not PIT violations. NULL counts do not count absent facts or establish expected coverage."}

    def annual_coverage(self) -> dict[str, Any]:
        expected = list(range(2015, self.as_of.year))
        gap = self.missing({
            "fundamental_item_coverage": ("source", "universe_id", "item_id", "basis", "fiscal_year", "n_securities", "n_with_value", "coverage_pct", "as_of_date", "cohort_status", "run_id"),
            "item_coverage_cohort_years": ("universe_id", "fiscal_year", "as_of_date", "ranking_date", "candidate_count", "eligible_count", "selected_count", "excluded_listing", "excluded_market_cap", "excluded_rank", "is_completed_year", "status", "run_id"),
        })
        base: dict[str, Any] = {"universe_id": ANNUAL_UNIVERSE, "source": ITEM_SOURCE,
                                "expected_completed_years": expected, "target_items": 110,
                                "minimum_item_coverage_pct": 90, "required_cohort_size": 3000}
        if gap:
            return {**base, **gap}
        years = self.rows("""
            WITH years AS (SELECT range::INTEGER AS fiscal_year FROM range(2015,?)),
            coverage AS (
                SELECT fiscal_year,count(*) AS coverage_rows,count(DISTINCT item_id) AS distinct_items,
                    min(coverage_pct) AS minimum_coverage_pct,max(coverage_pct) AS maximum_coverage_pct,
                    count(DISTINCT item_id) FILTER (WHERE coverage_pct>=90) AS items_at_90pct,
                    count(DISTINCT run_id) AS run_count,min(run_id) AS one_run_id,
                    min(as_of_date) AS earliest_measurement_date,max(as_of_date) AS latest_measurement_date
                FROM main.fundamental_item_coverage
                WHERE source=? AND universe_id=? AND basis='annual' AND year(as_of_date)=? AND as_of_date<=?
                GROUP BY fiscal_year
            )
            SELECT years.fiscal_year,y.as_of_date AS cohort_as_of_date,y.ranking_date,
                y.candidate_count,y.eligible_count,y.selected_count,y.excluded_listing,
                y.excluded_market_cap,y.excluded_rank,y.is_completed_year,y.status AS cohort_status,
                y.run_id AS cohort_run_id,c.coverage_rows,c.distinct_items,c.minimum_coverage_pct,
                c.maximum_coverage_pct,c.items_at_90pct,c.run_count,c.one_run_id,
                c.earliest_measurement_date,c.latest_measurement_date
            FROM years LEFT JOIN main.item_coverage_cohort_years y
                ON y.fiscal_year=years.fiscal_year AND y.universe_id=?
            LEFT JOIN coverage c ON c.fiscal_year=years.fiscal_year ORDER BY years.fiscal_year
        """, [self.as_of.year, ITEM_SOURCE, ANNUAL_UNIVERSE, self.as_of.year, self.as_of, ANNUAL_UNIVERSE])
        for row in years["rows"]:
            date = row["cohort_as_of_date"]
            row["evidence_status"] = (
                "not_run" if date is None else "different_observation_date" if date != self.as_of
                else "empty" if not row["coverage_rows"] else "incomplete_cohort"
                if row["cohort_status"] != "complete" or row["selected_count"] != 3000
                else "measured"
            )
        # Mirrors coverage_gate_count_sql: every completed FY2015+ year must
        # appear exactly once per item with a complete 3000-member denominator.
        gate = self.one("""
            WITH passing AS (
                SELECT c.item_id FROM main.fundamental_item_coverage c
                JOIN main.item_coverage_cohort_years y ON y.universe_id=c.universe_id
                    AND y.fiscal_year=c.fiscal_year AND y.as_of_date=c.as_of_date
                WHERE c.source=? AND c.universe_id=? AND c.basis='annual'
                    AND c.fiscal_year BETWEEN 2015 AND ?-1
                    AND year(c.as_of_date)=? AND c.as_of_date<=?
                GROUP BY c.item_id
                HAVING count(*)=? AND count(DISTINCT c.fiscal_year)=?
                    AND bool_and(coalesce(c.cohort_status='complete' AND y.status='complete'
                        AND y.selected_count=3000 AND c.n_securities=3000
                        AND c.n_with_value BETWEEN 0 AND 3000
                        AND abs(c.coverage_pct-100.0*c.n_with_value/3000)<=0.000001
                        AND c.coverage_pct>=90,false))
            ) SELECT count(*) AS items_meeting_every_completed_year FROM passing
        """, [ITEM_SOURCE, ANNUAL_UNIVERSE, self.as_of.year, self.as_of.year, self.as_of, len(expected), len(expected)])
        linked, link_params, lineage = self.lineage("item_coverage")
        lineage_counts = self.one(f"""
            SELECT count(*) AS annual_coverage_rows,
                count(*) FILTER (WHERE {linked}) AS rows_linked_to_latest_attempt
            FROM main.fundamental_item_coverage WHERE source=? AND universe_id=? AND basis='annual'
        """, [*link_params, ITEM_SOURCE, ANNUAL_UNIVERSE])
        return {**base, "status": "measured" if lineage_counts["annual_coverage_rows"] else "empty",
                "years": years, **gate, "threshold_met_in_recorded_measurements": bool(expected) and gate["items_meeting_every_completed_year"] >= 110,
                "lineage": {**lineage, **lineage_counts},
                "note": "A numeric threshold result does not certify freshness, historical listing evidence, or release readiness. Current fiscal year is not a completed-year gate input."}

    def provider_coverage(self) -> dict[str, Any]:
        gap = self.missing({
            "api_schema_coverage_slo": ("dataset_id", "schema_code", "slo_version", "expected_history_start", "minimum_history_years", "minimum_security_count", "minimum_item_count", "maximum_freshness_lag_days", "is_active", "valid_from"),
            "api_schema_coverage_snapshot": ("coverage_snapshot_id", "dataset_id", "schema_code", "slo_version", "source_relation", "observed_at", "source_loaded_at", "start_time", "end_time", "record_count", "security_count", "item_count", "history_years", "freshness_lag_days", "condition", "failed_slos_json", "run_id"),
        })
        if gap:
            return gap
        expected = self.latest.get("provider_coverage", {}).get("run_id")
        result = self.rows(f"""
            WITH slos AS (
                SELECT * FROM main.api_schema_coverage_slo WHERE is_active
                QUALIFY row_number() OVER (PARTITION BY dataset_id,schema_code ORDER BY valid_from DESC,slo_version DESC)=1
            ), snapshots AS (
                SELECT * FROM main.api_schema_coverage_snapshot
                QUALIFY row_number() OVER (PARTITION BY dataset_id,schema_code
                    ORDER BY observed_at DESC,source_loaded_at DESC,coverage_snapshot_id DESC)=1
            )
            SELECT coalesce(s.dataset_id,p.dataset_id) AS dataset_id,
                coalesce(s.schema_code,p.schema_code) AS schema_code,
                s.slo_version AS active_slo_version,s.expected_history_start,s.minimum_history_years,
                s.minimum_security_count,s.minimum_item_count,s.maximum_freshness_lag_days,
                p.slo_version AS measured_slo_version,p.source_relation,p.observed_at,p.source_loaded_at,
                p.start_time,p.end_time,p.record_count,p.security_count,p.item_count,p.history_years,
                p.freshness_lag_days,p.condition AS recorded_condition,p.run_id,
                left(p.failed_slos_json,{DETAIL_LIMIT}) AS failed_slos_excerpt,
                length(p.failed_slos_json) AS failed_slos_length
            FROM slos s FULL OUTER JOIN snapshots p USING(dataset_id,schema_code)
            ORDER BY dataset_id,schema_code
        """)
        for row in result["rows"]:
            row["failed_slos"] = detail(row.pop("failed_slos_excerpt"), row.pop("failed_slos_length"))
            row["run_link"] = "matches_latest_activation_attempt" if expected and row["run_id"] == expected + "-coverage" else "unverified"
            row["evidence_status"] = (
                "not_run" if row["observed_at"] is None else "stale" if row["observed_at"].date() < self.as_of
                else "after_report_date" if row["observed_at"].date() > self.as_of
                else "slo_version_mismatch" if row["measured_slo_version"] != row["active_slo_version"]
                else "empty" if row["record_count"] == 0 else "recorded"
            )
        return {"status": "measured" if result["rows"] else "empty", **result,
                "note": "Recorded SLO conditions are not recomputed or promoted. An empty failure list with zero records is empty evidence. Snapshot run linkage is separate from outcome."}

    def quality(self) -> dict[str, Any]:
        required = {"data_quality_checks": ("check_id", "dataset_id", "table_name", "check_name", "status", "severity", "observed_value", "threshold_value", "details_json", "checked_at")}
        result_gap = self.missing(required)
        registry_gap = self.missing({"quality_check_registry": ("check_name", "dataset_id", "table_name", "severity", "enabled")})
        if result_gap:
            return result_gap
        registry = ("SELECT check_name,dataset_id,table_name,severity,enabled FROM main.quality_check_registry"
                    if not registry_gap else "SELECT NULL::VARCHAR AS check_name,NULL::VARCHAR AS dataset_id,NULL::VARCHAR AS table_name,NULL::VARCHAR AS severity,NULL::BOOLEAN AS enabled WHERE false")
        result = self.rows(f"""
            WITH latest AS (
                SELECT check_id,dataset_id,table_name,check_name,status,severity,observed_value,
                    threshold_value,checked_at,left(details_json,{DETAIL_LIMIT}) AS details_excerpt,
                    length(details_json) AS details_length,
                    CASE WHEN json_valid(details_json) THEN json_extract_string(details_json,'$.checked_at') END AS modeled_checked_at
                FROM main.data_quality_checks
                QUALIFY row_number() OVER (PARTITION BY dataset_id,table_name,check_name ORDER BY checked_at DESC,check_id DESC)=1
            ), registry AS ({registry})
            SELECT coalesce(r.check_name,q.check_name) AS check_name,
                coalesce(r.dataset_id,q.dataset_id) AS dataset_id,
                coalesce(q.table_name,r.table_name) AS table_name,
                r.enabled,r.severity AS registry_severity,q.severity AS recorded_severity,
                q.status AS recorded_status,q.observed_value,q.threshold_value,q.checked_at,
                q.modeled_checked_at,q.details_excerpt,q.details_length
            FROM registry r FULL OUTER JOIN latest q ON r.check_name=q.check_name AND r.dataset_id=q.dataset_id
            ORDER BY dataset_id,check_name,table_name
        """)
        counts: Counter[str] = Counter()
        upstream = max((r for stage, r in self.latest.items() if stage != "quality"),
                       key=lambda r: r["started_at"], default=None)
        for row in result["rows"]:
            row["details"] = detail(row.pop("details_excerpt"), row.pop("details_length"))
            modeled = row["modeled_checked_at"]
            try:
                observed = dt.datetime.fromisoformat(modeled.replace("Z", "+00:00")).date() if modeled else None
            except (ValueError, AttributeError):
                observed = None
            row["evidence_status"] = (
                "disabled" if row["enabled"] is False else "not_run" if row["recorded_status"] is None
                else "newer_upstream_attempt_requires_review" if upstream and row["checked_at"] < upstream["started_at"]
                else "observation_date_unverified" if observed is None
                else "stale" if observed < self.as_of else "after_report_date" if observed > self.as_of
                else "recorded_run_link_unverified"
            )
            counts[row["evidence_status"]] += 1
        status = "recorded" if any(r["recorded_status"] is not None for r in result["rows"]) else "not_run" if result["rows"] else "empty"
        return {"status": status, **result,
                "returned_row_evidence_counts": dict(counts), "registry_schema_gaps": registry_gap,
                "latest_activation_attempt": self.latest.get("quality"),
                "newest_upstream_attempt": ({k: upstream[k] for k in ("stage", "run_id", "status", "started_at")} if upstream else None),
                "note": "No quality checks executed. checked_at is wall-clock insertion; details.checked_at is the modeled observation clock. This table has no run_id, so even same-day passes cannot be certified against a particular rebuild. Skipped, not-run, stale and empty evidence never become passes."}

    def historical_gaps(self) -> dict[str, Any]:
        result: dict[str, Any] = {"certification": "unmeasured", "note": "Counts of evidence rows or vendor/security IDs do not certify common-equity membership, issuers, historical source vintages, or adjustment economics."}
        gap = self.missing({"exchange_listings": ("security_id", "exchange_code", "mic", "valid_from", "available_at")})
        result["dated_listing_evidence"] = gap or self.one("""
            SELECT count(*) AS row_count,count(DISTINCT security_id) AS securities,
                count(*) FILTER (WHERE nullif(trim(exchange_code),'') IS NOT NULL OR nullif(trim(mic),'') IS NOT NULL) AS rows_with_venue,
                count(*) FILTER (WHERE available_at IS NULL) AS missing_availability,
                count(*) FILTER (WHERE available_at<=CAST(valid_from AS TIMESTAMP)+INTERVAL '22 hours'
                    AND (nullif(trim(exchange_code),'') IS NOT NULL OR nullif(trim(mic),'') IS NOT NULL)) AS venue_rows_known_by_interval_start,
                min(valid_from) AS first_interval_start,max(valid_from) AS last_interval_start
            FROM main.exchange_listings
        """)
        gap = self.missing({"universe_us_listed_membership": ("universe_id", "security_type", "has_cik", "valid_from", "valid_to", "available_at", "is_latest_revision", "security_id")})
        result["historical_universe"] = gap or self.rows("""
            SELECT universe_id,security_type,has_cik,count(*) AS intervals,
                count(DISTINCT security_id) AS securities,min(valid_from) AS first_valid_from,
                max(valid_from) AS last_valid_from,
                count(*) FILTER (WHERE available_at>CAST(valid_from AS TIMESTAMP)+INTERVAL '22 hours') AS intervals_known_after_start,
                count(*) FILTER (WHERE valid_from<=? AND (valid_to IS NULL OR valid_to>=?) AND available_at<=?) AS intervals_at_cutoff
            FROM main.universe_us_listed_membership WHERE is_latest_revision
            GROUP BY universe_id,security_type,has_cik ORDER BY universe_id,security_type,has_cik
        """, [self.as_of, self.as_of, self.cutoff])
        gap = self.missing({
            "delisting_events": ("security_id", "delist_date", "available_at", "delist_code"),
            "delisting_terminal_returns": ("security_id", "delist_date", "available_at", "terminal_return", "terminal_return_id", "source_loaded_at", "terminal_return_source"),
        })
        result["terminal_gaps"] = gap or self.rows("""
            WITH terminals AS (
                SELECT security_id,delist_date,terminal_return,terminal_return_source
                FROM main.delisting_terminal_returns WHERE available_at<=?
                QUALIFY row_number() OVER (PARTITION BY security_id,delist_date
                    ORDER BY available_at DESC,source_loaded_at DESC,terminal_return_id DESC)=1
            )
            SELECT e.delist_code,count(*) AS event_rows,
                count(*) FILTER (WHERE e.security_id IS NULL) AS unresolved_security_events,
                count(*) FILTER (WHERE t.security_id IS NULL) AS events_without_terminal_row,
                count(*) FILTER (WHERE t.security_id IS NOT NULL AND
                    NOT coalesce(isfinite(t.terminal_return) AND t.terminal_return>=-1,false)) AS events_with_invalid_latest_terminal,
                count(*) FILTER (WHERE t.terminal_return_source='observed') AS events_linked_to_observed_terminal,
                count(*) FILTER (WHERE t.terminal_return_source='policy') AS events_linked_to_policy_terminal
            FROM main.delisting_events e LEFT JOIN terminals t
                ON t.security_id=e.security_id AND t.delist_date=e.delist_date
            WHERE e.available_at<=? AND e.delist_date<=?
            GROUP BY e.delist_code ORDER BY e.delist_code
        """, [self.cutoff, self.cutoff, self.as_of])
        result["terminal_note"] = "Event rows can overlap across sources. Latest eligible terminal revision is selected before validity checks; zero event rows cannot establish complete delisting coverage. This does not recompute the survivorship stitch DQC."
        return result

    def cf1(self) -> dict[str, Any]:
        gap = self.missing({"custom_feature_runs": ("run_id", "run_kind", "feature_source", "feature_version", "price_source", "source_sha256", "as_of_date", "run_at")})
        if gap:
            return gap
        runs = self.rows("""
            SELECT run_id,run_kind,feature_source,feature_version,price_source,source_sha256,as_of_date,run_at
            FROM main.custom_feature_runs ORDER BY run_at DESC,run_id DESC
        """)
        gap = self.missing({"custom_feature_evaluations": ("run_id", "horizon_sessions", "split", "is_primary", "spread_dates", "label_coverage", "production_eligible")})
        summaries = gap or self.rows("""
            SELECT run_id,horizon_sessions,split,is_primary,count(*) AS evaluation_rows,
                min(spread_dates) AS minimum_spread_dates,max(spread_dates) AS maximum_spread_dates,
                min(label_coverage) AS minimum_label_coverage,
                count(*) FILTER (WHERE production_eligible) AS recorded_production_eligible_rows
            FROM main.custom_feature_evaluations GROUP BY run_id,horizon_sessions,split,is_primary
            ORDER BY run_id DESC,horizon_sessions,split
        """)
        return {"status": "measured" if runs["rows"] else "empty", "runs": runs, "evaluation_inventory": summaries,
                "inference": "No significance, candidacy, or production eligibility conclusion is made. Eligibility flags are recorded data only; CF1 design keeps production eligibility false pending independent certification."}


def markdown(report: dict[str, Any]) -> str:
    """Small overview followed by explicit control evidence, never security rows."""
    def cell(value: Any) -> str:
        if isinstance(value, (dict, list)):
            value = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        return str(value if value is not None else "unmeasured").replace("|", "\\|").replace("\r", " ").replace("\n", " ")

    def table(rows: list[dict[str, Any]], fields: tuple[str, ...]) -> list[str]:
        return ["| " + " | ".join(fields) + " |", "| " + " | ".join("---" for _ in fields) + " |",
                *["| " + " | ".join(cell(row.get(field)) for field in fields) + " |" for row in rows], ""]

    lines = ["# Tier-1 production measurement", "", f"Observation date: **{report['as_of_date']}**. Generated: {report['generated_at_utc']}.", "",
             "Read-only evidence inventory. Tier-1 parity and release readiness are **not certified** by this report.", "",
             "Counts describe current stored rows, including revisions. Security IDs, vendor IDs and CIKs are distinct identifiers, not verified issuer or common-equity counts. NULL counts omit absent facts. Availability after the cutoff is an inventory diagnostic, not a PIT violation.", "",
             "## Stored surfaces", ""]
    surface_rows = [{"table": name, **values} for name, values in report["surfaces"].items()]
    lines.extend(table(surface_rows, ("table", "status", "row_count", "distinct_security_ids", "minimum_date", "maximum_date", "rows_linked_to_latest_attempt")))
    lines += ["Fundamental item/metric breadth and stored-value missingness:", ""]
    lines.extend(table([row for row in surface_rows if row["table"] in ("fundamental_standardized", "derived_metric_values")],
                       ("table", "distinct_item_id", "distinct_basis", "distinct_metric_code", "distinct_metric_window", "value__null_rows", "value__nonfinite_rows")))
    lines += ["All numeric-field NULL/nonfinite counts, missing columns, lineage and cutoffs are retained in the JSON report.", "", "## Activation attempts", ""]
    activation = report["activation"]
    lines.extend(table(activation.get("rows", []), ("stage", "status", "run_id", "retained_attempts", "requested_as_of_date", "freshness", "error_excerpt")))
    for heading, key in (("Annual item coverage", "annual_item_coverage"), ("Provider SLO evidence", "provider_coverage"), ("Historical evidence gaps", "historical_gaps"), ("Optional CF1 inventory", "cf1")):
        lines += ["## " + heading, "", "```json", json.dumps(report[key], indent=2, ensure_ascii=False), "```", ""]
    quality = report["quality"]
    lines += ["## Recorded quality results", "", quality.get("note", "Quality evidence unmeasured; see schema gaps below."), "",
              "Evidence status and recorded outcome are separate. Disabled or skipped checks do not become passes.", ""]
    lines.extend(table(quality.get("rows", []), ("check_name", "dataset_id", "recorded_severity", "recorded_status", "evidence_status", "checked_at", "modeled_checked_at", "observed_value", "threshold_value", "details")))
    lines += ["Quality report metadata:", "", "```json", json.dumps({k: v for k, v in quality.items() if k != "rows"}, indent=2, ensure_ascii=False), "```", "",
              "## Measurement limits", "", *["- " + note for note in report["limitations"]], ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", required=True, type=Path)
    parser.add_argument("--as-of-date", required=True, type=dt.date.fromisoformat)
    parser.add_argument("--output-json", required=True, type=Path)
    parser.add_argument("--output-markdown", required=True, type=Path)
    parser.add_argument("--memory-limit", default="1GB")
    parser.add_argument("--include-cf1", action="store_true", help="Include bounded CF1 run/evaluation inventories.")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[1-9][0-9]*(?:\.[0-9]+)?\s*(?:MB|GB)", args.memory_limit, re.I):
        parser.error("--memory-limit must be a positive MB or GB quantity")
    args.db_path = args.db_path.resolve(strict=True)
    if not args.db_path.is_file():
        parser.error("--db-path must identify an existing warehouse file")
    outputs = [args.output_json.resolve(), args.output_markdown.resolve()]
    if outputs[0] == outputs[1] or args.db_path in outputs:
        parser.error("report paths must be distinct from each other and the warehouse")
    if outputs[0].suffix.lower() != ".json" or outputs[1].suffix.lower() != ".md":
        parser.error("report paths must have .json and .md extensions")
    for path in outputs:
        if path.exists():
            parser.error(f"refusing to overwrite existing evidence: {path}")
    # Lazy import: --help and importing this standalone script never open a DB
    # or import the atx_db package (whose store has initialization helpers).
    import duckdb

    started = dt.datetime.now(dt.UTC)
    before = args.db_path.stat()
    with (
        tempfile.TemporaryDirectory(prefix=".tier1-measurement-", dir=args.db_path.parent) as spill,
        duckdb.connect(str(args.db_path), read_only=True, config={
            "memory_limit": args.memory_limit, "threads": "1", "preserve_insertion_order": "false",
            "temp_directory": spill, "max_temp_directory_size": "2GB",
        }) as con,
    ):
        # Establish spill settings before locking external access: DuckDB
        # rejects temp_directory changes once external access is disabled.
        # Apply both session settings before starting any report query.
        con.execute("SET enable_external_access = false")
        con.execute("SET TimeZone = 'UTC'")
        con.execute("BEGIN TRANSACTION")
        measurement = Measurement(con, args.as_of_date)
        activation = measurement.activation()
        report = {
            "report_version": 1, "generated_at_utc": started, "as_of_date": args.as_of_date,
            "availability_cutoff": measurement.cutoff, "warehouse_path": str(args.db_path),
            "warehouse_bytes_before": before.st_size,
            "warehouse_mtime_before_utc": dt.datetime.fromtimestamp(before.st_mtime, dt.UTC),
            "duckdb_version": duckdb.__version__, "memory_limit": args.memory_limit,
            "threads": 1, "spill_limit": "2GB", "time_zone": "UTC", "read_only": True, "certification": "unmeasured",
            "activation": activation,
            "surfaces": {spec[0]: measurement.surface(spec) for spec in SURFACES},
            "annual_item_coverage": measurement.annual_coverage(),
            "provider_coverage": measurement.provider_coverage(), "quality": measurement.quality(),
            "historical_gaps": measurement.historical_gaps(),
            "cf1": measurement.cf1() if args.include_cf1 else {"status": "not_requested"},
            "limitations": [
                "All SQL runs sequentially in one read-only snapshot; no migrations, activation, quality checks, schema flips or network access are performed.",
                "Missing relations/columns are unmeasured. Query errors and memory/spill failures abort with a nonzero exit; they are not converted into absent evidence.",
                f"Variable-size results are capped at {ROW_LIMIT} rows per query with explicit truncation flags. Diagnostic text is capped at {DETAIL_LIMIT} characters per row. Email addresses in strings and sensitive keys in parsed structured objects are redacted; arbitrary secrets in plaintext or truncated JSON are not sanitized.",
                "Only aggregates and bounded control records cross into Python. COUNT DISTINCT and SQL scans still consume memory and time; the DuckDB setting is not a process-tree memory cap. Use the controller guard with no competing writer or heavy workload.",
                "Latest activation attempts, output run IDs and demonstrable dataset UUID lineage are reported separately. Unmatched lineage is unverified, not proof of staleness. No historical observation vintage is reconstructed.",
                "Recorded quality results have no run_id. Date agreement alone cannot associate a check with a rebuild or prove it covers current stored rows.",
                "Missing/undersized annual cohorts, empty quality or SLO results, and successful stage execution never certify parity. Historical listing, identity, adjustment and terminal-return completeness need independent evidence.",
            ],
        }
        con.execute("COMMIT")
    after = args.db_path.stat()
    report["warehouse_file_metadata_changed_during_report"] = (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns)
    report["elapsed_seconds"] = (dt.datetime.now(dt.UTC) - started).total_seconds()
    report = safe(report)
    rendered = markdown(report)
    serialized = json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False) + "\n"
    # Exclusive creation preserves earlier operator receipts. Failure to publish
    # either artifact is a visible nonzero failure; no prior file is replaced.
    for path, content in zip(outputs, (serialized, rendered), strict=True):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
    print(json.dumps({"status": "measured", "certification": "unmeasured", "output_json": str(outputs[0]), "output_markdown": str(outputs[1])}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
