"""Helpers the research scripts share (P9 lane E2, orchestration review section 4: one copy each instead of the
duplicates the review lists): file SHA-256, the bash-DRY command line."""
from __future__ import annotations

import hashlib
from pathlib import Path


def fmt_argv(argv: list[str]) -> str:
    """One command line: arguments joined by a space, an argument holding a space double-quoted (bash DRY format)."""
    return " ".join(f'"{a}"' if " " in a else a for a in argv)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
