"""Fixture writers for lake tests: publish a stage's Parquet files and a contract manifest under a temp root."""

from __future__ import annotations

import hashlib
import json
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from .contract import sha256_file


def write_table(root: Path, rel: str, columns: Mapping[str, list[Any]]) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table(dict(columns)), path)
    return path


def publish(root: Path, manifest_rel: str, schema: str = "atx.test/v1", code: Mapping[str, Path] | None = None,
            inputs: Mapping[str, str] | None = None, extra: Mapping[str, Any] | None = None,
            pattern: str = "**/*.parquet") -> Path:
    """Write ``manifest_rel`` binding every file under its directory matching ``pattern`` (as write_stage_manifest),
    the code identity of ``code`` ({file name: path}) and ``input_manifests_sha256`` = ``inputs``."""
    mpath = root / manifest_rel
    base = mpath.parent
    base.mkdir(parents=True, exist_ok=True)
    files = {p.relative_to(base).as_posix(): {"bytes": p.stat().st_size, "sha256": sha256_file(p)}
             for p in sorted(base.glob(pattern)) if p.is_file() and not p.name.endswith(".partial")}
    ident: dict[str, Any] = {}
    for name, p in (code or {}).items():
        blob = Path(p).read_bytes()
        ident[name] = {"sha256": hashlib.sha256(blob).hexdigest(),
                       "sha256_lf": hashlib.sha256(blob.replace(b"\r\n", b"\n")).hexdigest()}
    ident["git_head"] = None
    manifest = {"schema": schema, "status": "complete", "stage": base.name, "code": ident, "files": files,
                **({"input_manifests_sha256": dict(inputs)} if inputs is not None else {}), **dict(extra or {})}
    mpath.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    return mpath


def bind(root: Path, *manifest_rels: str) -> dict[str, str]:
    return {rel: sha256_file(root / rel) for rel in manifest_rels}
