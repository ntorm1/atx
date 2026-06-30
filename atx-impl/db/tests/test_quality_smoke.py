"""Smoke test: run_warehouse_quality_checks does not raise on an empty warehouse."""

from __future__ import annotations


def test_run_warehouse_quality_checks_does_not_raise(tmp_store):
    """run_warehouse_quality_checks should complete without raising on an empty DB.

    On an empty warehouse all checks will either:
    - skip (required tables missing) → status 'warning' or 'failed'
    - pass (count query returns 0 which satisfies threshold 0 with comparator 'eq')
    The critical guarantee is: no exception is raised.
    """
    from db.quality import run_warehouse_quality_checks

    results = run_warehouse_quality_checks(tmp_store, record=False)
    assert isinstance(results, list), "run_warehouse_quality_checks should return a list"


def test_quality_results_have_expected_structure(tmp_store):
    """Each QualityResult must have the required fields."""
    from db.quality import QualityResult, run_warehouse_quality_checks

    results = run_warehouse_quality_checks(tmp_store, record=False)
    assert len(results) > 0, "Expected at least one quality check result"

    for result in results:
        assert isinstance(result, QualityResult)
        assert result.dataset_id, "dataset_id must be non-empty"
        assert result.table_name, "table_name must be non-empty"
        assert result.check_name, "check_name must be non-empty"
        assert result.status in {"passed", "failed", "warning"}, (
            f"Unexpected status {result.status!r} for check {result.check_name!r}"
        )


def test_overlay_statement_map_rows_are_not_flagged_bad(tmp_store):
    """Bank/insurance/REIT overlay statement types are valid, not bad rows.

    The seeded fundamental_statement_map carries bank_statement /
    insurance_statement / reit_statement overlay rows (S4) plus a `quantity`
    unit type; the bad-row quality check allowlist must accept them.
    """
    from db.quality import run_warehouse_quality_checks

    results = {r.check_name: r for r in run_warehouse_quality_checks(tmp_store, record=False)}
    bad = results["bad_fundamental_statement_map_rows"]
    assert bad.status == "passed", f"observed bad rows = {bad.observed_value}"


def test_quality_checks_record_to_db(tmp_store):
    """With record=True, run_warehouse_quality_checks writes rows to data_quality_checks."""
    from db.quality import run_warehouse_quality_checks

    run_warehouse_quality_checks(tmp_store, record=True)
    count = tmp_store.con.execute("SELECT count(*) FROM data_quality_checks").fetchone()[0]
    assert count > 0, "data_quality_checks should have rows after record=True run"
