"""Authenticated preparation units under a quiescent, pinned source contract.

Writers must not modify the approved sources during preparation/publication.
Hashes stream in bounded memory; quarter checks never hash unrelated quarters.
"""
from __future__ import annotations

import hashlib
import json
import os
import uuid
from pathlib import Path

import pyarrow.parquet as pq

from . import common

VERSION = "fund-prepare-cache-v2"


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def identity(path: Path) -> dict:
    return {"sha256": digest(path), "bytes": path.stat().st_size}


class SourceSnapshot:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.manifest = self.root / "fsds-staging-manifest.json"
        self.manifest_identity = identity(self.manifest)
        metadata = common.read_json(self.manifest)
        self.pins = {}
        for quarter, entry in metadata["quarters"].items():
            for table, info in entry["tables"].items():
                rel = f"{table}/{quarter}.parquet"
                if info["path"] != rel or rel in self.pins:
                    raise ValueError("invalid or duplicate FSDS manifest path")
                pin = {"sha256": info["parquet_sha256"], "bytes": info["parquet_bytes"]}
                if len(pin["sha256"]) != 64 or not isinstance(pin["bytes"], int):
                    raise ValueError("missing FSDS byte pins")
                self.pins[rel] = pin
        self.verify()

    def verify(self, paths=None):
        if identity(self.manifest) != self.manifest_identity:
            raise ValueError("FSDS manifest drift")
        actual = {p.relative_to(self.root).as_posix() for p in self.root.glob("*/*.parquet")}
        if actual != set(self.pins):
            raise ValueError("FSDS inventory drift")
        for rel in self.pins if paths is None else paths:
            if rel not in self.pins or identity(self.root / rel) != self.pins[rel]:
                raise ValueError(f"FSDS source byte drift: {rel}")

    def binding(self):
        return {"root": self.root.as_posix(), "manifest": self.manifest_identity, "files": self.pins}


class PreparationCache:
    def __init__(self, source: SourceSnapshot, code: dict):
        self.source = source
        self.binding = {"version": VERSION, "source": source.binding(), "code": code}

    def verify_units(self, receipt, required):
        """Certify exact required units, their complete sidecars and live dependencies."""
        units = receipt.get("preparation_units")
        if not isinstance(units, dict) or set(units) != set(required):
            raise ValueError("incomplete or extra preparation inventory")
        identities = {}
        def live(path):
            if path not in identities:
                identities[path] = identity(Path(path))
            return identities[path]
        for path, spec in required.items():
            unit = units[path]
            side = common.read_json(Path(path).with_name(Path(path).name + ".prepare.json"))
            dependencies = {p: live(p) for p in spec["dependencies"]}
            sql = spec["sql"](units) if callable(spec["sql"]) else spec["sql"]
            expected = {**self.binding, "sql": sql, "key": spec["key"],
                        "dependencies": dependencies, "sources": list(spec["sources"]),
                        "options": spec["options"]}
            if (not isinstance(unit, dict) or unit.get("complete") is not True
                    or side != unit or unit.get("binding") != expected):
                raise ValueError(f"uncertified preparation unit or dependency drift: {path}")
            if unit.get("output") != live(path):
                raise ValueError(f"preparation output drift: {path}")
            if (type(unit.get("rows")) is not int
                    or unit["rows"] != pq.ParquetFile(path).metadata.num_rows):
                raise ValueError(f"preparation row mismatch: {path}")
        return {**self.binding, "units": units}

    def copy(self, con, sql, dest, receipt, key, *, sources=(), dependencies=(), **kw):
        receipt.setdefault("cached", [])
        sources = tuple(sources)
        dependencies = tuple(Path(p) for p in dependencies)
        dependency_pins = {p.as_posix(): identity(p) for p in dependencies}
        binding = {**self.binding, "sql": sql, "key": key,
                   "dependencies": dependency_pins, "sources": list(sources), "options": kw}
        receipt.setdefault("preparation_units", {})
        side = dest.with_name(dest.name + ".prepare.json")
        self.source.verify(sources)
        try:
            old = common.read_json(side)
            if (old["complete"] is True and old["binding"] == binding
                    and old["output"] == identity(dest)
                    and old["rows"] == pq.ParquetFile(dest).metadata.num_rows):
                self.source.verify(sources)
                if any(identity(p) != dependency_pins[p.as_posix()] for p in dependencies):
                    raise ValueError("preparation dependency drift")
                receipt["cached"].append(key)
                receipt["preparation_units"][dest.as_posix()] = old
                return old["rows"]
        except (OSError, ValueError, KeyError, TypeError):
            pass
        # Each attempt owns a unique candidate. Interrupted candidates and old
        # receipts survive; no legacy .sha256 sidecar can authenticate a unit.
        token = uuid.uuid4().hex
        candidate = dest.with_name(dest.name + f".attempt-{token}")
        rows = common.copy_to_parquet(con, sql, candidate, **kw)
        self.source.verify(sources)
        if any(identity(p) != dependency_pins[p.as_posix()] for p in dependencies):
            raise ValueError("preparation dependency drift")
        output = identity(candidate)
        if rows != pq.ParquetFile(candidate).metadata.num_rows:
            raise ValueError("preparation output row mismatch")
        # Archive before replace. If interrupted between output and receipt,
        # the old receipt cannot validate the new output on restart.
        for old_path in (dest, side):
            if old_path.exists():
                os.replace(old_path, old_path.with_name(old_path.name + f".previous-{token}"))
        os.replace(candidate, dest)
        unit = {"complete": True, "binding": binding, "output": output, "rows": rows}
        common.write_json_atomic(side, unit)
        receipt["preparation_units"][dest.as_posix()] = unit
        return rows
