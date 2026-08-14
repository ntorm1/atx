"""Deterministic JSON evidence helpers for governed research decisions."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math


def json_safe(value: object) -> object:
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_safe(item) for item in value]
    return value


def evidence_digest(payload: dict[str, object]) -> str:
    serialized = json.dumps(
        json_safe(payload),
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )
    return hashlib.sha256(serialized.encode()).hexdigest()
