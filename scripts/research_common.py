"""Helpers the research scripts share (P9 lane E2, orchestration review section 4: one copy each instead of the
duplicates the review lists):

  sha256_file      a file's SHA-256 (research_cycle, research_spec, run_bounded_research, wave_manifest; the copies in
                   atx-engine/tools/stage_chain.py and atx-impl/tools/backtest_integrity.py stay: those directories
                   never import scripts/)
  fmt_argv         one command line as research_cycle and the wave print it
  execute          the process executor of the cycle (capture per call) and of the wave (always captured)
  run_dir_of       the bounded runner's run dir of an output base, <base>-run[<k>] (RUN_SUFFIX): research_cycle names
                   every run dir with it and the wave's seal scan and timings find them with it (OR section 4: one
                   naming, so a rename cannot shrink the scan); free_run_dir allocates the first unused one
  dirty_or_error   the code pathspec's dirty paths (research_tree.dirty_paths) or the git failure, for the cycle's clean
                   check and the wave's
  read_ledger      a trial ledger's lines and chain head, the chain verified; ledger_view adds {path, lines, head, n}
                   (the wave's preflight and record, the scoreboard)
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess

import research_ledger
import research_tree

RUN_SUFFIX = "-run"


def fmt_argv(argv: list[str]) -> str:
    """One command line: arguments joined by a space, an argument holding a space double-quoted (bash DRY format)."""
    return " ".join(f'"{a}"' if " " in a else a for a in argv)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def execute(argv: list[str], root: Path, env: dict | None = None, capture: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=root, env=env, capture_output=capture, text=True)


def run_dir_of(base: str, k: int | None = None) -> str:
    """The run dir of attempt (or pass) ``k`` of the output ``base``: <base>-run, or <base>-run<k>."""
    return f"{base}{RUN_SUFFIX}" if k is None else f"{base}{RUN_SUFFIX}{k}"


def free_run_dir(base: str, exists) -> str:
    """<base>-run<k>, k the first number from 1 whose dir ``exists(path)`` denies (the runner never reuses a dir)."""
    k = 1
    while exists(run_dir_of(base, k)):
        k += 1
    return run_dir_of(base, k)


def dirty_or_error(root: Path) -> tuple[list[str], list[str], str | None]:
    """(blocking, ignored, None): the dirty paths inside and outside the code pathspec (research_tree.dirty_paths); or
    ([], [], the error) when git fails."""
    try:
        blocking, ignored = research_tree.dirty_paths(root)
    except (OSError, subprocess.CalledProcessError) as exc:
        return [], [], str(exc)
    return blocking, ignored, None


def read_ledger(path: Path) -> tuple[list[dict], str]:
    """(every line, the chain head) of the trial ledger at ``path``, its chain verified (backtest_integrity
    .ledger_read, .ledger_head; OSError / ValueError propagate)."""
    bi = research_ledger.backtest_integrity()
    return bi.ledger_read(path), bi.ledger_head(path)


def ledger_view(path: Path, rel: str) -> tuple[list[dict], dict]:
    """(the lines, {path: rel, lines, head, n}) of the trial ledger: ``read_ledger`` and its N with every scored cell
    in it (backtest_integrity.ledger_n(records, True))."""
    records, head = read_ledger(path)
    return records, {"path": rel, "lines": len(records), "head": head,
                     "n": research_ledger.backtest_integrity().ledger_n(records, True)}
