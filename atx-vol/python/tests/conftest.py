"""Contamination guard for the atx-vol Python suite (RECONCILE 4).

THE TRAP. ``atxvol`` is installed on the development box as a scikit-build-core
**editable** install. That install works by registering a ``sys.meta_path``
finder (``ScikitBuildRedirectingFinder``), and meta-path finders are consulted
BEFORE ``sys.path`` — so they outrank the current directory, an activated
virtualenv's site-packages, and ``PYTHONPATH`` alike. A plain ``pytest`` run
from ANY worktree therefore imports whatever checkout that install points at,
silently, and reports the result as this tree's.

That is not hypothetical. WS-Y reported a green Python suite that had been run
against a different checkout, and the merge agent had to build a throwaway venv
before any Python evidence from this worktree could be trusted. It is the same
defect class as a test binary borrowed from another build directory: a green
that is an accident of what is installed rather than evidence about the branch
under test.

WHY HERE. The registered ``atx-vol-python`` ctest lane already defends itself —
``_ctest_pytest_driver.py`` strips the finder, puts this tree's ``src`` first,
and then HARD-FAILS unless both ``atxvol.__file__`` and ``atxvol._core.__file__``
resolve inside the tree. Those assertions are untouched by this file and must
stay: they are the guarantee for the gated path. The hole is every *ad hoc*
run — ``pytest atx-vol/python/tests``, an IDE runner, a one-off ``-k`` while
debugging — none of which goes through the driver. A ``conftest.py`` sits on all
of them, so the trap reports itself instead of being folklore.

WHAT IT DOES. It FAILS, loudly, naming both resolved paths and the tree it
expected. It deliberately does NOT silently repair ``sys.path``: a run that
needs repairing is a run whose result you should not have trusted a moment
earlier, and quietly fixing it would hide that the ambient environment is
misconfigured. The driver repairs; this reports.

Resolution is done with ``importlib.util.find_spec`` rather than by importing,
so the guard still fires in a worktree where the compiled ``_core`` extension is
absent — which is precisely the worktree where someone is most likely to reach
for an ad-hoc pytest and land on another checkout's sources.
"""

from __future__ import annotations

import importlib.util
import os
import pathlib
import sys

import pytest

# The tree that OWNS this test file. `parents[1]` is .../atx-vol/python.
_SRC = (pathlib.Path(__file__).resolve().parents[1] / "src").resolve()

_UNRESOLVED = "<not resolvable>"


def _is_inside(path: str | None, root: pathlib.Path) -> bool:
    if not path:
        return False
    try:
        return os.path.commonpath([os.path.abspath(path), str(root)]) == str(root)
    except ValueError:  # different drives on Windows
        return False


def _spec_origin(name: str) -> str:
    """Where ``name`` WOULD resolve, without executing the package body.

    Falls back to an already-imported module's ``__file__`` (the ctest driver
    imports ``atxvol`` before pytest starts), and to the namespace-package
    search locations when a spec carries no origin.
    """
    mod = sys.modules.get(name)
    if mod is not None and getattr(mod, "__file__", None):
        return str(mod.__file__)
    try:
        spec = importlib.util.find_spec(name)
    except (ImportError, AttributeError, ValueError) as exc:
        return f"{_UNRESOLVED} ({type(exc).__name__}: {exc})"
    if spec is None:
        return f"{_UNRESOLVED} (no module named {name!r} on this interpreter)"
    if spec.origin:
        return str(spec.origin)
    locations = list(spec.submodule_search_locations or [])
    return str(locations[0]) if locations else f"{_UNRESOLVED} (namespace package)"


def _contamination_message(pkg: str, core: str) -> str:
    return (
        "atx-vol Python suite: `atxvol` does NOT resolve inside the tree that "
        "owns these tests, so this run would exercise a DIFFERENT checkout's "
        "sources and report the result as this one's.\n"
        f"  expected under : {_SRC}\n"
        f"  atxvol         -> {pkg}\n"
        f"  atxvol._core   -> {core}\n"
        "\n"
        "Most likely cause: a scikit-build-core EDITABLE install of `atxvol` "
        "pointing at another checkout. Its `sys.meta_path` finder is consulted "
        "before `sys.path`, so cwd, an activated venv and $PYTHONPATH all lose "
        "to it.\n"
        "Fixes, in order of preference:\n"
        "  1. run the registered lane instead: "
        "ctest --test-dir <build> -L atx_vol -R atx-vol-python\n"
        "  2. run tests/_ctest_pytest_driver.py, which strips the finder and "
        "pins this tree's src;\n"
        "  3. `pip uninstall atxvol` (or use an isolated venv) before an ad-hoc "
        "pytest.\n"
        "This is a hard failure on purpose: a green run against the wrong "
        "sources is worse than a red one."
    )


def _check_resolution() -> None:
    pkg = _spec_origin("atxvol")
    if pkg.startswith(_UNRESOLVED):
        # atxvol is not importable at all here. That is a different problem, and
        # the individual test modules report it far better than a blanket
        # collection abort would — there is no contamination to warn about.
        return
    core = _spec_origin("atxvol._core")
    # REV-TAIL M-4. This used to return the moment `atxvol` ALONE resolved in
    # tree, and resolved `core` only to BUILD the failure message — so a SPLIT
    # resolution (pure-Python package in tree, compiled extension out of it)
    # passed this guard while `_ctest_pytest_driver.py`, which loops over BOTH,
    # hard-failed on the same environment. The module docstring promises this
    # file reports what the gated path enforces; it did not.
    #
    # This is not hypothetical on this box: with the ScikitBuild finder in place
    # `atxvol` resolves to C:\atx\atx-vol\python\src\atxvol\__init__.py while
    # `atxvol._core` resolves to the user's site-packages .pyd — two different
    # locations already.
    #
    # An UNRESOLVABLE `_core` is not contamination and must not fire: a worktree
    # with no built extension is the normal case here (it is why `_spec_origin`
    # avoids importing), and the individual modules report it far better.
    if _is_inside(pkg, _SRC) and (core.startswith(_UNRESOLVED) or _is_inside(core, _SRC)):
        return
    raise pytest.UsageError(_contamination_message(pkg, core))


_check_resolution()
