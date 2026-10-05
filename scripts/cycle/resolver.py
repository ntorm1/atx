"""Root-relative files of a cycle (research_cycle.py, split move-only into scripts/cycle/ by P9 lane E2): existence,
SHA-256 (cached) and JSON under --root, and the placeholder of a pin not yet computable."""
from __future__ import annotations

import json
from pathlib import Path

from research_common import sha256_file


# ------------------------------------------------------------------ files under the root
class Resolver:
    """Root-relative files: existence, SHA-256 (cached) and JSON. ``known`` = {relpath: sha256} stands in for files
    that are not on disk (hash-only mode for fixtures: content checks are skipped and flagged)."""

    def __init__(self, root: Path, known: dict | None = None):
        self.root = Path(root)
        self.known = dict(known or {})
        self._sha: dict = {}

    def path(self, rel: str) -> Path:
        p = Path(rel)
        return p if p.is_absolute() else self.root / p

    def exists(self, rel: str) -> bool:
        return rel in self.known or self.path(rel).is_file()

    def exists_dir(self, rel: str) -> bool:
        pre = rel.rstrip("/") + "/"
        return self.path(rel).is_dir() or any(k.startswith(pre) for k in self.known)

    def sha(self, rel: str) -> str | None:
        if rel in self.known:
            return self.known[rel]
        if rel not in self._sha:  # only existing files are cached: outputs appear while a cycle runs
            p = self.path(rel)
            if not p.is_file():
                return None
            self._sha[rel] = sha256_file(p)
        return self._sha[rel]

    def read_json(self, rel: str):
        p = self.path(rel)
        if not p.is_file():
            return None
        return json.loads(p.read_text(encoding="utf-8"))

    def hash_only(self, rel: str) -> bool:
        return rel in self.known and not self.path(rel).is_file()


def placeholder(rel: str) -> str:
    return f"<sha256:{rel}>"
