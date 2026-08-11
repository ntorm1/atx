from __future__ import annotations

import json

import pytest

from atx_factor.mega_alpha import CandidateDecision
from atx_factor.registry import MegaAlphaRegistry


def _decision(*, accepted: bool) -> CandidateDecision:
    return CandidateDecision(
        candidate_id="candidate",
        baseline_id="baseline",
        accepted=accepted,
        rejection_reasons=() if accepted else ("failed_gate",),
        candidate_metrics={"sharpe": 1.0},
        baseline_metrics={"sharpe": 0.5},
        combined_metrics={"sharpe": 0.8},
        stressed_combined_metrics={"sharpe": 0.6},
        baseline_correlation=0.1,
        marginal_sharpe=0.3,
        gate={"candidate_allocation": 0.2},
        backtest_config={},
        walk_forward_config={},
        candidate_folds=(),
        baseline_folds=(),
        combined_folds=(),
        evidence_sha256="a" * 64,
    )


def test_registry_rejects_unaccepted_candidate(tmp_path) -> None:
    registry = MegaAlphaRegistry(tmp_path / "registry.json")
    with pytest.raises(ValueError, match="rejected candidate"):
        registry.admit(_decision(accepted=False), allocation=0.2)
    assert not registry.path.exists()


def test_registry_atomically_upserts_accepted_candidate(tmp_path) -> None:
    registry = MegaAlphaRegistry(tmp_path / "registry.json")
    registry.admit(_decision(accepted=True), allocation=0.2)
    registry.admit(_decision(accepted=True), allocation=0.3)
    payload = json.loads(registry.path.read_text(encoding="utf-8"))
    assert payload["schema_version"] == 1
    assert payload["signals"] == [
        {
            "factor_id": "candidate",
            "allocation": 0.3,
            "baseline_id": "baseline",
            "evidence_sha256": "a" * 64,
            "accepted_at": payload["signals"][0]["accepted_at"],
        }
    ]
    assert list(tmp_path.glob("*.tmp")) == []
