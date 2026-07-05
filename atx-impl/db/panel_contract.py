"""PF3-S2 S2-3: forward contract for the future factor-panel export.

This module deliberately declares only the export shape. It does not import or require
``v_factor_panel``; PF3-S10 will materialize and enforce the panel against this data.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import Sequence

from .schema_contract import SIGN_VALUES

PANEL_PIT_KEYS = ("security_id", "as_of_date")


@dataclass(frozen=True)
class PanelColumnSpec:
    """One declared column in the future factor-panel export surface."""

    name: str
    data_type: str
    unit: str
    sign: str
    scale: str
    is_panel_key: bool = False
    key_ordinal: int | None = None

    def __post_init__(self) -> None:
        if self.sign not in SIGN_VALUES:
            raise ValueError(f"sign must be one of {sorted(SIGN_VALUES)}, got {self.sign!r}")
        if self.is_panel_key and self.key_ordinal is None:
            raise ValueError("panel key columns must declare key_ordinal")
        if not self.is_panel_key and self.key_ordinal is not None:
            raise ValueError("non-key panel columns must not declare key_ordinal")


PANEL_CONTRACT: tuple[PanelColumnSpec, ...] = (
    PanelColumnSpec(
        "security_id",
        "VARCHAR",
        unit="identifier",
        sign="bounded",
        scale="nominal",
        is_panel_key=True,
        key_ordinal=1,
    ),
    PanelColumnSpec(
        "as_of_date",
        "DATE",
        unit="date",
        sign="bounded",
        scale="day",
        is_panel_key=True,
        key_ordinal=2,
    ),
    PanelColumnSpec("factor_id", "VARCHAR", unit="identifier", sign="bounded", scale="nominal"),
    PanelColumnSpec("value", "DOUBLE", unit="dimensionless", sign="signed", scale="1"),
    PanelColumnSpec("available_at", "TIMESTAMP", unit="timestamp", sign="bounded", scale="second"),
    PanelColumnSpec("source_loaded_at", "TIMESTAMP", unit="timestamp", sign="bounded", scale="second"),
    PanelColumnSpec("run_id", "VARCHAR", unit="identifier", sign="bounded", scale="nominal"),
    PanelColumnSpec("input_lineage_json", "VARCHAR", unit="json", sign="bounded", scale="nominal"),
)


def _panel_contract_payload(contract: Sequence[PanelColumnSpec]) -> list[dict[str, object]]:
    return [
        {
            "name": spec.name,
            "data_type": spec.data_type,
            "unit": spec.unit,
            "sign": spec.sign,
            "scale": spec.scale,
            "is_panel_key": spec.is_panel_key,
            "key_ordinal": spec.key_ordinal,
        }
        for spec in sorted(contract, key=lambda item: item.name)
    ]


def panel_contract_sha256(contract: Sequence[PanelColumnSpec] | None = None) -> str:
    """Stable hash over the declared panel export contract."""

    resolved = PANEL_CONTRACT if contract is None else tuple(contract)
    payload = json.dumps(_panel_contract_payload(resolved), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


PANEL_CONTRACT_SHA256 = panel_contract_sha256()
