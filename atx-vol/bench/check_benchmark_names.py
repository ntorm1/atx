#!/usr/bin/env python3
"""Fail when a Google Benchmark executable omits a required registered name."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence


_REGISTRATION_SUFFIX = re.compile(
    r"/(?:min_warmup_time|iterations|repetitions|repeats):|/(?:real_time|manual_time)(?:/|$)"
)


def registered_base_name(line: str) -> str:
    """Remove Google Benchmark policy metadata, retaining case dimensions."""
    stripped = line.strip()
    suffix = _REGISTRATION_SUFFIX.search(stripped)
    return stripped[: suffix.start()] if suffix is not None else stripped


def missing_required_names(list_output: str, required: Sequence[str]) -> list[str]:
    # Google Benchmark appends registration policy to list output. Strip that
    # framework-owned suffix while retaining real case dimensions such as
    # /threads:8, which are not an exact registration.
    registered = {
        registered_base_name(line) for line in list_output.splitlines() if line.strip()
    }
    return [name for name in required if name not in registered]


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, help="Google Benchmark executable")
    parser.add_argument(
        "--required",
        action="append",
        required=True,
        help="Exact registered benchmark name; repeat for every required name",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    completed = subprocess.run(
        [args.exe, "--benchmark_list_tests=true"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return completed.returncode

    missing = missing_required_names(completed.stdout, args.required)
    if missing:
        sys.stderr.write("missing required benchmark names:\n")
        for name in missing:
            sys.stderr.write(f"  {name}\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
