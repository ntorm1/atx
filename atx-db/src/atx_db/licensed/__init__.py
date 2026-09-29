"""Licensed-data adapters (tier1-v3 S8; ruling D3: buy nothing now, ship contracts, mocks and free substitutes).

``ADAPTERS`` maps adapter name -> module path; ``get(name)`` imports it and returns its ``ADAPTER`` instance. Every
adapter implements ``contract.Adapter``: ``load(raw_dir) -> Stage``, ``mock(raw_dir)``, ``validate(tables)`` and
``substitute()``. See ``docs/LICENSED_ADAPTERS.md`` for each PIT rule and the purchase decision points.
"""

from __future__ import annotations

from importlib import import_module
from typing import Any

ADAPTERS = {
    "estimates": "atx_db.licensed.estimates",
    "lending": "atx_db.licensed.lending",
    "gics": "atx_db.licensed.gics",
    "indexes": "atx_db.licensed.indexes",
    "transcripts": "atx_db.licensed.transcripts",
}


def get(name: str) -> Any:
    return import_module(ADAPTERS[name]).ADAPTER
