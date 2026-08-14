"""Atomic JSON registry for accepted mega-alpha constituents."""

from __future__ import annotations

import datetime as dt
import json
import os
import tempfile
from pathlib import Path

from .mega_alpha import CandidateDecision


def atomic_write_text(path: str | Path, serialized: str) -> None:
    """Durably replace a text artifact without exposing a partial file."""

    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{target.name}.",
        suffix=".tmp",
        dir=target.parent,
        text=True,
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(serialized)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, target)
    finally:
        if os.path.exists(temporary_name):
            os.unlink(temporary_name)


class MegaAlphaRegistry:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)

    def load(self) -> dict[str, object]:
        if not self.path.exists():
            return {"schema_version": 1, "signals": []}
        payload = json.loads(self.path.read_text(encoding="utf-8"))
        if payload.get("schema_version") != 1 or not isinstance(payload.get("signals"), list):
            raise ValueError("unsupported or malformed mega-alpha registry")
        return payload

    def admit(self, decision: CandidateDecision, *, allocation: float) -> dict[str, object]:
        if not decision.accepted:
            raise ValueError("cannot admit a rejected candidate")
        if not 0 < allocation <= 1:
            raise ValueError("allocation must be in (0, 1]")
        payload = self.load()
        signals = [
            signal
            for signal in payload["signals"]
            if signal.get("factor_id") != decision.candidate_id
        ]
        signals.append(
            {
                "factor_id": decision.candidate_id,
                "allocation": allocation,
                "allocation_semantics": (
                    "score_weight"
                    if getattr(decision, "selected_combination_mode", "sleeve_mix")
                    == "integrated_rank"
                    else "capital_weight"
                ),
                "baseline_id": decision.baseline_id,
                "selected_variant": getattr(
                    decision, "selected_variant", "externally_frozen"
                ),
                "selected_combination_mode": getattr(
                    decision, "selected_combination_mode", "sleeve_mix"
                ),
                "integrated_score_model": getattr(
                    decision, "integrated_score_model", None
                ),
                "evidence_sha256": decision.evidence_sha256,
                "accepted_at": dt.datetime.now(dt.UTC).isoformat(),
            }
        )
        payload = {
            "schema_version": 1,
            "updated_at": dt.datetime.now(dt.UTC).isoformat(),
            "signals": sorted(signals, key=lambda item: item["factor_id"]),
        }
        self._atomic_write(payload)
        return payload

    def _atomic_write(self, payload: dict[str, object]) -> None:
        serialized = json.dumps(payload, sort_keys=True, indent=2) + "\n"
        atomic_write_text(self.path, serialized)


class ShadowAlphaRegistry:
    """Atomic watchlist for economically valid candidates awaiting more OOS evidence."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)

    def load(self) -> dict[str, object]:
        if not self.path.exists():
            return {"schema_version": 2, "candidates": [], "revocations": []}
        payload = json.loads(self.path.read_text(encoding="utf-8"))
        version = payload.get("schema_version")
        if version not in {1, 2} or not isinstance(payload.get("candidates"), list):
            raise ValueError("unsupported or malformed shadow-alpha registry")
        if version == 1:
            payload = {**payload, "schema_version": 2, "revocations": []}
        if not isinstance(payload.get("revocations"), list):
            raise ValueError("unsupported or malformed shadow-alpha registry")
        return payload

    def admit(self, decision: object) -> dict[str, object]:
        """Upsert a shadow decision without granting production allocation."""

        if hasattr(decision, "to_dict"):
            decision_payload = decision.to_dict()
        elif isinstance(decision, dict):
            decision_payload = decision
        else:
            raise TypeError("shadow decision must be a decision object or dictionary")
        if decision_payload.get("disposition") != "shadow":
            raise ValueError("only shadow-eligible decisions can enter the shadow registry")
        candidate_id = str(decision_payload["candidate_id"])
        payload = self.load()
        candidates = [
            item
            for item in payload["candidates"]
            if item.get("factor_id") != candidate_id
        ]
        candidate_metrics = decision_payload["validation_candidate_metrics"]
        combined_metrics = decision_payload["validation_combined_metrics"]
        signal_breadth = decision_payload["validation_candidate_breadth"]
        portfolio_breadth = decision_payload[
            "validation_candidate_portfolio_breadth"
        ]
        candidates.append(
            {
                "factor_id": candidate_id,
                "baseline_id": decision_payload["baseline_id"],
                "selected_variant": decision_payload["selected_variant"],
                "selected_allocation": decision_payload["selected_allocation"],
                "selected_combination_mode": decision_payload.get(
                    "selected_combination_mode", "sleeve_mix"
                ),
                "validation_start": decision_payload["validation_start"],
                "candidate_sharpe": candidate_metrics["sharpe"],
                "candidate_probabilistic_sharpe": candidate_metrics[
                    "probabilistic_sharpe"
                ],
                "candidate_deflated_sharpe_probability": candidate_metrics[
                    "deflated_sharpe_probability"
                ],
                "combined_sharpe": combined_metrics["sharpe"],
                "marginal_sharpe": decision_payload["validation_marginal_sharpe"],
                "positive_fold_fraction": decision_payload["positive_fold_fraction"],
                "median_signal_breadth": signal_breadth["median_names"],
                "median_effective_breadth": portfolio_breadth[
                    "median_effective_breadth"
                ],
                "committed_trials": decision_payload["committed_trials"],
                "evidence_sha256": decision_payload["evidence_sha256"],
                "promotion_policy": (
                    "rerun the frozen construction only after new untouched formation dates; "
                    "promotion still requires every production gate, including 1,000-name "
                    "median signal breadth"
                ),
                "shadowed_at": dt.datetime.now(dt.UTC).isoformat(),
            }
        )
        updated: dict[str, object] = {
            "schema_version": 2,
            "updated_at": dt.datetime.now(dt.UTC).isoformat(),
            "candidates": sorted(candidates, key=lambda item: item["factor_id"]),
            "revocations": payload["revocations"],
        }
        atomic_write_text(
            self.path,
            json.dumps(updated, sort_keys=True, indent=2, allow_nan=False) + "\n",
        )
        return updated

    def revoke(
        self,
        factor_id: str,
        *,
        reason: str,
        superseding_evidence_sha256: str,
    ) -> dict[str, object]:
        """Remove a shadow candidate while retaining an auditable tombstone."""

        if not factor_id or not reason:
            raise ValueError("factor_id and reason are required")
        if len(superseding_evidence_sha256) != 64:
            raise ValueError("superseding evidence SHA-256 must contain 64 characters")
        payload = self.load()
        matches = [
            item for item in payload["candidates"] if item.get("factor_id") == factor_id
        ]
        if not matches:
            if any(
                item.get("factor_id") == factor_id
                for item in payload["revocations"]
            ):
                return payload
            raise KeyError(f"shadow candidate not found: {factor_id}")
        retained = [
            item for item in payload["candidates"] if item.get("factor_id") != factor_id
        ]
        revocations = [
            item for item in payload["revocations"] if item.get("factor_id") != factor_id
        ]
        original = matches[-1]
        revocations.append(
            {
                "factor_id": factor_id,
                "reason": reason,
                "original_evidence_sha256": original.get("evidence_sha256"),
                "superseding_evidence_sha256": superseding_evidence_sha256,
                "revoked_at": dt.datetime.now(dt.UTC).isoformat(),
                "original_candidate": original,
            }
        )
        updated: dict[str, object] = {
            "schema_version": 2,
            "updated_at": dt.datetime.now(dt.UTC).isoformat(),
            "candidates": sorted(retained, key=lambda item: item["factor_id"]),
            "revocations": sorted(revocations, key=lambda item: item["factor_id"]),
        }
        atomic_write_text(
            self.path,
            json.dumps(updated, sort_keys=True, indent=2, allow_nan=False) + "\n",
        )
        return updated
