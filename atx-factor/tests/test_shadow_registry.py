from __future__ import annotations

import pytest

from atx_factor.registry import ShadowAlphaRegistry


def _decision(*, disposition: str = "shadow") -> dict[str, object]:
    return {
        "candidate_id": "quality_net_operating_assets",
        "baseline_id": "router",
        "disposition": disposition,
        "selected_variant": "top_quintile_vs_universe",
        "selected_allocation": 0.2,
        "selected_combination_mode": "integrated_rank",
        "validation_start": "2020-10-30",
        "validation_candidate_metrics": {
            "sharpe": 0.67,
            "probabilistic_sharpe": 0.97,
            "deflated_sharpe_probability": 0.12,
        },
        "validation_combined_metrics": {"sharpe": 0.65},
        "validation_marginal_sharpe": 0.22,
        "positive_fold_fraction": 2 / 3,
        "validation_candidate_breadth": {"median_names": 750.0},
        "validation_candidate_portfolio_breadth": {
            "median_effective_breadth": 150.0
        },
        "committed_trials": 108,
        "evidence_sha256": "a" * 64,
    }


def test_shadow_registry_atomically_upserts_without_production_allocation(tmp_path) -> None:
    registry = ShadowAlphaRegistry(tmp_path / "shadow.json")
    registry.admit(_decision())
    registry.admit(_decision())
    payload = registry.load()
    assert len(payload["candidates"]) == 1
    candidate = payload["candidates"][0]
    assert candidate["factor_id"] == "quality_net_operating_assets"
    assert candidate["selected_variant"] == "top_quintile_vs_universe"
    assert "allocation" not in candidate
    assert candidate["selected_allocation"] == pytest.approx(0.2)
    assert candidate["selected_combination_mode"] == "integrated_rank"
    assert candidate["median_signal_breadth"] == pytest.approx(750.0)


def test_shadow_registry_rejects_non_shadow_decisions(tmp_path) -> None:
    registry = ShadowAlphaRegistry(tmp_path / "shadow.json")
    with pytest.raises(ValueError, match="only shadow-eligible"):
        registry.admit(_decision(disposition="accepted"))


def test_shadow_registry_revokes_with_auditable_tombstone(tmp_path) -> None:
    registry = ShadowAlphaRegistry(tmp_path / "shadow.json")
    registry.admit(_decision())
    payload = registry.revoke(
        "quality_net_operating_assets",
        reason="selection_no_feasible_construction",
        superseding_evidence_sha256="b" * 64,
    )
    assert payload["schema_version"] == 2
    assert payload["candidates"] == []
    assert payload["revocations"][0]["factor_id"] == "quality_net_operating_assets"
    assert payload["revocations"][0]["original_evidence_sha256"] == "a" * 64
