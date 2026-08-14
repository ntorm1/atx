from __future__ import annotations

import pytest

from atx_factor.cli import _build_parser


def test_evaluate_candidate_accepts_summary_only_output_mode() -> None:
    args = _build_parser().parse_args(
        [
            "evaluate-candidate",
            "--db-path",
            "warehouse.duckdb",
            "--candidate",
            "candidate",
            "--summary-only",
        ]
    )
    assert args.summary_only is True


def test_evaluate_replacement_accepts_summary_only_output_mode() -> None:
    args = _build_parser().parse_args(
        [
            "evaluate-replacement",
            "--db-path",
            "warehouse.duckdb",
            "--challenger",
            "challenger",
            "--summary-only",
        ]
    )
    assert args.summary_only is True
    assert args.aum_usd == 50_000_000.0


def test_explore_candidate_defaults_to_bounded_holdout_and_trial_ledger() -> None:
    args = _build_parser().parse_args(
        [
            "explore-candidate",
            "--db-path",
            "warehouse.duckdb",
            "--candidate",
            "candidate",
            "--summary-only",
        ]
    )
    assert args.selection_fraction == 0.60
    assert args.minimum_validation_periods == 48
    assert args.historical_trial_count == 66
    assert args.aum_usd == 50_000_000.0
    assert str(args.shadow_registry).endswith("shadow-alpha-registry.json")


def test_explore_candidate_rejects_overlapping_horizon_before_execution() -> None:
    with pytest.raises(SystemExit):
        _build_parser().parse_args(
            [
                "explore-candidate",
                "--db-path",
                "warehouse.duckdb",
                "--candidate",
                "candidate",
                "--horizon-days",
                "63",
            ]
        )
