"""The suite command of scripts/tests under two string-hash seeds (review F-7, P9 P0-FIX; root's R0-2 step).

A wave test once passed or failed by PYTHONHASHSEED (set-order path masking, integration log 6526, fix 3a146fe7). The
suite therefore runs twice, under PYTHONHASHSEED=0 and PYTHONHASHSEED=1, and passes only when both runs pass:

  "C:/Program Files/Python312/python.exe" scripts/tests/run_two_seeds.py
  "C:/Program Files/Python312/python.exe" scripts/tests/run_two_seeds.py atx-engine/tools atx-impl/tools

Arguments are the pytest paths (default scripts/tests), relative to the repository root, where pytest runs with
``-q -p no:cacheprovider``. The same two runs by hand (bash):

  PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests
  PYTHONHASHSEED=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests

Exit code: 0 when every run passed, else the first failing run's pytest exit code. Not collected by pytest (the file
name is not test_*).
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
SEEDS = ("0", "1")
DEFAULT_PATHS = ("scripts/tests",)


def runs(paths: list[str] | tuple[str, ...], python: str = sys.executable) -> list[tuple[dict, list[str]]]:
    """(environment overrides, argv) of each pytest run: one per seed, in SEEDS order."""
    argv = [python, "-m", "pytest", "-q", "-p", "no:cacheprovider", *(paths or DEFAULT_PATHS)]
    return [({"PYTHONHASHSEED": seed}, list(argv)) for seed in SEEDS]


def main(argv: list[str] | None = None) -> int:
    paths = list(sys.argv[1:] if argv is None else argv) or list(DEFAULT_PATHS)
    first_bad = 0
    for env, cmd in runs(paths):
        print(f"== PYTHONHASHSEED={env['PYTHONHASHSEED']} {' '.join(cmd[1:])}", flush=True)
        code = subprocess.run(cmd, cwd=REPO, env=dict(os.environ, **env)).returncode
        print(f"== PYTHONHASHSEED={env['PYTHONHASHSEED']}: exit {code}", flush=True)
        first_bad = first_bad or code
    return first_bad


if __name__ == "__main__":
    sys.exit(main())
