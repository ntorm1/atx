"""Production dispatch, bounded cohort and honest empty-input integration."""

from __future__ import annotations

import argparse
import datetime as dt
from types import SimpleNamespace

import pytest

from atx_db import activation
from atx_db.derived_factor_projection import FactorProjectionOptions
from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY, JobManager
from atx_db.production_panels import DerivedFactorProjectionDataset, ProductionPanelOptions


def test_safe_defaults_native_source_and_invalid_batch(tmp_path):
    parser = argparse.ArgumentParser()
    activation.add_activation_arguments(parser)
    options = activation.activation_options_from_args(parser.parse_args([
        "--as-of-date", "2026-09-20", "--ticker-history-source-path", str(tmp_path / "bars.parquet"),
    ]))
    assert (options.memory_limit, options.threads, options.reconciliation_shards, options.submissions_batch_size) == ("1GB", 1, 16, 50)
    assert options.ticker_history_source_path.name == "bars.parquet"
    with pytest.raises(ValueError, match="submissions_batch_size"):
        activation.ActivationOptions(submissions_batch_size=0)


def test_native_source_bypasses_zip_and_dispatches_same_path(tmp_path, monkeypatch):
    path = tmp_path / "bars.parquet"
    path.write_bytes(b"PAR1")
    options = activation.ActivationOptions(ticker_history_source_path=path)
    monkeypatch.setattr(activation, "extract_ticker_history_tsv", lambda *a, **k: pytest.fail("ZIP extraction"))
    assert activation.stage_ticker_history_extract(None, options).detail["skipped"]
    captured = []
    def publish(store, supplied):
        captured.append(supplied)
        return SimpleNamespace(rows=5, securities=2, latest_date=dt.date(2026, 9, 18),
                               latest_date_securities=2, invalid_rows=0, duplicate_keys=0,
                               elapsed_seconds=0, source_diagnostics={}, provenance={})
    monkeypatch.setattr(activation, "publish_bulk_ticker_history", publish)
    assert activation.stage_ticker_history_publish(None, options).rows == 5
    assert captured[0].input_path == path.resolve()


def test_activation_all_forms_uses_small_issuer_batch(tmp_path, monkeypatch):
    from atx_db.sec_submissions import SecSubmissionsBulkDataset
    options = activation.ActivationOptions(cache_dir=tmp_path)
    options.submissions_zip.write_bytes(b"offline fixture")
    captured = []
    def run(self, store, supplied):
        captured.append(supplied)
        return SimpleNamespace(rows_loaded=6, details={})
    monkeypatch.setattr(SecSubmissionsBulkDataset, "run", run)
    result = activation.stage_submissions_load(None, options)
    assert captured[0].forms is None and captured[0].batch_ciks == 50
    assert result.detail == {"forms": "all", "batch_size": 50}


def test_empty_projection_cohort_preserves_existing_rows_and_never_dispatches_parents(tmp_store, monkeypatch):
    import atx_db.production_panels as panels
    monkeypatch.setattr(panels, "refresh_compatibility_parents", lambda *a: pytest.fail("empty parent dispatch"))
    monkeypatch.setattr(panels, "refresh_projected_factor_values", lambda *a: pytest.fail("empty destructive projection"))
    result = DerivedFactorProjectionDataset().load(tmp_store, FactorProjectionOptions())
    assert result.rows_loaded == 0 and result.details["outcome"] == "degraded"
    assert result.details["previous_projection_rows_preserved"]
    assert result.details["selected_factor_count"] == 23


def test_same_projection_adapter_is_scheduled_and_activated(tmp_store, monkeypatch):
    from atx_db.dataset import DatasetLoadResult
    captured = []
    def load(self, store, options):
        captured.append(options)
        return DatasetLoadResult(self.dataset_id, 23, options.source, {"outcome": "measured"})
    monkeypatch.setattr(DerivedFactorProjectionDataset, "load", load)
    options = activation.ActivationOptions(as_of_date=dt.date(2026, 9, 20))
    assert activation.stage_factor_projections(tmp_store, options).rows == 23
    cls, factory = DATASET_REGISTRY["derived_factor_projection"]
    cls().load(tmp_store, factory({"as_of_date": "2026-09-20"}))
    assert captured[0].end_date == captured[1].end_date == dt.date(2026, 9, 20)
    assert captured[0].universe_id == captured[1].universe_id == "us_common_equity_liquid_v1"
    assert factory({"factor_ids": []}).factor_ids == ()


def test_nonempty_projection_keeps_complete_peers_in_sequential_year_scopes(tmp_store, monkeypatch):
    import atx_db.production_panels as panels
    from atx_db.derived_compatibility_parents import CompatibilityParentResult
    for year in (2024, 2025):
        day = dt.date(year, 1, 31)
        tmp_store.con.execute("""
            INSERT INTO equity_daily_bars(source,security_id,symbol,trade_date,close,volume,available_at)
            SELECT 'fixture','S'||i,'S'||i,?,10,1000000,? FROM range(24) t(i)
        """, [day, dt.datetime.combine(day, dt.time(22))])
        tmp_store.con.execute("""
            INSERT INTO universe_membership(universe_id,security_id,symbol,valid_from,valid_to,as_of_date,
                is_member,reason,rules_json,decision_count,available_at,source)
            SELECT 'us_common_equity_liquid_v1','S'||i,'S'||i,?,?,?,true,'member','{}',1,?,'fixture'
            FROM range(24) t(i)
        """, [day, day, day, dt.datetime.combine(day, dt.time(22))])
    calls = []
    def parents(store, options):
        calls.append(("parents", options.start_date, options.end_date))
        return CompatibilityParentResult({"asset_growth": 24}, 23, options.start_date)
    def projection(store, options):
        calls.append(("projection", options.start_date, options.end_date))
        return 24
    monkeypatch.setattr(panels, "refresh_compatibility_parents", parents)
    monkeypatch.setattr(panels, "refresh_projected_factor_values", projection)
    result = DerivedFactorProjectionDataset().load(tmp_store, FactorProjectionOptions(end_date=dt.date(2025, 6, 1)))
    assert calls == [("parents", dt.date(2024, 1, 1), dt.date(2024, 12, 31)),
                     ("projection", dt.date(2024, 1, 1), dt.date(2024, 12, 31)),
                     ("parents", dt.date(2025, 1, 1), dt.date(2025, 6, 1)),
                     ("projection", dt.date(2025, 1, 1), dt.date(2025, 6, 1))]
    assert result.rows_loaded == 48
    assert result.details["parent_rows_written"] == {"asset_growth": 48}
    assert result.details["eligible_security_dates"] == 48
    assert set(result.details["insufficient_peer_dates"].values()) == {0}


def test_seeded_production_jobs_and_migration_metadata(tmp_store):
    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()
    order = mgr.enabled_job_order()
    assert order.index("legacy_liquid_universe") < order.index("factor_projections")
    assert order.index("delisting_terminal_returns") < order.index("survivorship_forward_returns")
    assert "universe_membership" in DATASET_DEPENDENCIES["derived_factor_projection"]
    description = tmp_store.con.execute("SELECT description FROM dataset_catalog WHERE dataset_id='forward_returns_survivorship_safe'").fetchone()[0]
    assert "adjusted_close" in description and "named-policy" in description
    assert tmp_store.con.execute("SELECT count(*) FROM schema_migrations WHERE try_cast(version AS INTEGER)=312").fetchone()[0] == 1


def test_source_diagnostics_do_not_certify_empty_listing_history(tmp_store):
    detail = activation.listing_input_diagnostics(tmp_store, activation.ActivationOptions(as_of_date=dt.date(2026, 9, 20)))
    assert detail["historical_listing_prerequisite"] == "missing_or_partial"
    assert detail["bar_observed_names_are_verified_us_common_equity"] is False


def test_terminal_forward_coverage_stages_use_explicit_date_and_adjusted_basis(tmp_store, monkeypatch):
    import atx_db.production_panels as panels
    captured = []
    monkeypatch.setattr(panels, "refresh_survivorship_safe_forward_returns", lambda store, options: captured.append(options) or 7)
    monkeypatch.setattr(panels, "survivorship_forward_return_diagnostics", lambda store, options: {"rows": 7})
    options = activation.ActivationOptions(as_of_date=dt.date(2026, 9, 20))
    assert activation.stage_survivorship_forward_returns(tmp_store, options).rows == 7
    assert captured[0].price_basis == "adjusted_close"
    assert captured[0].observation_cutoff == dt.datetime(2026, 9, 20, 22)
    assert activation.stage_delisting_terminal_returns(tmp_store, options).detail["uncovered_by_reason"] == {}
    assert panels.ProductionPanelOptions is ProductionPanelOptions


def test_quality_stage_passes_explicit_clock_and_reports_failures(tmp_store, monkeypatch):
    import atx_db.quality as quality
    captured = []
    def checks(store, *, checked_at):
        captured.append(checked_at)
        return [SimpleNamespace(status="failed", check_name="coverage", severity="error", observed_value=0, threshold_value=110)]
    monkeypatch.setattr(quality, "run_warehouse_quality_checks", checks)
    result = activation.stage_quality(tmp_store, activation.ActivationOptions(as_of_date=dt.date(2026, 9, 20)))
    assert captured == [dt.datetime(2026, 9, 20, 22)]
    assert result.detail["outcome"] == "degraded" and result.detail["counts"]["failed"] == 1


def test_governed_migration_connections_are_capped_before_schema_work(tmp_store, monkeypatch, tmp_path):
    from atx_db.connection import DuckDBStore
    from atx_db.migration_admin import run_governed_migrations
    captured = []
    original = DuckDBStore._configure_session
    def configure(self, con):
        captured.append(con.execute("SELECT current_setting('memory_limit'),current_setting('threads'),current_setting('preserve_insertion_order')").fetchone())
        return original(self, con)
    tmp_store.close()
    monkeypatch.setattr(DuckDBStore, "_configure_session", configure)
    run_governed_migrations(tmp_store.path, backup_dir=tmp_path / "backup")
    assert len(captured) == 2
    # 06ab073f lowered the governed-migration startup budget from 1GB to 512MB (= 488.2 MiB).
    assert all(row == ("488.2 MiB", 1, False) for row in captured)
