"""A content-keyed JSON record store: a cache that is safe to delete at any time and never changes an output byte.

A record is ``ROOT/<kind>/<sha256 of canonical {kind, key}>.json`` holding ``{schema, kind, key, body,
content_sha256}``; the body keeps its key order (dicts round-trip in insertion order). ``get(kind, key)`` returns the body only when the file parses, names this schema and kind, carries
exactly this key (the whole key object, not just its hash) and its content SHA-256 re-hashes; anything else is a miss.
``put`` writes a temporary sibling, fsyncs it and renames it over the target, so a reader sees no record or a complete
one; a failed write is reported (False) and leaves no stray file. Floats round-trip exactly (``json`` writes ``repr``);
NaN is not representable, so callers store it as ``None``.

Used by the fitter's and the report card's per-candidate stores (``fit_composition_weights.py``,
``alpha_report_card.py``); the caller puts the producer fingerprint (``code_fingerprint``) and every input pin into the
key, so a hit and a recompute give the same body.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

SCHEMA = "atx.record-store/v1"


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def key_sha256(kind: str, key: dict) -> str:
    return hashlib.sha256(canonical({"kind": kind, "key": key})).hexdigest()


def content_sha256(kind: str, key: dict, body: dict) -> str:
    return hashlib.sha256(canonical({"schema": SCHEMA, "kind": kind, "key": key, "body": body})).hexdigest()


class RecordStore:
    def __init__(self, root: Path):
        self.root = Path(root)

    def path(self, kind: str, key: dict) -> Path:
        return self.root / kind / f"{key_sha256(kind, key)}.json"

    def get(self, kind: str, key: dict) -> dict | None:
        try:
            j = json.loads(self.path(kind, key).read_bytes())
        except (OSError, ValueError):
            return None
        if not (isinstance(j, dict) and j.get("schema") == SCHEMA and j.get("kind") == kind and j.get("key") == key
                and isinstance(j.get("body"), dict)):
            return None
        try:
            ok = j.get("content_sha256") == content_sha256(kind, key, j["body"])
        except ValueError:  # a non-finite float read back: not a record this store wrote
            return None
        return j["body"] if ok else None

    def put(self, kind: str, key: dict, body: dict) -> bool:
        # The file keeps the body's key order (a consumer may iterate a dict); the content SHA-256 is over the sorted
        # canonical form, so it does not depend on that order.
        record = {"schema": SCHEMA, "kind": kind, "key": key, "body": body,
                  "content_sha256": content_sha256(kind, key, body)}
        data = json.dumps(record, separators=(",", ":"), allow_nan=False).encode("utf-8")
        path = self.path(kind, key)
        partial = path.with_name(f"{path.name}.partial-{os.getpid()}")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with partial.open("wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(partial, path)
            return True
        except OSError:
            try:
                partial.unlink()
            except OSError:
                pass
            return False
