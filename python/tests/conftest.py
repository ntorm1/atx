"""Test-session bootstrap.

The machine-wide ``atxpy`` editable install (scikit-build-core's
``ScikitBuildRedirectingFinder``, registered via a ``.pth`` file in
site-packages) was baked with an absolute source path from a previous
repo location. That path no longer exists, so *any* ``import atxpy`` in
*any* worktree currently fails with a ``FileNotFoundError`` regardless of
``sys.path`` — the redirecting finder sits in ``sys.meta_path`` ahead of
the normal ``PathFinder`` and wins the lookup unconditionally.

This is a pre-existing, machine-level breakage unrelated to any one
worktree's code. Rather than mutate the user's global site-packages (or
require a full C++ engine rebuild just to run a pure-Python test), drop
the stale finder for this pytest process only and let the normal
``PathFinder`` resolve ``atxpy`` from *this* worktree's ``python/src``.
The compiled ``_core`` extension is resolved the ordinary way too: a
matching ``_core*.pyd`` is expected alongside this worktree's
``src/atxpy`` (copied from the machine's build output; see
``python/.gitignore`` — build artifacts are untracked).
"""

from __future__ import annotations

import sys
from pathlib import Path

_SRC = Path(__file__).resolve().parents[1] / "src"

sys.meta_path = [
    finder
    for finder in sys.meta_path
    if type(finder).__name__ != "ScikitBuildRedirectingFinder"
    or "atxpy" not in getattr(finder, "known_source_files", {})
]

_src_str = str(_SRC)
if _src_str not in sys.path:
    sys.path.insert(0, _src_str)

# Load-order guard: importing `pandas` before `atxpy._core` in a fresh
# process reproducibly crashes with a Windows access violation while the
# native `_core*.pyd` extension is loading (confirmed by isolated repro —
# `import pandas` then `import atxpy` segfaults; the reverse order is fine).
# Root cause looks like a native-dependency collision between pandas'
# bundled libraries and the vcpkg-linked arrow/parquet/zstd/openssl `_core`
# extension; whichever loads first wins the process-wide DLL/symbol table.
# Importing atxpy here, before pytest imports any test module (and before
# any test module's own top-level `import pandas`), makes every test file
# safe to run standalone regardless of its own import order.
import atxpy  # noqa: E402,F401
