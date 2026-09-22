"""Operator entry point for a scheduled full-universe publication."""

from __future__ import annotations

import sys

from atx_db import cli


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    return cli.main(["publish-release", *args])


if __name__ == "__main__":
    raise SystemExit(main())
