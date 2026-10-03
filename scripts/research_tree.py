"""Shared helpers of research_cycle.py and run_bounded_research.py (platform v8 lane A, task A-3).

* The clean-tree rule is scoped to the code: ``CODE_PATHSPEC`` (atx-core, atx-tsdb, atx-engine, atx-impl, scripts and
  the top-level CMake files). A dirty path outside it (a lane report under .superpowers/, a research output) does not
  stop a cycle; it is listed in the receipt instead.
* ``--no-git`` (contract K3) is accepted only for a root outside any git repository (test roots, e.g. the tiny_world
  fixture): ``repo_root_of`` walks up from the root looking for a ``.git`` entry.
* ``window_id`` names the research window (task W0-1, ``atx.research-window/v2``) for the derived cache and fit roots.
* ``RUNNER_MAX_SECONDS`` is the bounded runner's hard time cap (run_bounded_research.py refuses more): a wave
  manifest, a cycle spec and a wave step refuse a phase cap above it when they are loaded or built (P9 OR-1), never
  half way through a wave.
* ``argv_sha256`` is the one digest of a command's arguments after its executable (contract K-P9-10: the bounded
  runner's receipt, research_cycle's resume check and NAV binding); ``BUILD_TYPES`` maps a cycle spec's "build" to the
  CMake build type the runner records.
* ``launch_refusal`` checks the launch admission options (a bounded wait, a host memory budget: P9 F-5 (a), OR
  section 5) wherever they are given: the runner, research_cycle and a wave manifest's driver block.

Standard library only (the bounded runner imports it before psutil is needed).
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
CODE_PATHSPEC = ("atx-core", "atx-tsdb", "atx-engine", "atx-impl", "scripts", "CMakeLists.txt", "CMakePresets.json",
                 "cmake")
WINDOW_JSON = "atx-impl/strategies/research_window.json"     # W0-1: the one source of the research window
RUNNER_MAX_SECONDS = 600                                     # run_bounded_research.py's hard time cap (P9 OR-1)
BUILD_TYPES = {"equity": "Debug", "equity-rel": "Release"}  # research_cycle's spec "build" -> the CMake build type
ADMISSION_MAX_SECONDS = 3600     # the runner's --admission-wait-seconds bound (P9 F-5 (a))
HOST_BUDGET_MIB = (64, 65536)    # the runner's --host-budget-mib range (P9 OR section 5)


def launch_refusal(wait, budget) -> str | None:
    """Why launch admission options are refused: ``wait`` (seconds) in (0, ADMISSION_MAX_SECONDS], ``budget`` (MiB, an
    integer in HOST_BUDGET_MIB) only with a wait; None = accepted (either may be None: not given)."""
    if wait is not None and not (type(wait) in (int, float) and 0 < wait <= ADMISSION_MAX_SECONDS):
        return f"the launch admission wait must be in (0, {ADMISSION_MAX_SECONDS}] seconds"
    if budget is not None and (wait is None or type(budget) is not int or
                               not HOST_BUDGET_MIB[0] <= budget <= HOST_BUDGET_MIB[1]):
        return (f"the host memory budget needs a launch admission wait and is an integer in [{HOST_BUDGET_MIB[0]}, "
                f"{HOST_BUDGET_MIB[1]}] MiB")
    return None


def argv_sha256(args) -> str:
    """SHA-256 of a command's arguments after its executable (a compact JSON list of strings): the bounded runner's
    receipt ``argv_sha256`` (contract K-P9-10) and cycle_resume's NAV binding (review C-13) use this one rule."""
    return hashlib.sha256(json.dumps([str(a) for a in args], separators=(",", ":")).encode()).hexdigest()


def seconds_cap_refusal(key: str, value) -> str | None:
    """Why a runner time cap ``value`` at ``key`` (a dotted manifest or spec key) is refused: above
    RUNNER_MAX_SECONDS, the bounded runner would exit 2 with no receipt; None when it is within the cap (a number's
    other checks stay the caller's)."""
    if type(value) in (int, float) and value > RUNNER_MAX_SECONDS:
        return (f"{key} {value} is above the bounded runner's maximum {RUNNER_MAX_SECONDS} s "
                "(research_tree.RUNNER_MAX_SECONDS; run_bounded_research.py refuses it)")
    return None


def repo_root_of(path: Path) -> Path | None:
    """The enclosing git work tree of `path` (a directory holding `.git`, a dir or a worktree's file), else None."""
    p = Path(path).resolve()
    for candidate in (p, *p.parents):
        if (candidate / ".git").exists():
            return candidate
    return None


def no_git_refusal(root: Path) -> str | None:
    """Why --no-git is refused for `root` (None: accepted, the root is outside any git repository)."""
    top = repo_root_of(root)
    if top is None:
        return None
    return (f"--no-git is only for a root outside any git repository; {Path(root).resolve()} is inside {top} "
            "(commit and run without --no-git)")


def _porcelain(root: Path, pathspec: tuple[str, ...] | None) -> list[str]:
    argv = ["git", "status", "--porcelain"]
    if pathspec is not None:
        argv += ["--", *pathspec]
    done = subprocess.run(argv, cwd=root, capture_output=True, text=True, check=True)
    return [line[3:] for line in done.stdout.splitlines() if line.strip()]


def dirty_paths(root: Path) -> tuple[list[str], list[str]]:
    """(blocking, ignored): dirty paths inside the code pathspec, and dirty paths outside it (listed, not a stop).
    Raises OSError / CalledProcessError when git cannot answer (the caller treats that as not clean)."""
    blocking = _porcelain(root, CODE_PATHSPEC)
    everything = _porcelain(root, None)
    return blocking, [p for p in everything if p not in set(blocking)]


def window_id() -> str:
    """The research window id (W0-1): research_window.WINDOW_ID when the module exports it, else derived from the
    window JSON's schema (``atx.research-window/v2`` -> ``research-window-v2``). Raises LookupError when the window
    source is absent (W0-1 not merged): a derived root is never guessed."""
    try:
        tools = str(REPO / "atx-engine" / "tools")
        if tools not in sys.path:
            sys.path.insert(0, tools)
        import research_window  # type: ignore  # noqa: PLC0415  (W0-1)
        wid = getattr(research_window, "WINDOW_ID", None)
        doc = research_window.load() if wid is None else None
    except ImportError:
        wid, doc = None, None
        path = REPO / WINDOW_JSON
        if path.is_file():
            doc = json.loads(path.read_text(encoding="utf-8"))
    if wid is None and isinstance(doc, dict):
        wid = doc.get("id") or doc.get("window_id")
        schema = doc.get("schema", "")
        if wid is None and isinstance(schema, str) and schema.startswith("atx.") and "/" in schema:
            wid = schema[len("atx."):].replace("/", "-")
    if not isinstance(wid, str) or not wid:
        raise LookupError(f"no research window id: {WINDOW_JSON} / atx-engine/tools/research_window.py (task W0-1) "
                          "are absent")
    return wid
