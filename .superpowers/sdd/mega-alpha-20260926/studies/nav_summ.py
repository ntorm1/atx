"""Moved to atx-impl/tools/nav_summ.py (platform v8 V-1). This shim keeps the path the pinned specs v61..v71 and
scripts/tests/test_research_cycle.py use: run, it runs the moved script; imported, it is the moved module."""
import importlib
from pathlib import Path
import runpy
import sys

_TOOLS = Path(__file__).resolve().parents[4] / "atx-impl" / "tools"
sys.path.insert(0, str(_TOOLS))
if __name__ == "__main__":
    runpy.run_path(str(_TOOLS / "nav_summ.py"), run_name="__main__")
else:
    sys.modules.pop(__name__, None)
    _moved = importlib.import_module("nav_summ")
    sys.modules[__name__] = _moved
    globals().update({k: v for k, v in vars(_moved).items() if not k.startswith("__")})
