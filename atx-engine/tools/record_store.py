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

SQLite index (P9 SQL1, sql-design sections 3.8 and 3.10; ruling SQL-6). A store selects the SQLite backend when a
cache index ``index.sqlite`` exists in its root (partition ``root = ""``) or in its root's parent (partition
``root = <root directory name>``), checked once at construction; otherwise every line above holds unchanged. The index
is created only by C++ (``atx-research-store cache init``), never here, and carries the ``cache`` schema
(``research_store.py`` reads it). In SQLite mode:
* the process prints ``record_store: index sqlite <posix path>`` to stderr once per index file (the selection
  evidence until the run receipt records it, ruling SQL-11);
* ``get`` reads the ``record`` row, requires its ``key`` text to equal the compact canonical key and re-hashes
  ``content_sha256`` (the rule above); a miss (or a row that fails a check) reads the legacy JSON file with today's
  checks and, when it is valid, imports it into the index (read-through);
* ``put`` stores the body compact in insertion order (bodies over 1 MiB in ``objects/<sha[0:2]>/<sha>.json`` beside
  the index, tmp + fsync + rename) in one ``BEGIN IMMEDIATE``; a same-key row with another ``content_sha256`` returns
  False; no JSON record file is written;
* one connection per thread per index file; every failure is a miss (None) or False, as for files.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import sqlite3
import sys
import threading
from pathlib import Path

SCHEMA = "atx.record-store/v1"
INDEX_NAME = "index.sqlite"
INLINE_BODY_MAX = 1 << 20  # bytes; larger bodies are files under objects/ (sql-design section 3.8)

_ANNOUNCED: set = set()
_ANNOUNCE_LOCK = threading.Lock()
_THREAD = threading.local()
# What an index operation may raise: SQLite and file errors, refused values / documents (research_store.StoreError is
# a ValueError), and a missing accessor module. All of them are a miss or a failed put, never a crash.
_FAILURES = (sqlite3.Error, OSError, ValueError, TypeError, ImportError)


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def key_sha256(kind: str, key: dict) -> str:
    return hashlib.sha256(canonical({"kind": kind, "key": key})).hexdigest()


def content_sha256(kind: str, key: dict, body: dict) -> str:
    return hashlib.sha256(canonical({"schema": SCHEMA, "kind": kind, "key": key, "body": body})).hexdigest()


def _research_store():
    """The generic accessor beside this file (imported only in SQLite mode)."""
    try:
        import research_store
    except ImportError:
        spec = importlib.util.spec_from_file_location("research_store", Path(__file__).with_name("research_store.py"))
        research_store = importlib.util.module_from_spec(spec)
        sys.modules.setdefault("research_store", research_store)
        spec.loader.exec_module(research_store)
    return research_store


def close_indexes() -> None:
    """Close the index connections this thread opened (they are otherwise kept for the thread's life). Call before
    deleting a directory that holds an index the thread used (Windows keeps open files undeletable)."""
    stores = getattr(_THREAD, "stores", None) or {}
    for store in stores.values():
        store.close()
    stores.clear()


def _announce(index: Path) -> None:
    text = index.resolve().as_posix()
    with _ANNOUNCE_LOCK:
        if text in _ANNOUNCED:
            return
        _ANNOUNCED.add(text)
    print(f"record_store: index sqlite {text}", file=sys.stderr, flush=True)


class RecordStore:
    def __init__(self, root: Path):
        self.root = Path(root)
        self._index: Path | None = None
        self._partition = ""
        if (self.root / INDEX_NAME).is_file():
            self._index = self.root / INDEX_NAME
        elif (self.root.parent / INDEX_NAME).is_file():
            self._index, self._partition = self.root.parent / INDEX_NAME, self.root.name
        if self._index is not None:
            _announce(self._index)

    def path(self, kind: str, key: dict) -> Path:
        return self.root / kind / f"{key_sha256(kind, key)}.json"

    def get(self, kind: str, key: dict) -> dict | None:
        if self._index is None:
            return self._file_get(kind, key)
        return self._index_get(kind, key)

    def put(self, kind: str, key: dict, body: dict) -> bool:
        if self._index is None:
            return self._file_put(kind, key, body)
        return self._index_put(kind, key, body)

    # -- JSON files (no index: exactly the pre-SQLite store) ---------------------------------------------------------
    def _file_get(self, kind: str, key: dict) -> dict | None:
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

    def _file_put(self, kind: str, key: dict, body: dict) -> bool:
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

    # -- SQLite index ------------------------------------------------------------------------------------------------
    def _store(self):
        """This thread's open index (one connection per thread per index file)."""
        stores = getattr(_THREAD, "stores", None)
        if stores is None:
            stores = _THREAD.stores = {}
        name = str(self._index)
        store = stores.get(name)
        if store is None:
            rs = _research_store()
            store = rs.Store.open(self._index)
            if store.schema.get("db") != "cache" or "record" not in store.tables:
                store.close()
                raise rs.StoreError(f"{self._index} is not a cache index")
            stores[name] = store
        return store

    def _row_key(self, kind: str, key_sha: str) -> dict:
        return {"root": self._partition, "kind": kind, "key_sha256": key_sha}

    def _row_body(self, row: dict, kind: str, key: dict, compact_key: str) -> dict | None:
        try:
            if row["key"] != compact_key:
                return None
            if row["body"] is not None:
                text = row["body"]
            else:
                data = (self._index.parent / row["body_file"]).read_bytes()
                if len(data) != row["body_bytes"]:
                    return None
                text = data.decode("utf-8")
            body = json.loads(text)
            if not isinstance(body, dict):
                return None
            ok = row["content_sha256"] == content_sha256(kind, key, body)
        except (OSError, ValueError, TypeError):
            return None
        return body if ok else None

    def _index_get(self, kind: str, key: dict) -> dict | None:
        key_sha = key_sha256(kind, key)  # raises for an unserialisable key, as the file path does
        compact_key = canonical(key).decode("utf-8")
        store = None
        try:
            store = self._store()
            row = store.get("record", self._row_key(kind, key_sha))
        except _FAILURES:  # an unreadable index is a miss: fall through to the legacy file
            row = None
        if row is not None:
            body = self._row_body(row, kind, key, compact_key)
            if body is not None:
                return body
        body = self._file_get(kind, key)
        if body is not None and store is not None:
            try:
                self._write_row(store, kind, key_sha, compact_key, body, content_sha256(kind, key, body))
            except _FAILURES:  # the import is best effort; the verified legacy body is still the answer
                pass
        return body

    def _index_put(self, kind: str, key: dict, body: dict) -> bool:
        content = content_sha256(kind, key, body)  # raises for NaN / unserialisable input, as the file put does
        key_sha = key_sha256(kind, key)
        compact_key = canonical(key).decode("utf-8")
        try:
            return self._write_row(self._store(), kind, key_sha, compact_key, body, content)
        except _FAILURES:
            return False

    def _write_row(self, store, kind: str, key_sha: str, compact_key: str, body: dict, content: str) -> bool:
        body_text = json.dumps(body, separators=(",", ":"), allow_nan=False)
        data = body_text.encode("utf-8")
        row = {"root": self._partition, "kind": kind, "key_sha256": key_sha, "key": compact_key, "body": None,
               "body_file": None, "body_bytes": len(data), "content_sha256": content}
        if len(data) > INLINE_BODY_MAX:
            relative = f"objects/{content[:2]}/{content}.json"
            _publish(self._index.parent / relative, data)
            row["body_file"] = relative
        else:
            row["body"] = body_text
        with store.transaction():
            existing = store.get("record", self._row_key(kind, key_sha))
            if existing is not None:
                return existing["content_sha256"] == content
            store.insert("record", row)
        return True


def _publish(path: Path, data: bytes) -> None:
    """Write ``data`` at ``path`` atomically (tmp + fsync + rename). The name is the content SHA-256, so an existing
    file of the same size is the same object and is kept."""
    try:
        if path.stat().st_size == len(data):
            return
    except OSError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(f"{path.name}.partial-{os.getpid()}-{threading.get_ident()}")
    try:
        with partial.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(partial, path)
    except OSError:
        try:
            partial.unlink()
        except OSError:
            pass
        raise
