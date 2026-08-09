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


# --- Workaround 1: stale/foreign scikit-build-core editable-install redirect
#
# `pip install -e ./python` bakes an *absolute* source path into a finder
# registered on sys.meta_path (ahead of the normal PathFinder, so it wins
# every `import atxpy` regardless of sys.path). That finder is machine-wide,
# not worktree-scoped: only one `atxpy` editable install can be "active" on
# this machine at a time, whichever worktree last ran `pip install -e`.
#
# Two distinct ways that goes wrong for *this* worktree:
#   1. The baked target doesn't exist at all -- e.g. it was baked from a
#      previous repo location (`...\OneDrive\Desktop\atx\...`) that no
#      longer exists (the repo moved to `C:\atx` / `C:\atx-wt\<worktree>`).
#      Confirmed pre-existing and machine-wide (reproduces from `C:\atx`
#      directly too, not specific to this worktree or to atxpy.pbo).
#   2. The baked target exists but points at a *sibling* worktree's
#      `atxpy/__init__.py` -- e.g. someone re-ran `pip install -e ./python`
#      from `C:\atx` or another `pool-N` after this file's first version was
#      written. A bare `.exists()` check reads that as "healthy" and leaves
#      it alone, so `import atxpy` silently loads the wrong worktree's
#      source *and compiled binary* with no error -- worse than case 1,
#      which at least fails loudly.
#
# So "healthy" here specifically means "resolves to THIS worktree's
# atxpy/__init__.py", not merely "resolves to some file". The workaround
# engages whenever the redirect is missing OR points elsewhere; only when
# it resolves to this worktree's own `_ATXPY_SRC_DIR` is sys.meta_path left
# completely untouched (true no-op), so a real ABI break in a freshly built
# `_core` extension still surfaces normally instead of being silently
# masked by a stale, hand-copied fallback .pyd.
#
# DELETE this block once every worktree gets its own correctly-scoped
# editable install (or once this repo stops needing per-worktree pip
# installs at all), i.e. once `python -c "import atxpy"` succeeds *from
# this worktree* with no conftest involved and without disturbing any
# other worktree's install.
_finder, _target = _find_atxpy_redirect()
_this_worktree_init = (_ATXPY_SRC_DIR / "__init__.py").resolve()
_redirect_is_broken = _finder is not None and (
    not _target.exists() or _target.resolve() != _this_worktree_init
)
if _redirect_is_broken:
    sys.meta_path = [f for f in sys.meta_path if f is not _finder]

    src_str = str(_SRC)
    if src_str not in sys.path:
        sys.path.insert(0, src_str)

    _core_present = any(_ATXPY_SRC_DIR.glob("_core*.pyd")) or any(_ATXPY_SRC_DIR.glob("_core*.so"))
    if not _core_present:
        _reason = (
            "does not exist"
            if not _target.exists()
            else f"points at a different worktree ({_target}, not {_this_worktree_init})"
        )
        pytest.exit(
            f"atxpy's pip editable install is broken for this worktree: its "
            f"redirect target {_reason}, and no fallback compiled extension "
            f"(_core*.pyd / _core*.so) was found in {_ATXPY_SRC_DIR} either, "
            "so `import atxpy` cannot succeed.\n"
            "Fix: from THIS worktree's root run\n"
            "    python -m pip install -e ./python --no-build-isolation\n"
            "(see python/README.md) to rebuild/reinstall against this "
            "worktree's path -- note this will also change which worktree "
            "the machine-wide editable install points at for everyone else. "
            "As a stopgap, a matching compiled _core*.pyd copied from a "
            "working install into that directory will also let this "
            "workaround proceed.",
            returncode=1,
        )
# else: no editable-install finder found, or its target resolves to this
# worktree's own atxpy/__init__.py -- nothing to do here; atxpy will import
# normally below.

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
