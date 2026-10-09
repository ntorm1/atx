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

import pytest

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


# ── REV-TAIL M-3/M-4 — the GATED driver must be no weaker than this guard ─────
#
# `conftest.py` defends ad-hoc runs; `_ctest_pytest_driver.py` defends the
# registered ctest lane. The lane is the more important path, and it carried the
# weaker check: a string prefix where this file uses path containment, and it was
# this file that shipped the test forbidding the prefix form. The pair below pins
# the two against each other so they cannot drift apart again.


def _driver_is_inside():
    """The REAL ``_is_inside`` out of ``_ctest_pytest_driver.py``.

    The driver cannot be imported — its module body ends in
    ``raise SystemExit(pytest.main(...))``, so importing it would run the whole
    suite recursively. Its function is therefore lifted out of the source by
    ``ast`` and executed, which gates the shipped code rather than a copy of it.
    """
    import ast
    import os

    source = (pathlib.Path(__file__).resolve().parent / "_ctest_pytest_driver.py").read_text(
        encoding="utf-8"
    )
    tree = ast.parse(source)
    fn = next(
        (n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "_is_inside"),
        None,
    )
    assert fn is not None, "_ctest_pytest_driver.py no longer defines _is_inside"
    namespace: dict = {"os": os}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "<driver>", "exec"), namespace)
    return namespace["_is_inside"]


def test_the_gated_driver_uses_containment_not_a_string_prefix():
    """REV-TAIL M-3. ``<src>-other`` must NOT pass as in-tree in the DRIVER either.

    ``test_is_inside_is_a_path_containment_test_not_a_string_prefix`` above has
    forbidden this for ``conftest`` since RECONCILE 4, while the driver the ctest
    lane actually runs kept ``os.path.abspath(path).startswith(os.path.abspath(
    SRC))`` — which accepts exactly the sibling this rejects.
    """
    is_inside = _driver_is_inside()
    root = pathlib.Path("C:/x/src").resolve() if sys.platform == "win32" else pathlib.Path("/x/src")
    assert is_inside(str(root / "atxvol" / "__init__.py"), str(root))
    assert not is_inside(str(root.parent / "src-other" / "atxvol" / "__init__.py"), str(root))
    assert not is_inside(None, str(root))
    assert not is_inside("", str(root))


def test_the_two_guards_agree_on_every_case():
    """Same input, same verdict — the property that keeps them from drifting."""
    is_inside = _driver_is_inside()
    root = pathlib.Path("C:/x/src").resolve() if sys.platform == "win32" else pathlib.Path("/x/src")
    cases = (
        str(root / "atxvol" / "__init__.py"),
        str(root.parent / "src-other" / "atxvol" / "__init__.py"),
        str(root.parent / "elsewhere" / "atxvol.py"),
        None,
        "",
    )
    for case in cases:
        assert is_inside(case, str(root)) == conftest._is_inside(case, root), case


def _split_resolution(monkeypatch, core_origin: str) -> str:
    """Make ``atxvol`` resolve in tree and ``atxvol._core`` resolve to ``core_origin``."""
    pkg = str(conftest._SRC / "atxvol" / "__init__.py")
    monkeypatch.setattr(
        conftest, "_spec_origin", lambda name: pkg if name == "atxvol" else core_origin
    )
    return pkg


def test_a_split_resolution_is_contamination_and_the_guard_says_so(monkeypatch):
    """REV-TAIL M-4. ``_check_resolution`` used to return the moment ``atxvol``
    alone resolved in tree, never examining ``_core`` — it was resolved only to
    build the message. So an environment with the pure-Python package in tree and
    the COMPILED extension from a site-packages install elsewhere passed this
    guard and hard-failed the gated driver, which loops over both. The docstring
    promised parity; the code did not deliver it.

    ``<src>-other`` is used for the foreign path on purpose: it is also the
    string-prefix trap, so this fails for the right reason under either check."""
    foreign = str(
        conftest._SRC.parent / (conftest._SRC.name + "-other") / "atxvol" / "_core.pyd"
    )
    pkg = _split_resolution(monkeypatch, foreign)
    with pytest.raises(pytest.UsageError) as excinfo:
        conftest._check_resolution()
    message = str(excinfo.value)
    assert foreign in message, message
    assert pkg in message, message


def test_an_unbuilt_extension_is_not_contamination(monkeypatch):
    """The other half of M-4, and the reason it is not simply `and _is_inside`.

    A worktree with no compiled ``_core`` is the NORMAL case for this guard —
    it is why resolution goes through ``find_spec`` instead of importing — and
    tightening M-4 must not turn that into a blanket collection abort."""
    _split_resolution(monkeypatch, f"{conftest._UNRESOLVED} (no module named 'atxvol._core')")
    conftest._check_resolution()  # must not raise


def test_missing_atxvol_is_not_treated_as_contamination():
    """A tree with no ``atxvol`` at all has nothing to warn about — the guard
    must stay out of the way so the individual modules can report the real
    problem (no compiled extension) instead of a blanket collection abort."""
    assert conftest._spec_origin("atxvol_definitely_not_installed_xyz").startswith(
        conftest._UNRESOLVED
    )
    # And find_spec agrees, so the sentinel is not masking a real resolution.
    assert importlib.util.find_spec("atxvol_definitely_not_installed_xyz") is None
