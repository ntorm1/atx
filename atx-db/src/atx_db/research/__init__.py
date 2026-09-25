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

# R2a: research store and monthly point-in-time panel.
from .panel import (  # noqa: E402
    BASIS_RECONSTRUCTED,
    BASIS_STRICT,
    PanelFeature,
    ResearchPanelOptions,
    ResearchPanelResult,
    ResearchPanelValidation,
    build_research_panel,
    default_panel_features,
    expected_month_end_session,
    validate_research_panel,
)
from .store import ResearchStore, default_research_db_path, open_research_store  # noqa: E402

__all__ += [
    "BASIS_RECONSTRUCTED",
    "BASIS_STRICT",
    "PanelFeature",
    "ResearchPanelOptions",
    "ResearchPanelResult",
    "ResearchPanelValidation",
    "ResearchStore",
    "build_research_panel",
    "default_panel_features",
    "default_research_db_path",
    "expected_month_end_session",
    "open_research_store",
    "validate_research_panel",
]
