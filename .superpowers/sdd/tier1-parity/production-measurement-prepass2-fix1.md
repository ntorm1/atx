# Tier-1 production measurement

Observation date: **2026-09-20**. Generated: 2026-09-20T19:20:47.208807+00:00.

Read-only evidence inventory. Tier-1 parity and release readiness are **not certified** by this report.

Counts describe current stored rows, including revisions. Security IDs, vendor IDs and CIKs are distinct identifiers, not verified issuer or common-equity counts. NULL counts omit absent facts. Availability after the cutoff is an inventory diagnostic, not a PIT violation.

## Stored surfaces

| table | status | row_count | distinct_security_ids | minimum_date | maximum_date | rows_linked_to_latest_attempt |
| --- | --- | --- | --- | --- | --- | --- |
| sec_company_facts | measured | 31590760 | 6533 | 1967-05-25 | 2026-09-17 | 31590760 |
| fundamental_standardized | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| derived_metric_values | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| equity_daily_bars | measured | 31959271 | 34251 | 2012-03-26 | 2026-09-18 | 31959271 |
| market_daily_metrics | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| equity_price_metrics | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| exchange_listings | measured | 45820 | 36762 | 2012-03-26 | 2026-09-20 | 0 |
| listing_status_intervals | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| universe_us_listed_membership | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| delisting_events | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| delisting_terminal_returns | empty | 0 | 0 | unmeasured | unmeasured | 0 |
| forward_returns_survivorship_safe | empty | 0 | 0 | unmeasured | unmeasured | 0 |

Fundamental item/metric breadth and stored-value missingness:

| table | distinct_item_id | distinct_basis | distinct_metric_code | distinct_metric_window | value__null_rows | value__nonfinite_rows |
| --- | --- | --- | --- | --- | --- | --- |
| fundamental_standardized | 0 | 0 | unmeasured | unmeasured | 0 | 0 |
| derived_metric_values | unmeasured | unmeasured | 0 | 0 | 0 | 0 |

All numeric-field NULL/nonfinite counts, missing columns, lineage and cutoffs are retained in the JSON report.

## Activation attempts

| stage | status | run_id | retained_attempts | requested_as_of_date | freshness | error_excerpt |
| --- | --- | --- | --- | --- | --- | --- |
| migrate | completed | activation-run1 | 1 | 2026-09-19 | different_observation_date | {"value":null,"truncated":false} |
| security_master | completed | activation-run3 | 2 | 2026-09-20 | latest_recorded_attempt | {"value":null,"truncated":false} |
| symbol_directory | completed | activation-run3 | 1 | 2026-09-20 | latest_recorded_attempt | {"value":null,"truncated":false} |
| ticker_history_extract | completed | activation-run2-prices | 1 | 2026-09-19 | newer_upstream_attempt_requires_review | {"value":null,"truncated":false} |
| ticker_history_publish | completed | activation-prices-updated-bounded | 4 | 2026-09-20 | latest_recorded_attempt | {"value":null,"truncated":false} |
| sec_bulk_download | completed | activation-run3 | 1 | 2026-09-20 | newer_upstream_attempt_requires_review | {"value":null,"truncated":false} |
| submissions_load | failed | activation-source-prepass2 | 3 | 2026-09-20 | latest_recorded_attempt | {"value":"Operator recovery: memory guard stopped the source process for low host headroom (physical 1.096244812 GiB, commit 2.921028137 GiB). finished_at is ledger-recovery time, not the unrecorded stop time. 8,504,213 committed attempt rows across 37,700 CIKs preserved; this is partial work, not net additions or full archive completion. companyfacts_load did not start. Evidence: source-prepass2-stop-inspection.json and activation-source-prepass2-memory.json.","truncated":false} |
| companyfacts_load | completed | activation-run4 | 1 | 2026-09-20 | newer_upstream_attempt_requires_review | {"value":null,"truncated":false} |
| statement_points | failed | activation-run4 | 1 | 2026-09-20 | newer_upstream_attempt_requires_review | {"value":"cannot allocate memory for array","truncated":false} |
| periods | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| ttm | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| calendarization | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| standardized | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| industry_templates | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| reconciliation | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| derived_metrics | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| market_daily | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| legacy_liquid_universe | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| factor_projections | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| delisting_evidence | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| universe_us_listed | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| delisting_terminal_returns | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| trading_calendar | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| survivorship_forward_returns | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| item_coverage | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| provider_coverage | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| equity_price_metrics | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |
| quality | not_run | unmeasured | 0 | unmeasured | unmeasured | unmeasured |

## Annual item coverage

```json
{
  "universe_id": "us_common_equity_top3000_market_cap_annual_v1",
  "source": "fundamental_standardization_v1",
  "expected_completed_years": [
    2015,
    2016,
    2017,
    2018,
    2019,
    2020,
    2021,
    2022,
    2023,
    2024,
    2025
  ],
  "target_items": 110,
  "minimum_item_coverage_pct": 90,
  "required_cohort_size": 3000,
  "status": "empty",
  "years": {
    "rows": [
      {
        "fiscal_year": 2015,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2016,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2017,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2018,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2019,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2020,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2021,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2022,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2023,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2024,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      },
      {
        "fiscal_year": 2025,
        "cohort_as_of_date": null,
        "ranking_date": null,
        "candidate_count": null,
        "eligible_count": null,
        "selected_count": null,
        "excluded_listing": null,
        "excluded_market_cap": null,
        "excluded_rank": null,
        "is_completed_year": null,
        "cohort_status": null,
        "cohort_run_id": null,
        "coverage_rows": null,
        "distinct_items": null,
        "minimum_coverage_pct": null,
        "maximum_coverage_pct": null,
        "items_at_90pct": null,
        "run_count": null,
        "one_run_id": null,
        "earliest_measurement_date": null,
        "latest_measurement_date": null,
        "evidence_status": "not_run"
      }
    ],
    "truncated": false,
    "row_limit": 1000
  },
  "items_meeting_every_completed_year": 0,
  "threshold_met_in_recorded_measurements": false,
  "lineage": {
    "status": "unverified",
    "stage": "item_coverage",
    "reason": "no applicable activation attempt",
    "annual_coverage_rows": 0,
    "rows_linked_to_latest_attempt": 0
  },
  "note": "A numeric threshold result does not certify freshness, historical listing evidence, or release readiness. Current fiscal year is not a completed-year gate input."
}
```

## Provider SLO evidence

```json
{
  "status": "measured",
  "rows": [
    {
      "dataset_id": "ATX.US.EQUITIES",
      "schema_code": "delistings",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2010-01-01",
      "minimum_history_years": 10.0,
      "minimum_security_count": 500,
      "minimum_item_count": null,
      "maximum_freshness_lag_days": 30.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.EQUITIES",
      "schema_code": "market-daily-1d",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2010-01-01",
      "minimum_history_years": 10.0,
      "minimum_security_count": 5000,
      "minimum_item_count": 30,
      "maximum_freshness_lag_days": 7.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.EQUITIES",
      "schema_code": "ohlcv-1d",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2010-01-01",
      "minimum_history_years": 10.0,
      "minimum_security_count": 5000,
      "minimum_item_count": null,
      "maximum_freshness_lag_days": 7.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.EQUITIES",
      "schema_code": "universe",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2010-01-01",
      "minimum_history_years": 10.0,
      "minimum_security_count": 4000,
      "minimum_item_count": null,
      "maximum_freshness_lag_days": 7.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "derived-metrics",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 2500,
      "minimum_item_count": 120,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "industry-standardized",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 2500,
      "minimum_item_count": 200,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "ratios",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 2500,
      "minimum_item_count": 50,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "reconciliation",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 2500,
      "minimum_item_count": 15,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "reported",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 2500,
      "minimum_item_count": 90,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "restatements",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 1000,
      "minimum_item_count": null,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "security-master",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 5000,
      "minimum_item_count": null,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    },
    {
      "dataset_id": "ATX.US.FUNDAMENTALS",
      "schema_code": "standardized",
      "active_slo_version": "1.0.0",
      "expected_history_start": "2009-01-01",
      "minimum_history_years": 15.0,
      "minimum_security_count": 2500,
      "minimum_item_count": 110,
      "maximum_freshness_lag_days": 120.0,
      "measured_slo_version": null,
      "source_relation": null,
      "observed_at": null,
      "source_loaded_at": null,
      "start_time": null,
      "end_time": null,
      "record_count": null,
      "security_count": null,
      "item_count": null,
      "history_years": null,
      "freshness_lag_days": null,
      "recorded_condition": null,
      "run_id": null,
      "failed_slos": {
        "value": null,
        "truncated": false
      },
      "run_link": "unverified",
      "evidence_status": "not_run"
    }
  ],
  "truncated": false,
  "row_limit": 1000,
  "note": "Recorded SLO conditions are not recomputed or promoted. An empty failure list with zero records is empty evidence. Snapshot run linkage is separate from outcome."
}
```

## Historical evidence gaps

```json
{
  "certification": "unmeasured",
  "note": "Counts of evidence rows or vendor/security IDs do not certify common-equity membership, issuers, historical source vintages, or adjustment economics.",
  "dated_listing_evidence": {
    "row_count": 45820,
    "securities": 36762,
    "rows_with_venue": 0,
    "missing_availability": 0,
    "venue_rows_known_by_interval_start": 0,
    "first_interval_start": "2012-03-26",
    "last_interval_start": "2026-09-20"
  },
  "historical_universe": {
    "rows": [],
    "truncated": false,
    "row_limit": 1000
  },
  "terminal_gaps": {
    "rows": [],
    "truncated": false,
    "row_limit": 1000
  },
  "terminal_note": "Event rows can overlap across sources. Latest eligible terminal revision is selected before validity checks; zero event rows cannot establish complete delisting coverage. This does not recompute the survivorship stitch DQC."
}
```

## Optional CF1 inventory

```json
{
  "status": "empty",
  "runs": {
    "rows": [],
    "truncated": false,
    "row_limit": 1000
  },
  "evaluation_inventory": {
    "rows": [],
    "truncated": false,
    "row_limit": 1000
  },
  "inference": "No significance, candidacy, or production eligibility conclusion is made. Eligibility flags are recorded data only; CF1 design keeps production eligibility false pending independent certification."
}
```

## Recorded quality results

No quality checks executed. checked_at is wall-clock insertion; details.checked_at is the modeled observation clock. This table has no run_id, so even same-day passes cannot be certified against a particular rebuild. Skipped, not-run, stale and empty evidence never become passes.

Evidence status and recorded outcome are separate. Disabled or skipped checks do not become passes.

| check_name | dataset_id | recorded_severity | recorded_status | evidence_status | checked_at | modeled_checked_at | observed_value | threshold_value | details |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| bad_adjustment_factor_history_rows | adjustment_factor_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_corp_action_type_dim_rows | adjustment_factor_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_adjustment_factor_history | adjustment_factor_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_adjustment_factor_security_ids | adjustment_factor_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| alpha_backtest_manifests_without_catalog | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| alpha_signal_values_without_catalog | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| alpha_signal_values_without_security | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_alpha_backtest_manifest_rows | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_alpha_expression_catalog_rows | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_alpha_signal_value_rows | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_alpha_backtest_manifests | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_alpha_expression_catalog | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_alpha_signal_values | alpha_research | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| completed_batch_jobs_missing_reproducibility_identity | atx_saas_control | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| malformed_batch_manifest_hashes | atx_saas_control | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| calendar_ttm_no_duplicate_windows | calendarization | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| calendarization_53_week_flagged | calendarization | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| calendarization_coverage_green | calendarization | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| calendarization_map_exactly_one_label | calendarization | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| stitched_ttm_no_duplicate_windows | calendarization | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_corporate_action_dividend_metric_rows | corporate_action_dividend_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_corporate_action_dividend_metric_keys | corporate_action_dividend_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_corporate_action_factor_reconciliation_rows | corporate_action_factor_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_corporate_action_factor_reconciliation_keys | corporate_action_factor_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| mismatched_corporate_action_factor_steps | corporate_action_factor_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_corporate_action_split_metric_rows | corporate_action_split_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_corporate_action_split_metric_keys | corporate_action_split_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| mismatched_corporate_action_split_factors | corporate_action_split_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_corporate_action_values | corporate_actions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_corporate_actions | corporate_actions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_corporate_actions | corporate_actions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| cross_domain_factor_namespace_consistency | cross_domain_factor_values | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_daily_adjustment_factor_rows | daily_adjustment_factors | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_daily_adjustment_factors | daily_adjustment_factors | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_daily_adjustment_factor_security_ids | daily_adjustment_factors | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_delist_code_dim_rows | delist_code_dim | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| delisting_code_reconciliation_unresolved | delisting_code_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_delisting_event_rows | delisting_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_delisting_events | delisting_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_delisting_event_codes | delisting_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_delisting_event_listing_status_ids | delisting_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_delisting_event_return_observations | delisting_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_delisting_event_security_ids | delisting_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_delisting_return_observation_rows | delisting_return_observations | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_delisting_return_observations | delisting_return_observations | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_delisting_return_observation_security_ids | delisting_return_observations | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| delisting_events_without_terminal_return | delisting_terminal_returns | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| entity_classification_invalid_node_references | entity_classification | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| entity_classification_multiple_open_intervals | entity_classification | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_entity_classification_security_ids | entity_classification | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_feature_build_manifests | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_feature_build_manifests | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_feature_definitions | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_feature_values | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| feature_values_without_build_manifest | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| feature_values_without_definition | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_feature_available_at | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_feature_input_hash | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_feature_values | equity_daily_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_equity_price_metric_rows | equity_price_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_equity_price_metric_keys | equity_price_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_actual_duplicate_key | est_actual | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_actual_eps_missing_basis | est_actual | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_actual_invalid_fiscal_period | est_actual | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_actual_missing_available_at | est_actual | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_actual_null_value | est_actual | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_consensus_duplicate_id | est_consensus | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_consensus_invalid_stale_window | est_consensus | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_consensus_invalid_stat_range | est_consensus | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_consensus_missing_available_at | est_consensus | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_consensus_negative_counts | est_consensus | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_detail_duplicate_id | est_detail | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_detail_invalid_revision_window | est_detail | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_detail_invalid_stop_window | est_detail | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_detail_missing_available_at | est_detail | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_guidance_duplicate_id | est_guidance | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_guidance_invalid_values | est_guidance | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_guidance_missing_available_at | est_guidance | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_period_dim_duplicate_id | est_period_dim | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_duplicate_id | est_recommendation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_invalid_active_window | est_recommendation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_invalid_code | est_recommendation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_invalid_price_target | est_recommendation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_missing_available_at | est_recommendation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_summary_bad_total_count | est_recommendation_summary | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_summary_duplicate_id | est_recommendation_summary | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_summary_invalid_price_target_stats | est_recommendation_summary | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_summary_invalid_rating_mean | est_recommendation_summary | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_summary_missing_available_at | est_recommendation_summary | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_recommendation_summary_negative_counts | est_recommendation_summary | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_security_link_conflicting_accepted_targets | est_security_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_security_link_duplicate_id | est_security_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_security_link_invalid_status_confidence | est_security_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_security_link_missing_available_at | est_security_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_security_link_orphan_target_security | est_security_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_surprise_basis_mismatch_pct_null | est_surprise | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| est_surprise_nonfinite_sue | est_surprise | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fact_disagreement_rows | fact_disagreement | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fact_disagreement_agreement_ratio | fact_disagreement | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| factor_operator_pit_safety | factor_engine | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| factor_coverage_asof_universe | factor_panel | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| factor_leakage_tplus0 | factor_panel | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| factor_panel_export_contract | factor_panel | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_feature_dependency_edges | feature_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_feature_set_catalog_rows | feature_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| derived_feature_edges_without_definition | feature_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_feature_dependency_edges | feature_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_feature_set_catalog_keys | feature_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| feature_definitions_without_dependency_edges | feature_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| filer_alias_candidate_primary_has_self | filer_13f_cik_alias | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| filer_alias_confidence_out_of_range | filer_13f_cik_alias | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| filer_alias_overlapping_authoritative_windows | filer_13f_cik_alias | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_filing_context_backfill_attempt_rows | filing_context_backfill_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_filing_context_backfill_attempt_sequences | filing_context_backfill_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| exhausted_filing_context_backfill_attempts | filing_context_backfill_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| stale_running_filing_context_backfill_attempts | filing_context_backfill_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_filing_context_backfill_queue_rows | filing_context_backfill_queue | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| blocked_filing_context_backfill_queue_rows | filing_context_backfill_queue | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_filing_context_backfill_accessions | filing_context_backfill_queue | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| filing_context_backfill_queue_freshness | filing_context_backfill_queue | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| filing_context_backfill_queue_manifest_parity | filing_context_backfill_queue | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_finra_short_quantities | finra_short_interest | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_finra_short_interest | finra_short_interest | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_finra_short_interest_backfill_manifests | finra_short_interest_backfills | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| inconsistent_finra_short_interest_backfill_manifests | finra_short_interest_backfills | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_finra_short_interest_feature_values | finra_short_interest_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_finra_short_interest_xsec_features | finra_short_interest_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| finra_short_volume_bad_rows | finra_short_volume | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| finra_short_volume_multiple_latest_per_key | finra_short_volume | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_footnote_subledger_rows | footnotes | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_form144_intent_rows | form144_intent | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_form144_intent_accessions | form144_intent | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| form144_intent_multiple_latest_per_key | form144_intent | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_form144_to_form4_links | form144_to_form4_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_form144_to_form4_links | form144_to_form4_link | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| survivorship_forward_return_drops_delisted_names | forward_returns_survivorship_safe | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_macro_observations | fred_macro | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| macro_observations_without_series_metadata | fred_macro | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| stale_daily_macro_observations | fred_macro | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| stale_monthly_macro_observations | fred_macro | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_unmapped_concept_report_empty | fundamental_concept_coverage_report | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_fact_revision_rows | fundamental_fact_revisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_revision_change_flags | fundamental_fact_revisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_latest_fundamental_fact_revisions | fundamental_fact_revisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_fact_revision_keys | fundamental_fact_revisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| revision_rows_without_sec_company_facts | fundamental_fact_revisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| sec_company_facts_without_revision_rows | fundamental_fact_revisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_factor_family_coverage | fundamental_factor_values | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_factor_lineage_completeness | fundamental_factor_values | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_item_alias_item_mappings | fundamental_item_alias | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| unmapped_fundamental_fact_concepts | fundamental_item_alias | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_period_rows | fundamental_periods | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_latest_fundamental_periods | fundamental_periods | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_period_keys | fundamental_periods | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| statement_points_without_fundamental_period | fundamental_periods | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_ratio_rows | fundamental_ratios | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_ratio_natural_keys | fundamental_ratios | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_ratio_reconciliation | fundamental_ratios | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_ratios_without_fundamental_points | fundamental_ratios | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| non_finite_fundamental_ratio_values | fundamental_ratios | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| ratio_kind_division_consistency | fundamental_ratios | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_reconciliation_rows | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_latest_fundamental_reconciliation_chains | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_reconciliation_events | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_reconciliation_context_verification_rate | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_reconciliation_serving_freshness | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_reconciliation_serving_manifest_parity | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| latest_hard_fundamental_reconciliation_mismatch_rate | fundamental_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_standardization_exception_rate | fundamental_standardized | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_standardization_template_coverage | fundamental_standardized | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| standardized_item_breadth_target | fundamental_standardized | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_statement_map_rows | fundamental_statement_map | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_statement_map_keys | fundamental_statement_map | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_statement_map_concept_coverage | fundamental_statement_map | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_statement_map_industry_overlay_coverage | fundamental_statement_map | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| loaded_xbrl_concepts_without_statement_map | fundamental_statement_map | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_statement_point_rows | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_statement_value_mapping | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_latest_fundamental_statement_points | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_statement_point_keys | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_statement_points_item_without_fundamental_item | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| mapped_statement_concepts_without_points | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| statement_points_without_map | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| statement_points_without_revision_row | fundamental_statement_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_ttm_change_flags | fundamental_ttm_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_ttm_point_rows | fundamental_ttm_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_latest_fundamental_ttm_points | fundamental_ttm_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_ttm_point_keys | fundamental_ttm_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| ttm_points_without_anchor_statement_point | fundamental_ttm_points | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_fundamental_xbrl_metric_rows | fundamental_xbrl_metric | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_xbrl_metric_keys | fundamental_xbrl_metric | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| multiple_latest_fundamental_xbrl_metric_per_key | fundamental_xbrl_metric | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| xbrl_metric_covers_fundamental_ratio_universe | fundamental_xbrl_metric | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_identifier_resolution_candidates | identifier_resolution_candidates | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_identifier_resolution_candidates | identifier_resolution_candidates | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_identifier_resolution_available_at | identifier_resolution_candidates | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_identifier_resolution_targets | identifier_resolution_candidates | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| accepted_identifier_decisions_without_pit_identifier | identifier_resolution_decisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_identifier_resolution_decisions | identifier_resolution_decisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_identifier_resolution_decisions | identifier_resolution_decisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_identifier_resolution_decision_candidates | identifier_resolution_decisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_identifier_resolution_decision_targets | identifier_resolution_decisions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| industry_template_exactly_one_route | industry_template | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| industry_template_required_item_coverage | industry_template | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_insider_transaction_metric_rows | insider_transaction_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| insider_transaction_metric_flags_consistent | insider_transaction_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| insider_transaction_metrics_multiple_latest_per_key | insider_transaction_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_listing_status_intervals | listing_status_intervals | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_listing_status_interval_ids | listing_status_intervals | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| listing_status_same_method_overlaps | listing_status_intervals | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_listing_status_security_ids | listing_status_intervals | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_macro_metric_rows | macro_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_macro_metric_keys | macro_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_real_fedfunds_when_inputs_available | macro_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_sahm_rule_when_unrate_available | macro_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| metric_lineage_completeness | metric_lineage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_listing_event_actions | nasdaq_listing_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_listing_event_required_fields | nasdaq_listing_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_listing_event_ids | nasdaq_listing_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| listing_event_future_asof | nasdaq_listing_events | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_quality_report_bad_rows | offexchange_quality_report | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_quality_report_multiple_latest_per_key | offexchange_quality_report | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_quality_report_volume_consistent | offexchange_quality_report | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_security_period_pct_out_of_range | offexchange_security_period | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_security_period_total_inconsistent | offexchange_security_period | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_volume_bad_venue_class | offexchange_volume | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| offexchange_volume_multiple_latest_per_key | offexchange_volume | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_press_release_fact_rows | press_release_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| press_release_no_lookahead | press_release_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| press_release_preliminary_vintage_retained | press_release_reconciliation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_provider_parity_rows | provider_parity_matrix | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_provider_parity_domains | provider_parity_matrix | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| provider_parity_rows_without_open_tables | provider_parity_matrix | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_provider_schema_coverage_snapshots | provider_schema_coverage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| provider_schema_coverage_snapshot_without_slo | provider_schema_coverage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| catalog_completeness | schema_contract | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| pit_column_presence | schema_contract | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| semantic_contract | schema_contract | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_13f_holding_keys | sec_13f | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_13f_holdings | sec_13f | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_13f_security_ids | sec_13f | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| thirteenf_cusips_without_identifier_history | sec_13f | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_13f_manager_report_rows | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_13f_manager_rows | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_13f_security_ownership_rows | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_13f_security_position_rows | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_13f_manager_ids | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_13f_manager_report_ids | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_13f_ownership_ids | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_13f_position_ids | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_13f_ownership_feature_values | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_13f_manager_reports | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_13f_ownership_security_ids | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_13f_security_positions | sec_13f_ownership_features | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_blockholder_filing_rows | sec_blockholder_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_blockholder_reporting_person_rows | sec_blockholder_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_blockholder_filings | sec_blockholder_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_blockholder_reporting_persons | sec_blockholder_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_fundamental_points | sec_company_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_period_after_asof | sec_company_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| fundamental_points_item_without_fundamental_item | sec_company_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_fundamental_available_at | sec_company_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| rows_loaded | sec_company_facts | error | passed | newer_upstream_attempt_requires_review | 2026-09-20T14:59:21.084236 | unmeasured | 31590760.0 | 1.0 | {"value":{"as_of_date":"2026-09-20","companyfacts_zip":"data\\cache\\companyfacts.zip","concept_catalog_rows":0,"period_rows":0,"point_rows":31590760,"refresh_derived_surfaces":false,"revision_rows":0,"skip_loaded_targets":true,"source_mode":"bulk_zip","statement_rows":0,"symbol_limit":null,"symbol_offset":0,"symbol_source":"sec_company_tickers","symbols":[],"target_count":8031,"ttm_rows":0,"universe_id":null,"unresolved_cik_candidate_rows":6533},"truncated":false} |
| bad_form4_filing_rows | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_insider_relationship_rows | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_insider_transaction_rows | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_tradingplan_10b5_1_rows | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_form4_filings | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_insider_transaction_ids | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_insider_transactions | sec_insider_ownership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_identifier_validity_ranges | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_identifier_history_keys | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| export_scan_internal_cusip_leak | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| identifier_multi_security_overlaps | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| identifier_same_source_self_overlaps | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| listing_multi_security_overlaps | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| listing_same_source_self_overlaps | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_exchange_listings | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_identifier_history | sec_security_master | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| rows_loaded | sec_submissions | error | passed | newer_upstream_attempt_requires_review | 2026-09-20T12:50:35.071408 | unmeasured | 3045440.0 | 1.0 | {"value":{"ciks":null,"forms":["10-K","10-Q","8-K"],"zip_path":"data\\cache\\submissions.zip"},"truncated":false} |
| bad_security_listing_metric_rows | security_listing_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| security_listing_metric_flags_consistent | security_listing_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| security_listing_metrics_multiple_latest_per_key | security_listing_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| segment_footnote_coverage_counts_valid | segment_footnote_coverage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_segment_fact_rows | segments | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_segment_fact_revision_keys | segments | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| segment_reconciliation_divergence_warning | segments | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_shares_outstanding_history_rows | shares_outstanding_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_shares_outstanding_history | shares_outstanding_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| float_shares_not_above_outstanding | shares_outstanding_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_shares_outstanding_security_ids | shares_outstanding_history | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_short_interest_metric_rows | short_interest_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_short_interest_metric_keys | short_interest_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| liquid_short_pressure_without_tradeability | short_interest_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| persistent_short_pressure_without_squeeze | short_interest_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| short_volume_high_flow_flag_consistent | short_volume_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| short_volume_metrics_bad_rows | short_volume_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| short_volume_metrics_multiple_latest_per_key | short_volume_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_ohlcv_values | tbltickerhistory_daily | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_equity_daily_bar_keys | tbltickerhistory_daily | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_equity_daily_bars | tbltickerhistory_daily | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| institutional_latest_date_breadth | tbltickerhistory_daily | error | passed | newer_upstream_attempt_requires_review | 2026-09-20T18:00:09.053304 | unmeasured | 12386.0 | 5000.0 | {"value":"{\"duplicate_keys\": 0, \"invalid_rows\": 0, \"latest_date\": \"2026-09-18\", \"provenance\": {\"adjusted_close_recipe\": \"finite_positive(close * cumulReturnFactor)\", \"availability_policy\": \"modeled tradingDate 22:00 backfill; not historical publication evidence\", \"duplicate_policy\": \"quarantine all repeated positive vendor-security/date keys\", \"economic_adjustment_status\": \"unverified; diagnostics are internal consistency only\", \"historical_source_vintage\": \"unknown\", \"identifier_type_policy\": \"securityID/dn require native integer or strict integer text; native float/decimal columns rejected\", \"identity_policy\": \"current-ticker CIK shortcut unverified; historical symbol is display only\", \"source_format\": \"parquet\", \"source_numeric_representation\": \"native typed Parquet values; original text precision and source vintage unknown\", \"split_factor\": \"unknown; returnFactor includes distributions and is not split-only\", \"vendor_current_delivery\": \"05:00 America/Chicago T+1; historical vintage not authenticated\"}, \"rows\": 31959271, \"run_id\": \"activation-prices-updated-bounded-broad-bars\", \"securities\": 34251, \"source_diagnostics\": {\"adjacent_unique_pairs\": 31473425, \"adjusted_return_comparable\": 31461105, \"adjusted_return_not_comparable\": 589013, \"adjusted_return_residual_gt_1e_3\": 43082, \"adjusted_return_residual_gt_1e_4\": 51501, \"adjusted_return_residual_gt_1e_6\": 52994, \"adjusted_return_residual_gt_1e_8\": 22790207, \"cumulative_recurrence_comparable\": 31473425, \"cumulative_recurrence_not_comparable\": 576693, \"cumulative_recurrence_residual_gt_1e_3\": 0, \"cumulative_recurrence_residual_gt_1e_4\": 0, \"cumulative_recurrence_residual_gt_1e_6\": 0, \"cumulative_recurrence_residual_gt_1e_8\": 0, \"current_symbol_linked_unverified_rows\": 32164197, \"daily_factor_comparable\": 31461105, \"daily_factor_not_comparable\": 589013, \"daily_factor_residual_gt_1e_3\": 43090, \"daily_factor_residual_gt_1e_4\": 51502, \"daily_factor_residual_gt_1e_6\": 53089, \"daily_factor_residual_gt_1e_8\": 333738, \"first_date\": \"2012-03-26 00:00:00\", \"invalid_adjusted_product","truncated":true} |
| missing_bar_available_at | tbltickerhistory_daily | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_equity_daily_bars | tbltickerhistory_daily | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| source_preprojection_diagnostics | tbltickerhistory_daily | warning | warning | newer_upstream_attempt_requires_review | 2026-09-20T17:57:13.318846 | unmeasured | 2690.0 | 0.0 | {"value":"{\"diagnostics\": {\"adjacent_unique_pairs\": 31473425, \"adjusted_return_comparable\": 31461105, \"adjusted_return_not_comparable\": 589013, \"adjusted_return_residual_gt_1e_3\": 43082, \"adjusted_return_residual_gt_1e_4\": 51501, \"adjusted_return_residual_gt_1e_6\": 52994, \"adjusted_return_residual_gt_1e_8\": 22790207, \"cumulative_recurrence_comparable\": 31473425, \"cumulative_recurrence_not_comparable\": 576693, \"cumulative_recurrence_residual_gt_1e_3\": 0, \"cumulative_recurrence_residual_gt_1e_4\": 0, \"cumulative_recurrence_residual_gt_1e_6\": 0, \"cumulative_recurrence_residual_gt_1e_8\": 0, \"daily_factor_comparable\": 31461105, \"daily_factor_not_comparable\": 589013, \"daily_factor_residual_gt_1e_3\": 43090, \"daily_factor_residual_gt_1e_4\": 51502, \"daily_factor_residual_gt_1e_6\": 53089, \"daily_factor_residual_gt_1e_8\": 333738, \"first_date\": \"2012-03-26 00:00:00\", \"invalid_adjusted_product_rows\": 0, \"invalid_close_rows\": 0, \"invalid_cumulative_factor_rows\": 0, \"invalid_key_rows\": 892, \"invalid_ohlcv_rows\": 332607, \"last_date\": \"2026-09-18 00:00:00\", \"latest_date_distinct_positive_vendor_ids\": 12462, \"latest_date_source_rows\": 12489, \"missing_or_invalid_id_rows\": 0, \"negative_id_rows\": 0, \"prior_close_comparable\": 31473425, \"prior_close_not_comparable\": 576693, \"prior_close_residual_gt_1e_3\": 0, \"prior_close_residual_gt_1e_4\": 0, \"prior_close_residual_gt_1e_6\": 0, \"prior_close_residual_gt_1e_8\": 0, \"quarantined_positive_key_rows\": 2690.0, \"repeated_positive_keys\": 1345.0, \"reported_return_comparable\": 31461105, \"reported_return_not_comparable\": 589013, \"reported_return_residual_gt_1e_3\": 1, \"reported_return_residual_gt_1e_4\": 8, \"reported_return_residual_gt_1e_6\": 100, \"reported_return_residual_gt_1e_8\": 22742903, \"source_rows\": 32323644, \"zero_id_rows\": 272181}, \"provenance\": {\"adjusted_close_recipe\": \"finite_positive(close * cumulReturnFactor)\", \"availability_policy\": \"modeled tradingDate 22:00 backfill; not historical publication evidence\", \"duplicate_policy\": \"quarantine all repeated positive vendor-security/date keys\", \"economic","truncated":true} |
| bad_thirteenf_concentration_metric_rows | thirteenf_concentration_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_thirteenf_concentration_metric_keys | thirteenf_concentration_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| mismatched_thirteenf_concentration_ordering | thirteenf_concentration_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_thirteenf_option_metric_rows | thirteenf_option_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_thirteenf_option_metric_keys | thirteenf_option_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| mismatched_thirteenf_option_bias | thirteenf_option_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_thirteenf_position_metric_rows | thirteenf_position_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_thirteenf_position_metric_keys | thirteenf_position_metrics | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| priced_fundamental_universe_decision_coverage | universe_membership | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_universe_memberships | universe_memberships | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_universe_available_at | universe_memberships | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_universe_memberships | universe_memberships | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| universe_us_listed_missing_decile | universe_us_listed | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| universe_us_listed_overlapping_intervals | universe_us_listed | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| valuation_core_item_stub_detector | valuation_input_coverage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| valuation_input_core_completeness | valuation_input_coverage | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_valuation_multiple_rows | valuation_multiples | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_valuation_multiple_natural_keys | valuation_multiples | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| non_finite_valuation_multiple_values | valuation_multiples | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| stale_price_fundamental_gap_days | valuation_multiples | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| valuation_multiple_arithmetic_consistency | valuation_multiples | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| valuation_multiple_non_positive_denominator_meaningfulness | valuation_multiples | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_valuation_overlap_slice_rows | valuation_overlap_slice | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| activation_stage_runs_stuck_running | warehouse_activation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_dataset_watermarks | warehouse_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_core_dataset_watermarks | warehouse_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_field_catalog_entries | warehouse_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| missing_table_catalog_entries | warehouse_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_dataset_watermarks | warehouse_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_etl_job_retry_policy | warehouse_jobs | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_etl_job_run_retry_metadata | warehouse_jobs | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_lake_export_files | warehouse_lake_exports | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_lake_export_runs | warehouse_lake_exports | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_lake_export_files | warehouse_lake_exports | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| incomplete_lake_export_runs | warehouse_lake_exports | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_concept_catalog_rows | xbrl_concept_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_concept_catalog_keys | xbrl_concept_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| sec_company_fact_concepts_without_catalog | xbrl_concept_catalog | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_dimension_edges | xbrl_dimensions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| dimension_edges_without_relationship | xbrl_dimensions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_dimension_edges | xbrl_dimensions | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_fact_frames | xbrl_fact_frames | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_fact_frames | xbrl_fact_frames | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| xbrl_fact_frames_without_sec_company_facts | xbrl_fact_frames | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_filing_context_rows | xbrl_filing_contexts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_filing_dimension_rows | xbrl_filing_contexts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_filing_contexts | xbrl_filing_contexts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_filing_dimensions | xbrl_filing_contexts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| xbrl_filing_contexts_without_sec_submission | xbrl_filing_contexts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| xbrl_filing_dimensions_without_context | xbrl_filing_contexts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_filing_fact_rows | xbrl_filing_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_filing_facts | xbrl_filing_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| xbrl_filing_facts_without_context | xbrl_filing_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| xbrl_filing_facts_without_sec_submission | xbrl_filing_facts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_filing_taxonomy_edges | xbrl_filing_taxonomy_packages | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_latest_xbrl_filing_taxonomy_edges | xbrl_filing_taxonomy_packages | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_xbrl_filing_taxonomy_edges | xbrl_filing_taxonomy_packages | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_processor_finding_rows | xbrl_processor_findings | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| orphan_xbrl_processor_findings | xbrl_processor_findings | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_processor_run_rows | xbrl_processor_runs | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_latest_xbrl_processor_runs | xbrl_processor_runs | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_standard_taxonomy_package_revisions | xbrl_standard_taxonomy_packages | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_latest_xbrl_standard_taxonomy_packages | xbrl_standard_taxonomy_packages | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_taxonomy_packages | xbrl_taxonomy | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_taxonomy_relationships | xbrl_taxonomy | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_taxonomy_packages | xbrl_taxonomy | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_taxonomy_relationships | xbrl_taxonomy | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| observed_xbrl_concepts_without_taxonomy_relationships | xbrl_taxonomy | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_taxonomy_package_capture_attempts | xbrl_taxonomy_package_capture_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_latest_xbrl_taxonomy_package_capture_attempts | xbrl_taxonomy_package_capture_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| failed_latest_xbrl_taxonomy_package_capture_attempts | xbrl_taxonomy_package_capture_attempts | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| bad_xbrl_validation_result_rows | xbrl_validation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| duplicate_xbrl_validation_results | xbrl_validation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |
| failed_xbrl_calculation_linkbase_checks | xbrl_validation | unmeasured | unmeasured | not_run | unmeasured | unmeasured | unmeasured | unmeasured | {"value":null,"truncated":false} |

Quality report metadata:

```json
{
  "status": "recorded",
  "truncated": false,
  "row_limit": 1000,
  "returned_row_evidence_counts": {
    "not_run": 375,
    "newer_upstream_attempt_requires_review": 4
  },
  "registry_schema_gaps": null,
  "latest_activation_attempt": null,
  "newest_upstream_attempt": {
    "stage": "submissions_load",
    "run_id": "activation-source-prepass2",
    "status": "failed",
    "started_at": "2026-09-20T18:03:27.338560"
  },
  "note": "No quality checks executed. checked_at is wall-clock insertion; details.checked_at is the modeled observation clock. This table has no run_id, so even same-day passes cannot be certified against a particular rebuild. Skipped, not-run, stale and empty evidence never become passes."
}
```

## Measurement limits

- All SQL runs sequentially in one read-only snapshot; no migrations, activation, quality checks, schema flips or network access are performed.
- Missing relations/columns are unmeasured. Query errors and memory/spill failures abort with a nonzero exit; they are not converted into absent evidence.
- Variable-size results are capped at 1000 rows per query with explicit truncation flags. Diagnostic text is capped at 2048 characters per row. Email addresses in strings and sensitive keys in parsed structured objects are redacted; arbitrary secrets in plaintext or truncated JSON are not sanitized.
- Only aggregates and bounded control records cross into Python. COUNT DISTINCT and SQL scans still consume memory and time; the DuckDB setting is not a process-tree memory cap. Use the controller guard with no competing writer or heavy workload.
- Latest activation attempts, output run IDs and demonstrable dataset UUID lineage are reported separately. Unmatched lineage is unverified, not proof of staleness. No historical observation vintage is reconstructed.
- Recorded quality results have no run_id. Date agreement alone cannot associate a check with a rebuild or prove it covers current stored rows.
- Missing/undersized annual cohorts, empty quality or SLO results, and successful stage execution never certify parity. Historical listing, identity, adjustment and terminal-return completeness need independent evidence.
