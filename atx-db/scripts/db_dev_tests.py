#!/usr/bin/env python
"""Fast local pytest runner for the atx-db development loop.

The full DB suite is still the release gate, but it is too heavy to run after
every edit. This helper maps changed DB files to the smallest useful pytest set
and forces a bounded worker count so local test runs do not saturate the box.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


SMOKE_TESTS = (
    "tests/test_import.py",
    "tests/test_migrations.py::test_migrations_ordered_ascending",
    "tests/test_migrations.py::test_migrations_unique_versions",
)

SPECIAL_TESTS: dict[str, tuple[str, ...]] = {
    "src/atx_db/migrations": (
        "tests/test_migrations.py",
        "tests/test_migration_governance.py",
        "tests/test_schema_contract.py",
        "tests/test_schema_contract_quality_checks.py",
    ),
    "src/atx_db/schema.py": ("tests/test_schema.py", "tests/test_migrations.py"),
    "src/atx_db/schema_contract.py": (
        "tests/test_schema_contract.py",
        "tests/test_schema_contract_quality_checks.py",
        "tests/test_schema_contract_v2.py",
    ),
    "src/atx_db/panel_contract.py": ("tests/test_schema_contract_v2.py",),
    "src/atx_db/quality": (
        "tests/test_quality_smoke.py",
        "tests/test_quality_gating.py",
        "tests/test_schema_contract_quality_checks.py",
    ),
    "src/atx_db/orchestrator.py": ("tests/test_orchestrator.py", "tests/test_jobs_dag.py"),
    "src/atx_db/backfill.py": ("tests/test_backfill.py",),
    "src/atx_db/jobs.py": ("tests/test_jobs_dag.py", "tests/test_warehouse_jobs_cli.py"),
    "src/atx_db/warehouse.py": ("tests/test_schema_contract_v2.py", "tests/test_migrations.py"),
    "scripts/warehouse_backfill.py": ("tests/test_warehouse_backfill_cli.py",),
    "scripts/warehouse_jobs.py": ("tests/test_warehouse_jobs_cli.py",),
}

ALWAYS_WITH_CHANGED = ("tests/test_import.py",)


def _run(
    args: list[str],
    *,
    cwd: Path,
    check: bool = True,
    capture: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def _repo_root() -> Path:
    result = _run(["git", "rev-parse", "--show-toplevel"], cwd=Path.cwd())
    return Path(result.stdout.strip())


def _git_lines(args: list[str], *, cwd: Path) -> list[str]:
    result = _run(["git", *args], cwd=cwd, check=False)
    if result.returncode != 0:
        return []
    return [line.strip().replace("\\", "/") for line in result.stdout.splitlines() if line.strip()]


def _current_branch(repo_root: Path) -> str:
    result = _run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=repo_root)
    return result.stdout.strip()


def _merge_base(repo_root: Path, base: str) -> str | None:
    result = _run(["git", "merge-base", base, "HEAD"], cwd=repo_root, check=False)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _changed_files(repo_root: Path, base: str | None) -> list[str]:
    changed: set[str] = set()
    if base:
        base_ref = _merge_base(repo_root, base) or base
        changed.update(_git_lines(["diff", "--name-only", f"{base_ref}...HEAD"], cwd=repo_root))
    changed.update(_git_lines(["diff", "--name-only"], cwd=repo_root))
    changed.update(_git_lines(["diff", "--cached", "--name-only"], cwd=repo_root))
    changed.update(_git_lines(["ls-files", "--others", "--exclude-standard"], cwd=repo_root))
    return sorted(changed)


def _to_project_path(repo_path: str) -> str | None:
    repo_path = repo_path.replace("\\", "/")
    prefix = "atx-db/"
    if not repo_path.startswith(prefix):
        return None
    return repo_path[len(prefix) :]


def _module_test_for(path: str, project_root: Path) -> str | None:
    if not path.startswith("src/atx_db/") or not path.endswith(".py"):
        return None
    module = Path(path).stem
    test_path = project_root / "tests" / f"test_{module}.py"
    if test_path.exists():
        return test_path.relative_to(project_root).as_posix()
    return None


def _script_test_for(path: str, project_root: Path) -> str | None:
    if not path.startswith("scripts/") or not path.endswith(".py"):
        return None
    stem = Path(path).stem
    candidates = [
        project_root / "tests" / f"test_{stem}.py",
        project_root / "scripts" / "tests" / f"test_{stem}.py",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate.relative_to(project_root).as_posix()
    if stem.startswith("build_"):
        related = project_root / "tests" / f"test_{stem.removeprefix('build_')}.py"
        if related.exists():
            return related.relative_to(project_root).as_posix()
    return None


def _selected_tests(changed: list[str], project_root: Path) -> list[str]:
    tests: set[str] = set()
    db_or_script_change = False
    for repo_path in changed:
        path = _to_project_path(repo_path)
        if path is None:
            continue
        if not (path.startswith("src/atx_db/") or path.startswith("tests/") or path.startswith("scripts/")):
            continue
        db_or_script_change = True
        for special_path, special_tests in SPECIAL_TESTS.items():
            if path == special_path or path.startswith(f"{special_path}/"):
                tests.update(special_tests)
        if path.startswith("tests/") and path.endswith(".py"):
            tests.add(path)
        if module_test := _module_test_for(path, project_root):
            tests.add(module_test)
        if script_test := _script_test_for(path, project_root):
            tests.add(script_test)
        if path.startswith("src/atx_db/seeds/"):
            tests.update(
                (
                    "tests/test_formula_library.py",
                    "tests/test_formula_registry_catalog.py",
                    "tests/test_migrations.py",
                )
            )
    if db_or_script_change:
        tests.update(ALWAYS_WITH_CHANGED)
    return sorted(tests)


def _pytest_command(tests: list[str], workers: str, extra_args: list[str]) -> list[str]:
    return [sys.executable, "-m", "pytest", *tests, "-n", workers, *extra_args]


def _kill_stale_pytest(repo_root: Path) -> None:
    try:
        import psutil
    except Exception as exc:  # pragma: no cover - best-effort operator aid
        print(f"psutil unavailable; cannot kill stale pytest workers: {exc}", file=sys.stderr)
        return
    repo_text = str(repo_root).lower()
    parents = []
    for proc in psutil.process_iter(["pid", "cmdline"]):
        try:
            cmd = " ".join(proc.info.get("cmdline") or [])
            cwd = proc.cwd().lower()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
        lower = cmd.lower()
        if "-m pytest" in lower and "atx-db" in lower and (repo_text in lower or cwd.startswith(repo_text)):
            parents.append(proc)
    for parent in parents:
        procs = [parent] + parent.children(recursive=True)
        for proc in procs:
            try:
                proc.terminate()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        _, alive = psutil.wait_procs(procs, timeout=5)
        for proc in alive:
            try:
                proc.kill()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
    if parents:
        print(f"Stopped {len(parents)} stale pytest parent process(es).")


def main(argv: list[str] | None = None) -> int:
    repo_root = _repo_root()
    project_root = repo_root / "atx-db"
    branch = _current_branch(repo_root)
    default_base = "main" if branch != "main" else None

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "pytest_args",
        nargs=argparse.REMAINDER,
        help="extra pytest args after --, e.g. -- -k semantic",
    )
    parser.add_argument("--base", default=default_base, help="base branch/ref for changed-test selection")
    parser.add_argument("--smoke", action="store_true", help="run a tiny import/registry smoke set")
    parser.add_argument("--full", action="store_true", help="run the full db/tests suite explicitly")
    parser.add_argument("--list", action="store_true", help="print selected tests without running them")
    parser.add_argument("--kill-stale", action="store_true", help="stop stale atx-db pytest process trees first")
    parser.add_argument(
        "--workers",
        default=None,
        help="pytest-xdist worker count. Default: 0 for changed/smoke, 4 for --full.",
    )
    args = parser.parse_args(argv)

    if args.kill_stale:
        _kill_stale_pytest(repo_root)

    extra = args.pytest_args
    if extra and extra[0] == "--":
        extra = extra[1:]

    if args.full:
        tests = ["tests"]
        workers = args.workers or os.environ.get("ATX_DB_TEST_WORKERS", "4")
    elif args.smoke:
        tests = list(SMOKE_TESTS)
        workers = args.workers or os.environ.get("ATX_DB_TEST_WORKERS", "0")
    else:
        changed = _changed_files(repo_root, args.base)
        tests = _selected_tests(changed, project_root)
        workers = args.workers or os.environ.get("ATX_DB_TEST_WORKERS", "0")
        if not tests:
            print("No changed atx-db tests selected. Use --smoke or --full if needed.")
            return 0

    command = _pytest_command(tests, workers, extra)
    print("Selected tests:")
    for test in tests:
        print(f"  {test}")
    print("Command:")
    print("  " + " ".join(command))
    if args.list:
        return 0
    return subprocess.call(command, cwd=project_root)


if __name__ == "__main__":
    raise SystemExit(main())
