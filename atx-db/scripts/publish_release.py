"""Operator entry point for a scheduled full-universe publication.

Delegates to ``atx_db.cli publish-release`` (read-only pending-migration preflight, bounded
session), which prints one JSON line including ``eligibility`` and ``gates_not_passed``.
Exit codes: 0 = ``eligible``; ``atx_db.publication.CANDIDATE_EXIT_CODE`` (3) = written, but
only a ``candidate``; 1 = error (nothing published); 2 = usage error.
"""

from __future__ import annotations

import sys

from atx_db import cli


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    return cli.main(["publish-release", *args])


if __name__ == "__main__":
    raise SystemExit(main())
