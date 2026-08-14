"""Durable pre-result ledger for honest multiple-testing trial counts."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path

from .registry import atomic_write_text


class ResearchTrialLedger:
    """Commit a research grid before evaluation and expose its cumulative trial count."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)

    def load(self) -> dict[str, object]:
        if not self.path.exists():
            return {"schema_version": 1, "historical_trial_count": 0, "experiments": []}
        payload = json.loads(self.path.read_text(encoding="utf-8"))
        if (
            payload.get("schema_version") != 1
            or not isinstance(payload.get("historical_trial_count"), int)
            or not isinstance(payload.get("experiments"), list)
        ):
            raise ValueError("unsupported or malformed research trial ledger")
        return payload

    def commit(
        self,
        spec: dict[str, object],
        *,
        configuration_trials: int,
        historical_trial_count: int = 0,
    ) -> tuple[dict[str, object], int]:
        """Idempotently commit a grid and return its record and total trial count."""

        if configuration_trials < 1:
            raise ValueError("configuration_trials must be positive")
        if historical_trial_count < 0:
            raise ValueError("historical_trial_count cannot be negative")
        canonical = json.dumps(spec, sort_keys=True, separators=(",", ":"), allow_nan=False)
        spec_sha256 = hashlib.sha256(canonical.encode()).hexdigest()
        payload = self.load()
        existing_historical = int(payload["historical_trial_count"])
        experiments = list(payload["experiments"])
        if not experiments and existing_historical == 0:
            existing_historical = historical_trial_count
        elif existing_historical != historical_trial_count:
            raise ValueError(
                "historical_trial_count disagrees with the initialized trial ledger"
            )
        for item in experiments:
            if item.get("spec_sha256") == spec_sha256:
                if int(item["configuration_trials"]) != configuration_trials:
                    raise ValueError("committed spec trial count cannot be changed")
                return item, existing_historical + sum(
                    int(entry["configuration_trials"]) for entry in experiments
                )
        record: dict[str, object] = {
            "spec_sha256": spec_sha256,
            "committed_at": dt.datetime.now(dt.UTC).isoformat(),
            "configuration_trials": configuration_trials,
            "spec": spec,
        }
        experiments.append(record)
        updated: dict[str, object] = {
            "schema_version": 1,
            "historical_trial_count": existing_historical,
            "updated_at": dt.datetime.now(dt.UTC).isoformat(),
            "experiments": experiments,
        }
        atomic_write_text(
            self.path,
            json.dumps(updated, sort_keys=True, indent=2, allow_nan=False) + "\n",
        )
        return record, existing_historical + sum(
            int(entry["configuration_trials"]) for entry in experiments
        )
