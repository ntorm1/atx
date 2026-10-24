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

4. THE IN-TREE BUILD (lane PY, 2026-10-23). Configured with
   ``-DATX_VOL_PYTHON=ON``, the monorepo build owns the extension as target
   ``atx-vol-python-core`` and stages it into this tree's ``src/atxvol``; ctest
   then runs this script with ``--expect-core <$<TARGET_FILE:...>>``. In that
   mode nothing may SKIP — the build that registered the lane also built the
   module, so an unimportable ``_core`` is a red gate, not an environment
   probe — and the staged module must be byte-identical (sha256) to that exact
   build output, so a stale stage, or one another build tree of this worktree
   wrote, fails loudly instead of being tested in its place.

5. TWO PROCESSES, BECAUSE TWO ARROWS (lane PY, 2026-10-23). ``_core`` links
   vcpkg's ``arrow.dll`` (24.0.0 at this writing, through atx-core's
   unconditional Parquet loader) and the user-site ``pyarrow`` bundles its own,
   differently-versioned ``arrow.dll`` (18.0.0). Windows binds a bare-name DLL
   import to whichever same-named module is already resident, so once ``_core``
   is loaded, ``import pyarrow`` dies with "DLL load failed while importing lib:
   The specified procedure could not be found" — the ORDER rule
   ``atxvol/chain.py`` documents (atxvol first, then pyarrow) held only while the
   two Arrow builds happened to be compatible. One pytest session cannot host
   both, and the first gated run of this suite in months showed ten collection
   errors from exactly that. So the suite is PARTITIONED, statically, before any
   test module is imported: a module that imports ``atxvol`` or ``conftest``
   (directly, or through a sibling test module it imports) runs in THIS process
   behind the guards above; every other module runs in a fresh child interpreter
   (``--arrow-partition``) with ``--noconftest`` — conftest's resolution probe
   would itself import ``atxvol._core`` — and with ``atxvol`` made unimportable,
   so a misclassified module fails by name instead of poisoning the Arrow slot.
   That is the same isolation the ``atx-vol-quote-benchmark-tools`` lane already
   uses for seven of those modules. Either partition failing fails the lane.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
import types

SKIP = 77  # must match SKIP_RETURN_CODE in atx-vol/tests/CMakeLists.txt

_USAGE = "usage: [--expect-core PATH] | --arrow-partition FILE..."


def _parse_args(argv: list[str]) -> tuple[str | None, list[str] | None]:
    """(expect_core, arrow_partition_files). Both None = legacy skip mode."""
    if not argv:
        return None, None
    if len(argv) == 2 and argv[0] == "--expect-core" and argv[1]:
        return os.path.abspath(argv[1]), None
    if len(argv) >= 2 and argv[0] == "--arrow-partition":
        return None, argv[1:]
    print(f"FAIL: unrecognised driver arguments {argv!r}; {_USAGE}")
    raise SystemExit(2)


EXPECT_CORE, ARROW_PARTITION = _parse_args(sys.argv[1:])
# In gate mode an absent prerequisite is a FAILURE: the build asked for this lane.
UNAVAILABLE = 1 if EXPECT_CORE else SKIP
UNAVAILABLE_TAG = "FAIL" if EXPECT_CORE else "SKIP"

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

_PYTEST_COMMON = [
    "-q",
    # Skip reasons in the gate log (PY-FIX 1). Without this, `-q` prints a
    # bare `s` and the reason is never recorded anywhere — which is how a
    # module-wide guard misfire (5 tests silently not running on every
    # worktree) survived. A skip is only acceptable when the log says why. `x`
    # adds the same for strict xfails (a known defect recorded in the suite).
    "-rsx",
    "-o", "addopts=",          # ignore any ambient addopts
    "-p", "no:cacheprovider",  # never write .pytest_cache into the tree
    "--rootdir", PYROOT,
]


# ── (5) the static partition ─────────────────────────────────────────────────
_IMPORT_RE = re.compile(r"^[ \t]*(?:import|from)[ \t]+([A-Za-z_]\w*)", re.MULTILINE)
_ATXVOL_ROOTS = frozenset({"atxvol", "conftest"})


def _partition(tests_dir: str) -> tuple[list[str], list[str]]:
    """(atxvol_modules, arrow_safe_modules), both sorted test_*.py paths.

    A module needs the atxvol process when any import line names ``atxvol`` or
    ``conftest`` (whose body resolves ``atxvol._core``), or names a sibling
    module of this directory that does — the closure over sibling imports is
    what puts ``test_dispersion_e2e_discovery`` (which imports the e2e module)
    beside the module it imports. The scan is deliberately conservative: an
    import line inside a string (a child-interpreter preamble) also counts, which
    can only move a module INTO the atxvol process, never out of it.
    """
    sources: dict[str, str] = {}
    for name in sorted(os.listdir(tests_dir)):
        if name.endswith(".py"):
            with open(os.path.join(tests_dir, name), encoding="utf-8") as fh:
                sources[name[:-3]] = fh.read()
    imports = {m: set(_IMPORT_RE.findall(text)) for m, text in sources.items()}
    needs: dict[str, bool] = {}

    def need(mod: str, path: frozenset[str]) -> bool:
        if mod in needs:
            return needs[mod]
        direct = bool(imports[mod] & _ATXVOL_ROOTS)
        via = any(need(dep, path | {mod}) for dep in sorted(imports[mod] & sources.keys())
                  if dep not in path and dep != mod)
        needs[mod] = direct or via
        return needs[mod]

    atx: list[str] = []
    arrow: list[str] = []
    for mod in sources:
        if mod.startswith("test_"):
            (atx if need(mod, frozenset()) else arrow).append(os.path.join(tests_dir, mod + ".py"))
    return atx, arrow


def _child_flags() -> list[str]:
    """This interpreter's isolation flags, so the child resolves imports the same way."""
    flags = []
    if sys.flags.ignore_environment:
        flags.append("-E")
    if getattr(sys.flags, "safe_path", False):
        flags.append("-P")
    if sys.flags.dont_write_bytecode:
        flags.append("-B")
    if sys.flags.utf8_mode:
        flags += ["-X", "utf8"]
    return flags


class _AtxvolBlocked:
    """Meta-path entry that makes ``atxvol`` unimportable in the Arrow partition."""

    def find_spec(self, name, path=None, target=None):  # noqa: ARG002
        if name == "atxvol" or name.startswith("atxvol."):
            raise ImportError(
                f"{name} imported inside the Arrow partition of the atx-vol Python suite: "
                "_ctest_pytest_driver.py classified this test module as atxvol-free, but "
                "loading _core here would claim the process-wide arrow.dll slot pyarrow "
                "needs. Import atxvol (or conftest) at module level so the driver's static "
                "scan moves the module into the atxvol process."
            )
        return None


try:
    import pytest
except ImportError as exc:  # pragma: no cover - environment probe
    print(f"{UNAVAILABLE_TAG}: pytest is not installed for {sys.executable}: {exc}")
    raise SystemExit(UNAVAILABLE)

if ARROW_PARTITION is not None:
    # The child: atxvol-free modules, in an interpreter that never loads _core.
    sys.meta_path.insert(0, _AtxvolBlocked())
    print(f"atx-vol-python[arrow]: {len(ARROW_PARTITION)} module(s), --noconftest")
    raise SystemExit(pytest.main([*ARROW_PARTITION, *_PYTEST_COMMON, "--noconftest"]))

# ── (1) the extension guard ──────────────────────────────────────────────────
try:
    import numpy  # noqa: F401
except ImportError as exc:  # pragma: no cover - environment probe
    print(f"{UNAVAILABLE_TAG}: numpy is not installed for {sys.executable}: {exc}")
    raise SystemExit(UNAVAILABLE)

if EXPECT_CORE is not None and not os.path.isfile(EXPECT_CORE):
    print(
        f"FAIL: --expect-core {EXPECT_CORE} does not exist. Build the in-tree "
        "extension first: atx-build.ps1 build atx-vol-python-core -Preset <preset>"
    )
    raise SystemExit(1)

try:
    import atxvol
except Exception as exc:  # pragma: no cover - environment probe
    print(
        f"{UNAVAILABLE_TAG}: the atxvol compiled extension (_core) is not importable, so "
        "the Python suite cannot run. Build it in-tree (-DATX_VOL_PYTHON=ON, target "
        "atx-vol-python-core) or with the standalone project under atx-vol/python "
        f"(see its README). Reason: {exc!r}"
    )
    raise SystemExit(UNAVAILABLE)

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


def _sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


if EXPECT_CORE is not None:
    staged, built = _sha256(atxvol._core.__file__), _sha256(EXPECT_CORE)
    if staged != built:
        print(
            f"FAIL: the imported {atxvol._core.__file__} (sha256 {staged}) is not the "
            f"build output {EXPECT_CORE} (sha256 {built}). The staged copy is stale or "
            "was written by another build tree; rebuild atx-vol-python-core in this one."
        )
        raise SystemExit(1)
    print(f"atx-vol-python: staged _core == {EXPECT_CORE} (sha256 {built})")

print(f"atx-vol-python: {atxvol.__file__}")
print(f"atx-vol-python: {atxvol._core.__file__}")

ATX_MODULES, ARROW_MODULES = _partition(TESTS)
print(
    f"atx-vol-python: partition atxvol={len(ATX_MODULES)} module(s) in-process, "
    f"arrow={len(ARROW_MODULES)} module(s) in a child interpreter"
)
arrow_rc = 0
if ARROW_MODULES:
    sys.stdout.flush()
    arrow_rc = subprocess.run(
        [sys.executable, *_child_flags(), os.path.abspath(__file__), "--arrow-partition",
         *ARROW_MODULES],
        check=False,
    ).returncode
    print(f"atx-vol-python[arrow]: exit {arrow_rc}")
atx_rc = int(pytest.main([*ATX_MODULES, *_PYTEST_COMMON]))
print(f"atx-vol-python[atxvol]: exit {atx_rc}")
raise SystemExit(atx_rc if atx_rc != 0 else arrow_rc)
