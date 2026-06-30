"""Test that seed_default_jobs() + enabled_job_order() produces a valid topological order."""

from __future__ import annotations


def test_seed_default_jobs_succeeds(tmp_store):
    """seed_default_jobs should register jobs without raising."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    count = tmp_store.con.execute("SELECT count(*) FROM etl_job_definitions").fetchone()[0]
    assert count > 0, "seed_default_jobs() registered no jobs"


def test_enabled_job_order_is_topologically_valid(tmp_store):
    """Every job in enabled_job_order() must appear after all its dependencies."""
    from db.jobs import JobManager, normalize_dependencies

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert len(order) > 0, "enabled_job_order() returned empty list"

    # Build dependency map
    rows = tmp_store.con.execute(
        "SELECT job_name, dependencies_json FROM etl_job_definitions WHERE enabled"
    ).fetchall()
    deps = {job_name: normalize_dependencies(deps_json) for job_name, deps_json in rows}

    # For every job in order, all its dependencies must appear before it
    seen: set[str] = set()
    for job_name in order:
        for dep in deps.get(job_name, []):
            assert dep in seen, (
                f"Job {job_name!r} appears before its dependency {dep!r} in the order"
            )
        seen.add(job_name)


def test_enabled_job_order_contains_no_cycle(tmp_store):
    """enabled_job_order() must not raise a cycle error."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    # If there is a cycle, enabled_job_order() raises RuntimeError
    order = mgr.enabled_job_order()
    assert isinstance(order, list)


def test_enabled_job_order_no_duplicates(tmp_store):
    """Each job should appear exactly once in the order."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert len(order) == len(set(order)), f"Duplicate jobs in order: {order}"


def test_default_jobs_include_shares_after_sec_company_facts(tmp_store):
    """Share-count history depends on normalized SEC statement points."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert "sec_company_facts" in order
    assert "shares_outstanding_history" in order
    assert order.index("sec_company_facts") < order.index("shares_outstanding_history")


def test_default_jobs_include_adjustment_factors_after_corporate_actions(tmp_store):
    """Adjustment factors are derived from normalized corporate-action events."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert "daily_bars" in order
    assert "corporate_actions" in order
    assert "adjustment_factor_history" in order
    assert "daily_adjustment_factors" in order
    assert order.index("daily_bars") < order.index("corporate_actions")
    assert order.index("corporate_actions") < order.index("adjustment_factor_history")
    assert order.index("adjustment_factor_history") < order.index("daily_adjustment_factors")
    assert "corporate_action_split_metrics" in order
    assert order.index("daily_adjustment_factors") < order.index("corporate_action_split_metrics")
    assert "corporate_action_factor_reconciliation" in order
    assert order.index("daily_adjustment_factors") < order.index("corporate_action_factor_reconciliation")


def test_default_jobs_include_thirteenf_option_metrics_after_ownership(tmp_store):
    """13F option metrics are derived from cached security-position rows."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert "sec_13f_ownership_features" in order
    assert "thirteenf_option_metrics" in order
    assert "thirteenf_concentration_metrics" in order
    assert order.index("sec_13f_ownership_features") < order.index("thirteenf_option_metrics")
    assert order.index("sec_13f_ownership_features") < order.index("thirteenf_concentration_metrics")


def test_default_jobs_include_short_volume_metrics_after_raw_flow(tmp_store):
    """Daily short-volume metrics are derived from injected FINRA short-flow rows."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert "finra_short_volume" in order
    assert "short_volume_metrics" in order
    assert order.index("security_master") < order.index("finra_short_volume")
    assert order.index("finra_short_volume") < order.index("short_volume_metrics")


def test_default_jobs_include_offexchange_quality_report_after_public_flow_inputs(tmp_store):
    """The off-exchange quality report waits for both OTC and short-flow surfaces."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert "offexchange_security_period" in order
    assert "short_volume_metrics" in order
    assert "offexchange_quality_report" in order
    assert order.index("offexchange_security_period") < order.index("offexchange_quality_report")
    assert order.index("short_volume_metrics") < order.index("offexchange_quality_report")


def test_default_jobs_include_delistings_after_listing_status(tmp_store):
    """Delisting proxy events are derived from listing-status intervals."""
    from db.jobs import JobManager

    mgr = JobManager(tmp_store)
    mgr.seed_default_jobs()

    order = mgr.enabled_job_order()
    assert "nasdaq_listing_events" in order
    assert "listing_status_intervals" in order
    assert "delisting_events" in order
    assert order.index("nasdaq_listing_events") < order.index("listing_status_intervals")
    assert order.index("listing_status_intervals") < order.index("delisting_events")
