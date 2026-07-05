#!/usr/bin/env python
"""Fast local pytest runner for the atx-impl/db dev loop.

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
    "db/tests/test_import.py",
    "db/tests/test_migrations.py::test_migrations_ordered_ascending",
    "db/tests/test_migrations.py::test_migrations_unique_versions",
)

SPECIAL_TESTS: dict[str, tuple[str, ...]] = {
    "db/migrations.py": (
        "db/tests/test_migrations.py",
        "db/tests/test_migration_governance.py",
        "db/tests/test_schema_contract.py",
        "db/tests/test_schema_contract_quality_checks.py",
    ),
    "db/schema.py": ("db/tests/test_schema.py", "db/tests/test_migrations.py"),
    "db/schema_contract.py": (
        "db/tests/test_schema_contract.py",
        "db/tests/test_schema_contract_quality_checks.py",
        "db/tests/test_schema_contract_v2.py",
    ),
    "db/panel_contract.py": ("db/tests/test_schema_contract_v2.py",),
    "db/quality.py": (
        "db/tests/test_quality_smoke.py",
        "db/tests/test_quality_gating.py",
        "db/tests/test_schema_contract_quality_checks.py",
    ),
    "db/orchestrator.py": ("db/tests/test_orchestrator.py", "db/tests/test_jobs_dag.py"),
    "db/backfill.py": ("db/tests/test_backfill.py",),
    "db/jobs.py": ("db/tests/test_jobs_dag.py", "db/tests/test_warehouse_jobs_cli.py"),
    "db/warehouse.py": ("db/tests/test_schema_contract_v2.py", "db/tests/test_migrations.py"),
    "scripts/warehouse_backfill.py": ("db/tests/test_warehouse_backfill_cli.py",),
    "scripts/warehouse_jobs.py": ("db/tests/test_warehouse_jobs_cli.py",),
}

ALWAYS_WITH_CHANGED = ("db/tests/test_import.py",)


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


def _to_atx_impl_path(repo_path: str) -> str | None:
    repo_path = repo_path.replace("\\", "/")
    prefix = "atx-impl/"
    if not repo_path.startswith(prefix):
        return None
    return repo_path[len(prefix) :]


def _module_test_for(path: str, atx_impl: Path) -> str | None:
    if not path.startswith("db/") or not path.endswith(".py"):
        return None
    if path.startswith("db/tests/"):
        return path
    module = Path(path).stem
    test_path = atx_impl / "db" / "tests" / f"test_{module}.py"
    if test_path.exists():
        return test_path.relative_to(atx_impl).as_posix()
    return None


def _script_test_for(path: str, atx_impl: Path) -> str | None:
    if not path.startswith("scripts/") or not path.endswith(".py"):
        return None
    stem = Path(path).stem
    candidates = [
        atx_impl / "db" / "tests" / f"test_{stem}.py",
        atx_impl / "scripts" / "tests" / f"test_{stem}.py",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate.relative_to(atx_impl).as_posix()
    if stem.startswith("build_"):
        related = atx_impl / "db" / "tests" / f"test_{stem.removeprefix('build_')}.py"
        if related.exists():
            return related.relative_to(atx_impl).as_posix()
    return None


def _selected_tests(changed: list[str], atx_impl: Path) -> list[str]:
    tests: set[str] = set()
    db_or_script_change = False
    for repo_path in changed:
        path = _to_atx_impl_path(repo_path)
        if path is None:
            continue
        if not (path.startswith("db/") or path.startswith("scripts/")):
            continue
        db_or_script_change = True
        tests.update(SPECIAL_TESTS.get(path, ()))
        if module_test := _module_test_for(path, atx_impl):
            tests.add(module_test)
        if script_test := _script_test_for(path, atx_impl):
            tests.add(script_test)
        if path.startswith("db/seeds/"):
            tests.update(
                (
                    "db/tests/test_formula_library.py",
                    "db/tests/test_formula_registry_catalog.py",
                    "db/tests/test_migrations.py",
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
        if "-m pytest" in lower and "atx-impl" in lower and (repo_text in lower or cwd.startswith(repo_text)):
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
    atx_impl = repo_root / "atx-impl"
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
    parser.add_argument("--kill-stale", action="store_true", help="stop stale atx-impl pytest process trees first")
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
        tests = ["db/tests"]
        workers = args.workers or os.environ.get("ATX_DB_TEST_WORKERS", "4")
    elif args.smoke:
        tests = list(SMOKE_TESTS)
        workers = args.workers or os.environ.get("ATX_DB_TEST_WORKERS", "0")
    else:
        changed = _changed_files(repo_root, args.base)
        tests = _selected_tests(changed, atx_impl)
        workers = args.workers or os.environ.get("ATX_DB_TEST_WORKERS", "0")
        if not tests:
            print("No changed atx-impl DB/script tests selected. Use --smoke or --full if needed.")
            return 0

    command = _pytest_command(tests, workers, extra)
    print("Selected tests:")
    for test in tests:
        print(f"  {test}")
    print("Command:")
    print("  " + " ".join(command))
    if args.list:
        return 0
    return subprocess.call(command, cwd=atx_impl)


if __name__ == "__main__":
    raise SystemExit(main())
