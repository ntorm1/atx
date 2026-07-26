"""ctest entry point for the atx-vol Python suite (FIX-5/I6).

WHY THIS FILE EXISTS. Before FIX-5 the eight ``atx-vol/python/tests/test_*.py``
modules were registered with **zero** ctest tests: ``grep -rn "pytest"
--include=CMakeLists.txt .`` returned nothing repo-wide, and a grep for any
pytest test name in the whole-repo gate log returned 0. An entire workstream's
output (the Python layer) was gated by nothing, and its evidence came from
running pytest by hand. The concrete casualty was
``test_backtest.py::test_run_config_defaults_mirror_the_engine_header`` — a
tripwire hand-edited during a merge to track an engine-side default flip, which
no gate executed.

``atx-vol/tests/CMakeLists.txt`` now registers this script as ``atx-vol-python``
under the ``atx_vol`` label, beside the five existing script tests.

Three things have to be handled here rather than in the ``add_test`` line, which
is why the command is a driver script and not a bare ``-m pytest``:

1. THE EXTENSION GUARD. ``atx-vol/python`` is a *standalone* scikit-build-core
   project (its own ``project()``, its own build tree) — it is NOT an
   ``add_subdirectory`` of the monorepo build, so there is no ``_core`` target in
   the main build graph to guard the registration on at configure time. The
   guard is therefore made at run time: if the compiled extension cannot be
   imported, this exits ``SKIP_RETURN_CODE`` (77) with the reason on stdout, and
   ctest reports the lane as **Skipped**. That is deliberately different from not
   registering: a skip with a reason is visible in every gate log, which is the
   property I6 is about. ``atxvol/__init__.py`` does ``from . import _core``, so
   every test module — even the pure-Python reader tests, which reach the reader
   through the ``atxvol`` package — needs the extension.

2. WHICH SOURCES GET TESTED. The user's site-packages carries a
   scikit-build-core **editable** install of ``atxvol`` whose meta-path finder
   outranks ``sys.path`` and resolves the package to a DIFFERENT checkout. A
   plain ``pytest`` invoked from this tree would silently exercise that other
   checkout's Python sources and report the result as this tree's. The finder is
   stripped and this tree's ``src`` is put first, and both ``atxvol.__file__``
   and ``atxvol._core.__file__`` are then asserted to resolve inside this tree —
   a hard failure, not a skip, because a green run against the wrong sources is
   worse than a red one.

3. TWO HOST STALLS THAT HANG PYTEST FOREVER (documented by WS-Y; not repo
   defects, and not fixed in the repo). ``_pytest.capture._readline_workaround``
   does a bare ``import readline``; on a box where that resolves to the
   ``pyreadline3`` shim, the shim's module body calls ``platform.system()`` ->
   ``_win32_ver`` -> a WMI query that never returns. Same stall through a second
   door: ``numpy.testing`` calls ``platform.machine()`` at module scope, so the
   first ``np.testing.assert_*`` in a session hangs. Both are neutralized below
   before anything else is imported. A ctest ``TIMEOUT`` would turn either stall
   into a 15-minute red gate; neutralizing them keeps the lane usable.
"""

from __future__ import annotations

import os
import sys
import types

SKIP = 77  # must match SKIP_RETURN_CODE in atx-vol/tests/CMakeLists.txt

# ── (3) host stalls — before importing pytest or numpy ───────────────────────
# pytest only wants `readline` for its libedit side effect, so a stub satisfies it.
sys.modules.setdefault("readline", types.ModuleType("readline"))

import platform  # noqa: E402


def _no_wmi(*_args, **_kwargs):
    # platform's own code path treats OSError from _wmi_query as "WMI
    # unavailable" and falls back to the registry / sys.getwindowsversion().
    raise OSError("WMI disabled by the atx-vol ctest driver (host query stalls)")


platform._wmi_query = _no_wmi

# ── (2) pin the sources under test to THIS tree ──────────────────────────────
HERE = os.path.dirname(os.path.abspath(__file__))
PYROOT = os.path.dirname(HERE)          # .../atx-vol/python
SRC = os.path.join(PYROOT, "src")
TESTS = HERE

sys.meta_path[:] = [f for f in sys.meta_path if "ScikitBuild" not in type(f).__name__]
sys.path.insert(0, SRC)

# ── (1) the extension guard ──────────────────────────────────────────────────
try:
    import pytest
except ImportError as exc:  # pragma: no cover - environment probe
    print(f"SKIP: pytest is not installed for {sys.executable}: {exc}")
    raise SystemExit(SKIP)

try:
    import numpy  # noqa: F401
except ImportError as exc:  # pragma: no cover - environment probe
    print(f"SKIP: numpy is not installed for {sys.executable}: {exc}")
    raise SystemExit(SKIP)

try:
    import atxvol
except Exception as exc:  # pragma: no cover - environment probe
    print(
        "SKIP: the atxvol compiled extension (_core) is not importable, so the "
        "Python suite cannot run. Build it with the standalone project under "
        f"atx-vol/python (see its README). Reason: {exc!r}"
    )
    raise SystemExit(SKIP)

def _is_inside(path, root):
    """Path CONTAINMENT, not a string prefix (REV-TAIL M-3).

    ``<root>-other`` starts with ``<root>`` as a STRING but is a DIFFERENT
    directory, so the ``startswith`` form this replaces would let a sibling
    checkout pass as in-tree -- precisely the contamination this check exists to
    stop. ``conftest.py`` has used ``commonpath`` since RECONCILE 4 and ships
    ``test_is_inside_is_a_path_containment_test_not_a_string_prefix`` forbidding
    the prefix form; this driver -- the one the GATED ctest lane actually runs --
    still carried it, so the weaker check was on the more important path.

    Kept as a local copy rather than imported from ``conftest``: this module runs
    BEFORE pytest starts and must not depend on pytest's collection machinery.
    ``test_conftest_guard.py`` pins the two implementations against each other.
    """
    if not path:
        return False
    try:
        root = os.path.abspath(root)
        return os.path.commonpath([os.path.abspath(path), root]) == root
    except ValueError:  # different drives on Windows
        return False


# Wrong-sources check is a HARD failure: a green run against another checkout is
# worse than a red one.
for mod, path in (("atxvol", atxvol.__file__), ("atxvol._core", atxvol._core.__file__)):
    if not _is_inside(path, SRC):
        print(
            f"FAIL: {mod} resolved to {path}, which is OUTSIDE this tree's "
            f"{SRC}. An editable install is shadowing the sources under test."
        )
        raise SystemExit(1)

print(f"atx-vol-python: {atxvol.__file__}")
print(f"atx-vol-python: {atxvol._core.__file__}")

raise SystemExit(
    pytest.main(
        [
            TESTS,
            "-q",
            # Skip reasons in the gate log (PY-FIX 1). Without this, `-q` prints a
            # bare `s` and the reason is never recorded anywhere — which is how a
            # module-wide guard misfire (5 tests silently not running on every
            # worktree) survived. A skip is only acceptable when the log says why.
            "-rs",
            "-o", "addopts=",          # ignore any ambient addopts
            "-p", "no:cacheprovider",  # never write .pytest_cache into the tree
            "--rootdir", PYROOT,
        ]
    )
)
