"""Research lane R: anomaly catalog, monthly PIT panels, statistics and qualification.

Outputs of this package live in the separate research store (ruling RX6), never in
the governed warehouse. Each lane appends its own exports below; never remove another
lane's lines.
"""

from __future__ import annotations

__all__: list[str] = []

# R1a: anomaly catalog.
from .catalog import (
    ANOMALY_CLASSES,
    CONTROL_CLASSES,
    AnomalyCatalogEntry,
    AnomalyCatalogError,
    anomaly_catalog_sha256,
    anomaly_class_counts,
    default_anomaly_catalog,
    load_anomaly_catalog,
)

__all__ += [
    "ANOMALY_CLASSES",
    "CONTROL_CLASSES",
    "AnomalyCatalogEntry",
    "AnomalyCatalogError",
    "anomaly_catalog_sha256",
    "anomaly_class_counts",
    "default_anomaly_catalog",
    "load_anomaly_catalog",
]
