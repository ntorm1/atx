"""Tests for the ad-hoc-run contamination guard in ``conftest.py`` (RECONCILE 4).

The guard exists because a ``sys.meta_path`` finder registered by a
scikit-build-core editable install outranks ``sys.path``, so a plain ``pytest``
from any worktree can silently exercise a different checkout's sources. See
``conftest.py`` for the full account.

A guard is only worth having if it fires, so the end-to-end test below drives a
REAL pytest subprocess whose ``atxvol`` deliberately resolves out of tree and
asserts the run dies with both resolved paths on stderr.
"""

from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys

import conftest


def test_guard_accepts_this_tree():
    """The negative control. If the guard mis-fired on an in-tree resolution
    this module could not have been collected at all — but assert it explicitly
    rather than leaning on that, so the property is stated and not implied."""
    assert conftest._SRC.name == "src"
    assert conftest._SRC.parent.name == "python"
    origin = conftest._spec_origin("atxvol")
    assert not origin.startswith(conftest._UNRESOLVED), origin
    assert conftest._is_inside(origin, conftest._SRC), origin


def test_is_inside_is_a_path_containment_test_not_a_string_prefix():
    """``C:\\x\\src-other`` starts with ``C:\\x\\src`` as a STRING but is a
    different directory. A string-prefix check here would let a sibling tree
    pass as in-tree."""
    root = pathlib.Path("C:/x/src").resolve() if sys.platform == "win32" else pathlib.Path("/x/src")
    inside = str(root / "atxvol" / "__init__.py")
    sibling = str(root.parent / "src-other" / "atxvol" / "__init__.py")
    assert conftest._is_inside(inside, root)
    assert not conftest._is_inside(sibling, root)
    assert not conftest._is_inside(None, root)
    assert not conftest._is_inside("", root)


def test_message_names_both_resolved_paths_and_the_expected_tree():
    """The message is the whole deliverable: whoever hits this needs to see WHAT
    resolved WHERE without re-deriving it."""
    msg = conftest._contamination_message(
        r"C:\other\atx-vol\python\src\atxvol\__init__.py",
        r"C:\site-packages\atxvol\_core.cp312-win_amd64.pyd",
    )
    assert r"C:\other\atx-vol\python\src\atxvol\__init__.py" in msg
    assert r"C:\site-packages\atxvol\_core.cp312-win_amd64.pyd" in msg
    assert str(conftest._SRC) in msg
    assert "meta_path" in msg  # names the mechanism, not just the symptom


def test_out_of_tree_resolution_aborts_a_real_pytest_run(tmp_path):
    """End-to-end: a pytest subprocess whose ``atxvol`` resolves to a throwaway
    package must die at conftest load, naming both paths.

    ``sitecustomize`` strips any ScikitBuild finder so the outcome does not
    depend on whether THIS box happens to carry the editable install — the fake
    on ``$PYTHONPATH`` then wins on every machine, and the test measures the
    guard rather than the host."""
    fake = tmp_path / "fakeroot"
    (fake / "atxvol").mkdir(parents=True)
    (fake / "atxvol" / "__init__.py").write_text(
        "__version__ = 'impostor'\n", encoding="utf-8"
    )
    (fake / "sitecustomize.py").write_text(
        "import sys\n"
        "sys.meta_path[:] = [f for f in sys.meta_path\n"
        "                    if 'ScikitBuild' not in type(f).__name__]\n",
        encoding="utf-8",
    )

    tests_dir = pathlib.Path(__file__).resolve().parent
    env = dict(**_clean_env())
    env["PYTHONPATH"] = str(fake)

    proc = subprocess.run(
        [sys.executable, "-m", "pytest", str(tests_dir), "--collect-only", "-q",
         "-p", "no:cacheprovider"],
        capture_output=True, text=True, env=env, cwd=str(tmp_path),
    )
    combined = proc.stdout + proc.stderr
    assert proc.returncode != 0, f"guard did not fire:\n{combined}"
    assert "does NOT resolve inside the tree" in combined, combined
    assert str(fake / "atxvol" / "__init__.py") in combined, combined
    assert str(conftest._SRC) in combined, combined


def _clean_env() -> dict[str, str]:
    import os
    env = dict(os.environ)
    env.pop("PYTHONPATH", None)
    return env


def test_missing_atxvol_is_not_treated_as_contamination():
    """A tree with no ``atxvol`` at all has nothing to warn about — the guard
    must stay out of the way so the individual modules can report the real
    problem (no compiled extension) instead of a blanket collection abort."""
    assert conftest._spec_origin("atxvol_definitely_not_installed_xyz").startswith(
        conftest._UNRESOLVED
    )
    # And find_spec agrees, so the sentinel is not masking a real resolution.
    assert importlib.util.find_spec("atxvol_definitely_not_installed_xyz") is None
