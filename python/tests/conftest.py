"""Test-session bootstrap.

Two independent, environment-specific workarounds live here. Each is
guarded to engage only when its underlying condition is actually present,
and each should be deleted once that condition is fixed on this
machine/CI image -- see the comment above each block.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_SRC = Path(__file__).resolve().parents[1] / "src"
_ATXPY_SRC_DIR = _SRC / "atxpy"


def _find_atxpy_redirect():
    """Return (finder, target_path) for the ``atxpy`` entry of the
    scikit-build-core ``ScikitBuildRedirectingFinder`` registered on
    ``sys.meta_path`` by the editable install, or ``(None, None)`` if no
    such finder is present (e.g. atxpy is installed normally, not
    editable)."""
    for finder in sys.meta_path:
        if type(finder).__name__ != "ScikitBuildRedirectingFinder":
            continue
        known = getattr(finder, "known_source_files", {})
        target = known.get("atxpy")
        if target is not None:
            return finder, Path(target)
    return None, None


# --- Workaround 1: stale scikit-build-core editable-install redirect -------
#
# `pip install -e ./python` bakes an *absolute* source path into a finder
# registered on sys.meta_path (ahead of the normal PathFinder, so it wins
# every `import atxpy` regardless of sys.path). On this machine that path
# was baked from a previous repo location (`...\OneDrive\Desktop\atx\...`)
# that no longer exists -- the repo has since moved to `C:\atx` /
# `C:\atx-wt\<worktree>`. Confirmed pre-existing and machine-wide (reproduces
# from `C:\atx` directly too, not specific to this worktree or to atxpy.pbo).
#
# This block engages the workaround ONLY when that redirect is actually
# broken (its baked target path does not exist). In a correctly configured
# environment (fresh clone, `pip install -e` re-run against the new path,
# a CI image) the finder's target resolves fine and this block is a no-op:
# sys.meta_path and sys.path are left completely untouched, so a real ABI
# break in a freshly built `_core` extension still surfaces normally
# instead of being silently masked by a stale, hand-copied fallback .pyd.
#
# DELETE this block once the editable install is repaired for real, i.e.
# once `python -c "import atxpy"` succeeds with no conftest involved.
_finder, _target = _find_atxpy_redirect()
if _finder is not None and not _target.exists():
    sys.meta_path = [f for f in sys.meta_path if f is not _finder]

    src_str = str(_SRC)
    if src_str not in sys.path:
        sys.path.insert(0, src_str)

    _core_present = any(_ATXPY_SRC_DIR.glob("_core*.pyd")) or any(_ATXPY_SRC_DIR.glob("_core*.so"))
    if not _core_present:
        pytest.exit(
            "atxpy's pip editable install is broken: its redirect target "
            f"({_target}) does not exist (the repo moved), and no fallback "
            f"compiled extension (_core*.pyd / _core*.so) was found in "
            f"{_ATXPY_SRC_DIR} either, so `import atxpy` cannot succeed.\n"
            "Fix: from the repo root run\n"
            "    python -m pip install -e ./python --no-build-isolation\n"
            "(see python/README.md) to rebuild/reinstall against the "
            "current path. As a stopgap, a matching compiled _core*.pyd "
            "copied from a working install into that directory will also "
            "let this workaround proceed.",
            returncode=1,
        )
# else: no editable-install finder found, or its target resolves fine --
# nothing to do here; atxpy will import normally below.

# --- Workaround 2: pandas-before-atxpy native load-order crash -------------
#
# Importing `pandas` before `atxpy` (i.e. before atxpy._core's native
# extension loads) reproducibly crashes with a Windows access violation in
# a fresh process -- confirmed by isolated repro (pandas-then-atxpy segfaults,
# atxpy-then-pandas is fine). Looks like a native-dependency/DLL collision
# between pandas' bundled libraries and the vcpkg-linked
# (arrow/parquet/zstd/openssl) `_core` extension; whichever loads first wins
# the process-wide DLL/symbol table. Importing atxpy here, before pytest
# imports any test module (and therefore before that module's own top-level
# `import pandas` can run), makes every test file safe to run standalone
# regardless of its own import order. Independent of Workaround 1 above --
# keep this even after that one is deleted.
import atxpy  # noqa: E402,F401
