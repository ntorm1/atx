"""atx-engine/tools modules for atx-impl/tools, without an installed package (platform v8 W0-1).

``from engine_tools import research_window as rw`` gives the research window (TRAIN and the seal) read from
``atx-impl/strategies/research_window.json`` by ``atx-engine/tools/research_window.py``.

The module is loaded from its file under a private name, so it is this directory's own instance: nothing here puts
``atx-engine/tools`` on ``sys.path``, and a test harness that rebinds the engine tools' instance (the
``atx-engine/tools/conftest.py`` of the field-builder fixtures) cannot change the window seen by atx-impl tools.
Import the window only through this module.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ENGINE_TOOLS = Path(__file__).resolve().parents[2] / "atx-engine" / "tools"


def _load(name: str):
    """``atx-engine/tools/<name>.py`` as the private module ``atx_impl_engine_tools_<name>`` (loaded once)."""
    private = f"atx_impl_engine_tools_{name}"
    if private in sys.modules:
        return sys.modules[private]
    spec = importlib.util.spec_from_file_location(private, ENGINE_TOOLS / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {ENGINE_TOOLS / f'{name}.py'}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[private] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        del sys.modules[private]
        raise
    return module


research_window = _load("research_window")
