"""Python bindings for the atx-vol options and volatility library."""

from __future__ import annotations

try:
    from . import _core
except ImportError as exc:  # pragma: no cover - environment-dependent
    # The known one is the pyarrow DLL collision: `atxvol._core` and `pyarrow.lib`
    # both link vcpkg's `arrow.dll` / `parquet.dll` by base name, and on Windows the
    # first one loaded claims the process-wide slot, so the second fails here with
    # "DLL load failed while importing _core". A bare ImportError points at nothing;
    # the README names both directions of the failure and the workaround.
    raise ImportError(
        f"{exc}\n\n"
        "atxvol failed to import its native extension. If this is "
        "'DLL load failed while importing _core' and this interpreter has also "
        "imported pyarrow, it is the known atxvol/pyarrow Arrow-DLL collision: "
        "use separate processes for the two. See 'Known limitation: atxvol and "
        "pyarrow cannot share a process on Windows' in atx-vol/python/README.md. "
        "Note that BUILDING and VERIFYING a surface database needs no Python at "
        "all - atx-vol-surface-db-build and atx-vol-surface-db do the whole job."
    ) from exc

from ._core import *  # noqa: F403,E402

__version__ = _core.__version__
__all__ = [name for name in dir(_core) if not name.startswith("_")]

