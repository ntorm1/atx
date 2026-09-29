"""``lake verify``: invariant checker over every registered stage (plan section 3, task S1.2).

Per stage, against its manifest and the registry entry:

* FAIL ``missing_manifest`` / ``unreadable_manifest`` / ``incomplete_manifest`` (no ``schema``, ``status`` other
  than ``complete``, no ``code``, no ``files``) / ``schema_mismatch`` (manifest schema differs from the registry);
* FAIL ``file_missing`` / ``size_mismatch`` / ``sha_mismatch``: a file the manifest lists is absent or differs;
* FAIL ``unbound_file``: a file matched by a registered output glob is not in the manifest (the catalog would
  expose unverified bytes); ``output_missing``: a registered output glob matches nothing;
* FAIL ``clock_missing``: an output with a clock lacks its clock column in some Parquet file; ``clock_future``:
  its maximum (row-group statistics, else a streamed pyarrow scan; verify never opens DuckDB) lies after ``now``; WARN ``legacy_clock``: the clock column is not
  ``available_at``;
* FAIL ``partial_file``: a ``.partial`` file (an interrupted atomic write) under the stage's directories;
* STALE ``input_changed`` / ``input_missing``: a registered input's manifest SHA-256 differs from the binding the
  manifest recorded (``input_manifests_sha256``, a legacy layout, or the orchestrator's binding sidecar for exactly
  this manifest); WARN ``input_unbound``: no binding recorded for a registered input.

Status per stage: FAIL > STALE > WARN > OK; ``PLANNED`` for a registered stage not yet published. The root check
adds FAIL ``unregistered_manifest`` / ``unregistered_file`` for manifests and Parquet files no entry covers.

    python -m atx_db.lake verify [--stage S ...] [--root R] [--no-hash] [--json OUT]

Exit code 1 when any FAIL, 2 when STALE (no FAIL), else 0.
"""

from __future__ import annotations

import datetime as dt
import json
import os
from collections.abc import Iterable, Mapping
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from .contract import (
    REQUIRED_MANIFEST_KEYS,
    Stage,
    manifest_sha,
    output_files,
    read_manifest,
    recorded_files,
    recorded_inputs,
    sha256_file,
)

SIDECAR_DIR = "_lake/bindings"
RANK = {"OK": 0, "PLANNED": 0, "WARN": 1, "STALE": 2, "FAIL": 3}


@dataclass
class Finding:
    kind: str  # FAIL | STALE | WARN
    code: str
    detail: str


@dataclass
class StageResult:
    stage: str
    status: str = "OK"
    manifest: str = ""
    manifest_sha256: str | None = None
    binding_basis: str = "none"
    files_checked: int = 0
    bytes_hashed: int = 0
    findings: list[Finding] = field(default_factory=list)

    def add(self, kind: str, code: str, detail: str) -> None:
        self.findings.append(Finding(kind, code, detail))
        if RANK[kind] > RANK[self.status]:
            self.status = kind


def sidecar_path(root: Path, stage: str) -> Path:
    return root / SIDECAR_DIR / f"{stage}.json"


def input_binding(stage: Stage, manifest: Mapping[str, Any], root: Path, manifest_sha256: str | None
                  ) -> tuple[dict[str, str], str]:
    """Recorded input bindings of the published manifest and their basis (manifest, legacy, orchestrator, none)."""
    got, basis = recorded_inputs(manifest, root)
    if basis == "none":
        side = read_manifest(sidecar_path(root, stage.name))
        if side and side.get("manifest_sha256") == manifest_sha256 and isinstance(side.get("input_manifests_sha256"), dict):
            return dict(side["input_manifests_sha256"]), "orchestrator"
    return got, basis


def _as_utc(value: Any) -> dt.datetime | None:
    if value is None:
        return None
    if hasattr(value, "to_pydatetime"):
        value = value.to_pydatetime()
    if isinstance(value, dt.datetime):
        return value.replace(tzinfo=dt.UTC) if value.tzinfo is None else value.astimezone(dt.UTC)
    if isinstance(value, dt.date):
        return dt.datetime(value.year, value.month, value.day, tzinfo=dt.UTC)
    return None


def column_max(path: Path, column: str) -> tuple[bool, dt.datetime | None]:
    """(column present, max as aware UTC datetime) from Parquet row-group statistics, else a DuckDB scan."""
    import pyarrow.parquet as pq

    pf = pq.ParquetFile(path)
    names = pf.schema_arrow.names
    if column not in names:
        return False, None
    j = names.index(column)
    md = pf.metadata
    best: dt.datetime | None = None
    exact = True
    for i in range(md.num_row_groups):
        col = md.row_group(i).column(j)
        st = col.statistics
        if md.row_group(i).num_rows == 0:
            continue
        if st is None or not st.has_min_max:
            if st is not None and st.null_count == md.row_group(i).num_rows:
                continue
            exact = False
            break
        v = _as_utc(st.max)
        if v is None:
            exact = False
            break
        best = v if best is None or v > best else best
    if exact:
        return True, best
    import pyarrow.compute as pc

    best = None
    for batch in pf.iter_batches(batch_size=65536, columns=[column]):  # streamed: no DuckDB, bounded memory
        v = _as_utc(pc.max(batch.column(0)).as_py())
        best = v if best is None or (v is not None and v > best) else best
    return True, best


def _stage_dirs(root: Path, stage: Stage) -> set[Path]:
    dirs = {root / stage.manifest_dir} if stage.manifest_dir else set()
    for o in stage.outputs:
        static = []
        for part in o.glob.split("/")[:-1]:
            if any(ch in part for ch in "*?[") or "=" in part:
                break
            static.append(part)
        if static:
            dirs.add(root.joinpath(*static))
    return dirs


def partial_files(root: Path, stage: Stage) -> list[Path]:
    """``.partial`` files under the stage's directories (scratch dirs starting with ``_`` skipped) or next to a
    root-level output."""
    found: set[Path] = set()
    for d in _stage_dirs(root, stage):
        if d.is_dir():
            for p in d.rglob("*.partial"):
                if not any(part.startswith("_") for part in p.relative_to(root).parts[:-1]):
                    found.add(p)
    for o in stage.outputs:
        found.update(p for p in root.glob(o.glob + ".partial") if p.is_file())
    return sorted(found)


def verify_stage(root: Path, stage: Stage, by_name: Mapping[str, Stage], now: dt.datetime,
                 hash_files: bool = True) -> StageResult:
    res = StageResult(stage.name, manifest=stage.manifest)
    mpath = root / stage.manifest
    for p in partial_files(root, stage):
        res.add("FAIL", "partial_file", p.relative_to(root).as_posix())
    if not mpath.is_file():
        if stage.planned:
            if res.status == "OK":
                res.status = "PLANNED"
            return res
        res.add("FAIL", "missing_manifest", stage.manifest)
        return res
    manifest = read_manifest(mpath)
    res.manifest_sha256 = sha256_file(mpath)
    if manifest is None:
        res.add("FAIL", "unreadable_manifest", stage.manifest)
        return res
    missing = [k for k in REQUIRED_MANIFEST_KEYS if not manifest.get(k)]
    if missing:
        res.add("FAIL", "incomplete_manifest", "no " + ", ".join(missing))
    if manifest.get("status") and manifest.get("status") != "complete":
        res.add("FAIL", "incomplete_manifest", f"status {manifest.get('status')!r}")
    if stage.schema and manifest.get("schema") and manifest["schema"] != stage.schema:
        res.add("FAIL", "schema_mismatch", f"manifest {manifest['schema']!r} != registry {stage.schema!r}")
    # files listed by the manifest
    base = root / stage.manifest_dir
    files = recorded_files(manifest, base)
    if files is not None:
        for rel, rec in sorted(files.items()):
            p = base / rel
            if not p.is_file():
                res.add("FAIL", "file_missing", rel)
                continue
            res.files_checked += 1
            size = p.stat().st_size
            if rec.get("bytes") is not None and int(rec["bytes"]) != size:
                res.add("FAIL", "size_mismatch", f"{rel}: {size} bytes, manifest {rec['bytes']}")
                continue
            if not rec.get("sha256"):
                res.add("FAIL", "incomplete_manifest", f"{rel}: no sha256")
            elif hash_files:
                res.bytes_hashed += size
                if sha256_file(p) != rec["sha256"]:
                    res.add("FAIL", "sha_mismatch", rel)
    # registered outputs: present, bound, clocked
    matched = output_files(root, stage)
    for o in stage.outputs:
        paths = matched[o.view]
        if not paths:
            res.add("FAIL", "output_missing", o.glob)
            continue
        if files is not None:
            for p in paths:
                rel = Path(os.path.relpath(p, base)).as_posix()
                if rel not in files:
                    res.add("FAIL", "unbound_file", p.relative_to(root).as_posix())
        if o.clock is None or not o.glob.endswith(".parquet"):
            continue
        if o.clock != "available_at":
            res.add("WARN", "legacy_clock", f"{o.view}: clock column {o.clock!r}, not available_at")
        latest: dt.datetime | None = None
        for p in paths:
            try:
                present, mx = column_max(p, o.clock)
            except Exception as exc:
                res.add("FAIL", "unreadable_parquet", f"{p.relative_to(root).as_posix()}: {type(exc).__name__}")
                continue
            if not present:
                res.add("FAIL", "clock_missing", f"{p.relative_to(root).as_posix()}: no column {o.clock!r}")
            elif mx is not None and (latest is None or mx > latest):
                latest = mx
        if latest is not None and latest > now:
            res.add("FAIL", "clock_future", f"{o.view}: max({o.clock}) = {latest.isoformat()} > now {now.isoformat()}")
    # input binding
    got, res.binding_basis = input_binding(stage, manifest, root, res.manifest_sha256)
    for name in stage.inputs:
        inp = by_name[name]
        cur = manifest_sha(root, inp.manifest)
        if cur is None:
            res.add("STALE", "input_missing", f"{name}: {inp.manifest} absent")
        elif inp.manifest not in got:
            res.add("WARN", "input_unbound", f"{name}: no binding recorded for {inp.manifest}")
        elif got[inp.manifest] != cur:
            res.add("STALE", "input_changed", f"{name}: {inp.manifest} changed since the build "
                                              f"(recorded {got[inp.manifest][:12]}, now {cur[:12]})")
    return res


# ---------------------------------------------------------------- run
def verify(root: Path, stages: list[Stage], names: Iterable[str] | None = None, now: dt.datetime | None = None,
           hash_files: bool = True) -> list[StageResult]:
    now = now or dt.datetime.now(dt.UTC)
    by_name = {s.name: s for s in stages}
    want = list(names) if names else [s.name for s in stages]
    unknown = [n for n in want if n not in by_name]
    if unknown:
        raise KeyError(f"not registered: {unknown}")
    return [verify_stage(root, by_name[n], by_name, now, hash_files) for n in want]


def render(results: list[StageResult]) -> str:
    out = []
    for r in results:
        head = f"{r.status:<7} {r.stage:<32} files={r.files_checked:<4} binding={r.binding_basis}"
        out.append(head)
        seen: set[tuple[str, str]] = set()
        for f in r.findings:
            key = (f.code, f.detail)
            if key not in seen:
                seen.add(key)
                out.append(f"        {f.kind:<5} {f.code}: {f.detail}")
    counts: dict[str, int] = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1
    out.append("summary: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    import argparse

    from . import registry
    from .contract import default_root
    from .coverage import coverage_report

    ap = argparse.ArgumentParser(prog="python -m atx_db.lake verify", description=__doc__.splitlines()[0])
    ap.add_argument("--stage", action="append", default=None, help="stage name (repeatable); default: all")
    ap.add_argument("--root", type=Path, default=None)
    ap.add_argument("--no-hash", action="store_true", help="check sizes only, skip SHA-256")
    ap.add_argument("--json", type=Path, default=None, help="write the full report here")
    ap.add_argument("--now", default=None, help="ISO timestamp for the future-clock check (default: now)")
    args = ap.parse_args(argv)
    root = args.root or default_root()
    stages = registry.load()
    now = dt.datetime.fromisoformat(args.now).replace(tzinfo=dt.UTC) if args.now else None
    results = verify(root, stages, args.stage, now, not args.no_hash)
    print(render(results))
    gaps = "" if args.stage else coverage_report(root, stages)
    if gaps:
        print("\n" + gaps)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"root": root.as_posix(), "results": [asdict(r) for r in results],
                                         "coverage_gaps": gaps}, indent=2), encoding="utf-8")
    if gaps or any(r.status == "FAIL" for r in results):
        return 1
    return 2 if any(r.status == "STALE" for r in results) else 0
