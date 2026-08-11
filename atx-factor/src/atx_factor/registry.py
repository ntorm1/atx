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
                "baseline_id": decision.baseline_id,
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
