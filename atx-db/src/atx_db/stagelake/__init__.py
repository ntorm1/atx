"""Parquet stage lake platform: registry, invariant checker, catalog and orchestrator (docs/LAKE.md).

``python -m atx_db.lake {list,verify,catalog,plan,run}``; stage code keeps using ``atx_db.alpha_panel.common``
(``stage_dir``, ``connect``, ``copy_to_parquet``, ``write_stage_manifest``) plus :func:`bind_inputs` here.
"""

from __future__ import annotations

from .contract import Output, RegistryError, Stage, bind_inputs

__all__ = ["Output", "RegistryError", "Stage", "bind_inputs"]
