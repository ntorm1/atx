"""P9 A0 check harness: the identity bridge rebuilt against itself (request A0.3), check only.

Arms: A as published; B1 = A's rehearsal with an empty backfill tier (S2); B2 = a rehearsal without the 2026 SEC
ticker list (S1); B12 = B2's rehearsal with S2. S1 (``ATX_A0_S1``, ``atx_db.identity_links prepare``) and S2
(``ATX_A0_S2``, ``alpha_panel.identity_backfill``) are default-off switches in those modules. S3 and S4 live here and
are on in every arm:

* S3: ``stage`` sets ``ATX_ALPHA_PANEL_ROOT`` to the arm root and ``common.IDENTITY_R4_DIR`` to the arm's rehearsal
  output in memory BEFORE the stage module is imported (Stage I computes ``R4_EXPORT`` at import); it refuses when
  a stage module is already imported.
* S4: Stage I's own ``build`` runs with ``_validation_result`` wrapped. The honest result is what Stage I writes as
  ``validation`` (``seal``, ``expected``, ``got``, ``pass``) in ``identity/manifest.json``; only the single read
  that gates its ``SystemExit`` sees a pass. S4 is the only check relaxed; no input pin or hash guard is.

Frozen code: ``run-arms --commit`` runs every step from a ``git archive`` export of that commit in ``code/<sha7>/``
(``PYTHONPATH`` = its ``atx-db/src``; no junction or symlink into the data lake). Every step first asserts that
``atx_db`` is imported from the export, then puts the ``PACKAGE_ROOT``-derived constants of ``alpha_panel.common``
(``DATA_CONSTANTS``) back on the real data root in memory before any stage module is imported.

Every step that opens lake data is its own process under the memory guard (``--heavy``). The ``run-arms``
controller runs outside any guard (a guard inside a guard is refused) and reads only this harness's own records.

Usage (from C:/atx/atx-db; ``<guard>`` = ``run_memory_guarded.py --heavy --job-gb <cap> --wait-minutes 90
--disk-path C:/ --min-free-disk-gb 25 --receipt <new path> --``)::

    <guard> python -m atx_db.alpha_panel.a0_check recover-inputs
    <guard> python -m atx_db.alpha_panel.a0_check run-arms --commit <sha> --dry-run
    python -m atx_db.alpha_panel.a0_check run-arms --commit <sha>        # resumable; each step guarded
    <guard> python -m atx_db.alpha_panel.a0_check report [--window original --window 2020-latest]

Report windows: ``original`` is the request's pre-2024 window (the regression, with the zero gate of request line
445); ``2020-latest`` runs from 2020-01-01 to the last session of the pinned calendar the arms were seeded with,
and splits differences at the observation clock of the source each comparison removes.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib
import importlib.util
import io
import json
import os
import runpy
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path
from typing import Any

from . import common as C

A0 = "atx_db.alpha_panel.a0_check"
BASE = C.PACKAGE_ROOT / "data" / "research" / "identity_rehearsal" / "a0-check-20261004"
GUARD = C.PACKAGE_ROOT.parent / ".superpowers" / "sdd" / "tier1-parity" / "run_memory_guarded.py"
GUARD_FLAGS = ("--heavy", "--wait-minutes", "90", "--disk-path", "C:/", "--min-free-disk-gb", "25", "--quiet")
EXIT_NOT_ADMITTED = 78  # the guard never started the command
TICKER_SOURCE = Path(r"C:\Users\natha\Downloads\TickerHistory3.parquet")
ORIGINAL_RECEIPT = r"C:\atx\atx-db\data\tmp\v2\0.6\ri2_a.json"
# below the data root
PUBLISHED_R4 = Path("research") / "identity_rehearsal" / "session8-phased-r4"
PUBLISHED_ROOT = Path("alpha_panel") / "v1"
OPS_MANIFEST = Path("research") / "identity_inputs" / "session5" / "manifest.json"
DOCUMENT_CACHE = Path("cache") / "identity-oracle-session8"
FETCH_RETRY_ALLOWANCE = 8
PROVENANCE_COMMIT = "dec03454"

DATA_CONSTANTS = ("DEFAULT_ROOT", "COMPANYFACTS_DIR", "FSDS_DIR", "IDENTITY_R4_DIR")
STAGE_MODULES = ("atx_db.alpha_panel.identity_links", "atx_db.alpha_panel.identity_backfill",
                 "atx_db.alpha_panel.identity_names", "atx_db.alpha_panel.identity_table")
EXEC_MODULES = ("atx_db.identity_links",)
ID_STAGES = ("identity", "backfill", "names", "combine", "identity_table")
PREPARE_STEPS = ("vendor", "facts", "reconstruct")
ARMS = {"A": ("A", False, False), "B1": ("A", False, True),  # arm: (rehearsal arm, S1, S2)
        "B2": ("B2", True, False), "B12": ("B2", True, True)}
PAIRS = (("B1", "A"), ("B2", "A"), ("B12", "A"), ("B12", "B1"))  # (variant, baseline), request column order
PAIR_SWITCHES = {("B1", "A"): ("s2",), ("B2", "A"): ("s1",), ("B12", "A"): ("s1", "s2"), ("B12", "B1"): ("s1",)}
SEED_PATTERNS = ("prices/*/*.parquet", "calendar.parquet", "short_interest/si.parquet", "_tmp/panel_core/*.parquet",
                 "_tmp/panel_member/*.parquet")
STAGE_SCRATCH = tuple(f"_tmp/{name}.duckdb{wal}" for name in ("identity_backfill", "identity_names", "identity_table")
                      for wal in ("", ".wal"))
PHASE_CAPS = {"candidates": 1.0}  # r4 peaks: candidates 0.58 GiB, every other phase and step <= 0.37 GiB
# the rehearsal's own code pin (identity_links.build_rehearsal code_files) plus the alpha-panel identity chain
IDENTITY_MODULES = tuple(f"atx_db/{name}.py" for name in (
    "identity_links", "identity_reconstruction", "market_owner_bridge", "calendar", "_submissions_archive",
    "universe_us_listed", "historical_identity", "historical_identity_sources", "_fundamental_clock", "ticker_history",
    "market_daily", "warehouse")) + tuple(f"atx_db/alpha_panel/{name}.py" for name in (
    "common", "build", "identity_links", "identity_backfill", "identity_names", "identity_table", "a0_check",
    "private_identity_differential", "private_identity_arm_comparison"))
# stage: (output glob, date column) whose maxima make the stage's latest output date explicit
COVERAGE = {
    "identity": (("identity/links.parquet", "end_incl"),),
    "backfill": (("identity/links_backfill.parquet", "end_incl"), ("identity/links_combined.parquet", "end_incl")),
    "names": (("identity/links_name_days.parquet", "session_date"),),
    "combine": (("identity/links_backfill.parquet", "end_incl"), ("identity/links_combined.parquet", "end_incl")),
    "identity_table": (("identity/link_table.parquet", "valid_to"), ("export/*/links.parquet", "end_incl")),
}
EXTENDED_FROM = dt.date(2020, 1, 1)
WINDOWS = ("original", "2020-latest")


def _utc() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def _hashes(blob: bytes) -> dict[str, str]:
    return {"sha256": hashlib.sha256(blob).hexdigest(),
            "sha256_lf": hashlib.sha256(blob.replace(b"\r\n", b"\n")).hexdigest()}


def _print(payload: Any) -> None:
    print(json.dumps(payload, default=str), flush=True)


# ---------------------------------------------------------------------------------------------
# frozen code and real data paths


def assert_frozen_code(code_root: Path) -> Path:
    """Refuse unless the imported ``atx_db`` lies inside ``<code_root>/atx-db/src`` (the frozen export)."""
    import atx_db

    here = Path(atx_db.__file__).resolve()
    src = (Path(code_root) / "atx-db" / "src").resolve()
    if not here.is_relative_to(src) or not Path(C.__file__).resolve().is_relative_to(src):
        raise RuntimeError(f"atx_db imported from {here}, outside the frozen export {src}")
    return here


def restore_data_paths(data_root: Path) -> dict[str, str]:
    """Rebase the ``PACKAGE_ROOT``-derived data constants of ``alpha_panel.common`` on ``data_root`` (in memory)."""
    package_data = C.PACKAGE_ROOT / "data"
    restored = {}
    for name in DATA_CONSTANTS:
        value = Path(getattr(C, name))
        if not value.is_relative_to(data_root):
            value = Path(data_root) / value.relative_to(package_data)
        setattr(C, name, value)
        restored[name] = str(value)
    return restored


def _prelude(code_root: Path | None, data_root: Path | None) -> dict[str, Any]:
    """Every step: frozen-code assertion first, then the real data paths."""
    here = assert_frozen_code(code_root) if code_root is not None else None
    return {"atx_db": str(here) if here else None, "code_root": str(code_root) if code_root else None,
            "data_paths": restore_data_paths(Path(data_root) if data_root else C.PACKAGE_ROOT / "data")}


def check_arm_root(root: Path) -> Path:
    """An arm root never overlaps the published build root (GC4)."""
    root = Path(root).resolve()
    published = Path(C.DEFAULT_ROOT).resolve()
    if root == published or published in root.parents or root in published.parents:
        raise ValueError(f"arm root {root} overlaps the published build root {published}")
    return root


def _inside(path: Path, root: Path) -> Path:
    """``path`` below ``root`` with no symlink or junction on the way, and outside the published root."""
    for item in (path, *path.parents):
        if item == root:
            break
        if item.is_symlink() or item.is_junction():
            raise ValueError(f"reparse point refused: {item}")
    resolved = path.resolve()
    if not resolved.is_relative_to(root) or resolved == root:
        raise ValueError(f"{resolved} is not below {root}")
    if resolved.is_relative_to(Path(C.DEFAULT_ROOT).resolve()):
        raise ValueError(f"{resolved} lies in the published build root")
    return resolved


# ---------------------------------------------------------------------------------------------
# S3 + S4: one alpha-panel identity stage per process


class _GateOnce(dict):
    """Stage I's validation result, kept honest (S4).

    ``build`` reads ``["pass"]`` once, right after ``_validation_result`` returns, to decide its ``SystemExit``; that
    one read sees True so the stage continues. Every later read gets the stored value, and the manifest does too:
    ``json`` serialises a dict subclass from its stored items.
    """

    def __init__(self, result: dict[str, Any]):
        super().__init__(result)
        self._gate = True

    def __getitem__(self, key: str) -> Any:
        if key == "pass" and self._gate:
            self._gate = False
            return True
        return super().__getitem__(key)


def _record_validation(module: Any) -> list[dict[str, Any]]:
    original = module._validation_result
    seen: list[dict[str, Any]] = []

    def recorded(stats: dict[str, Any]) -> dict[str, Any]:
        result = original(stats)
        seen.append(dict(result))
        return _GateOnce(result)

    module._validation_result = recorded
    return seen


def stage_coverage(stage: str, root: Path) -> dict[str, Any]:
    """Row counts and latest dates of the stage's outputs, and the arm calendar's last session."""
    con = C.connect(memory="256MB", threads=1)
    try:
        out: dict[str, Any] = {"calendar_last_session": str(con.execute(
            f"SELECT max(session_date) FROM read_parquet('{C.calendar_path().as_posix()}')").fetchone()[0])}
        for pattern, column in COVERAGE[stage]:
            for path in sorted(root.glob(pattern)):
                rows, latest, known = con.execute(f"""SELECT count(*), max("{column}"), max(available_at)
                    FROM read_parquet('{path.as_posix()}')""").fetchone()
                out[path.relative_to(root).as_posix()] = {"rows": int(rows), f"max_{column}": str(latest),
                                                          "max_available_at": str(known)}
        return out
    finally:
        con.close()


def run_stage(root: Path, r4: Path, stage: str, *, data_root: Path | None = None,
              code_root: Path | None = None) -> int:
    """Run one identity stage's own entry point in arm root ``root`` against rehearsal output ``r4``."""
    if stage not in ID_STAGES:
        raise ValueError(f"unknown stage {stage!r}; one of {ID_STAGES}")
    here = assert_frozen_code(code_root) if code_root is not None else None
    early = [name for name in STAGE_MODULES if name in sys.modules]
    if early:
        raise RuntimeError(f"stage modules imported before the S3 patch: {early}")
    restored = restore_data_paths(Path(data_root) if data_root else C.PACKAGE_ROOT / "data")
    root, r4 = check_arm_root(root), Path(r4).resolve()
    os.environ["ATX_ALPHA_PANEL_ROOT"] = str(root)
    C.IDENTITY_R4_DIR = r4  # S3: before Stage I computes R4_EXPORT at import
    from . import build

    entry = {name: args for name, _, args in build.STAGES}[stage]  # ["-m", module, *argv]
    module = importlib.import_module(entry[1])
    if code_root is not None and not Path(module.__file__).resolve().is_relative_to(here.parent.parent):
        raise RuntimeError(f"{module.__name__} imported from outside the frozen export")
    seen = None
    if stage == "identity":
        if r4 / "phases" / "export-001" != module.R4_EXPORT:
            raise RuntimeError(f"Stage I reads {module.R4_EXPORT}, not the arm's rehearsal output")
        seen = _record_validation(module)
    sys.argv = [entry[1], *entry[2:]]
    rc = int(module.main() or 0)
    receipt: dict[str, Any] = {"stage": stage, "root": str(root), "r4": str(r4), "exit_code": rc,
                               "switches": {k: os.environ.get(k) for k in ("ATX_A0_S1", "ATX_A0_S2")},
                               "atx_db": str(here) if here else None, "module": str(Path(module.__file__).resolve()),
                               "data_paths": restored, "created_utc": _utc()}
    if seen is not None:
        if len(seen) != 1:
            raise RuntimeError(f"Stage I computed {len(seen)} validations")
        written = C.read_json(root / "identity" / "manifest.json")["validation"]
        if written != json.loads(json.dumps(seen[0], default=str)):
            raise RuntimeError("identity/manifest.json validation differs from Stage I's own result")
        receipt["validation"] = written
        print(f"a0 S4: Stage I seal {written['seal']} validation pass={written['pass']}", flush=True)
    if rc == 0:
        receipt["coverage"] = stage_coverage(stage, root)
        C.write_json_atomic(root / "_a0" / f"stage-{stage}.json", receipt)
    _print(receipt)
    return rc


# ---------------------------------------------------------------------------------------------
# rehearsal steps (prepare / rehearsal) from the export


def _export_out(argv: list[str]) -> Path | None:
    if argv[:1] == ["rehearsal"] and "--phase" in argv and argv[argv.index("--phase") + 1] == "export":
        return Path(argv[argv.index("--out") + 1])
    return None


def _export_sealed(out: Path) -> bool:
    receipt = out / "phases" / "export.json"
    return receipt.exists() and Path(C.read_json(receipt)["launch"]).parent.name == "export-001"


def run_module(module: str, argv: list[str], *, code_root: Path | None = None,
               data_root: Path | None = None) -> int:
    """``python -m <module> <argv>`` after the frozen-code assertion; the export phase must be attempt 001."""
    if module not in EXEC_MODULES:
        raise ValueError(f"exec runs only {EXEC_MODULES}")
    _prelude(code_root, data_root)
    out = _export_out(argv)
    if out is not None and (out / "phases" / "export-001").exists() and not _export_sealed(out):
        raise RuntimeError(f"{out}: export-001 exists unsealed; the export must succeed on its first attempt, "
                           "so the arm's six phases re-run into a new rehearsal root")
    if module in sys.modules:
        raise RuntimeError(f"{module} imported before its run")
    spec = importlib.util.find_spec(module)
    if code_root is not None and not Path(spec.origin).resolve().is_relative_to(
            (Path(code_root) / "atx-db" / "src").resolve()):
        raise RuntimeError(f"{module} resolves outside the frozen export: {spec.origin}")
    sys.argv = [spec.origin, *argv]
    try:
        runpy.run_module(module, run_name="__main__", alter_sys=True)
        code = 0
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
    if code == 0 and out is not None and not _export_sealed(out):
        raise RuntimeError(f"{out}: the export phase did not seal as attempt 001")
    return code


# ---------------------------------------------------------------------------------------------
# arm seeds: plain copies of the identity-independent published inputs


def _copy_verified(source: Path, dest: Path) -> dict[str, Any]:
    before = source.stat()
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_name(dest.name + ".partial")
    h = hashlib.sha256()
    with source.open("rb") as src, partial.open("wb") as out:
        while block := src.read(1 << 22):
            h.update(block)
            out.write(block)
    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f"published seed changed while copying: {source}")
    os.replace(partial, dest)
    if C.sha256_file(dest) != h.hexdigest():
        raise ValueError(f"seed copy differs from its source: {dest}")
    return {"bytes": after.st_size, "sha256": h.hexdigest(), "source_mtime_ns": after.st_mtime_ns}


def seed_coverage(root: Path) -> dict[str, str]:
    con = C.connect(memory="256MB", threads=1, temp_dir=root / "_tmp")
    try:
        query = {"calendar_first_session": f"SELECT min(session_date) FROM read_parquet('{(root / 'calendar.parquet').as_posix()}')",
                 "calendar_last_session": f"SELECT max(session_date) FROM read_parquet('{(root / 'calendar.parquet').as_posix()}')",
                 "member_last_session": "SELECT max(session_date) FROM read_parquet('"
                                        f"{(root / '_tmp' / 'panel_member' / '*.parquet').as_posix()}') WHERE member",
                 "prices_last_session": "SELECT max(session_date) FROM read_parquet('"
                                        f"{(root / 'prices' / '*' / '*.parquet').as_posix()}', hive_partitioning = false)"}
        return {key: str(con.execute(sql).fetchone()[0]) for key, sql in query.items()}
    finally:
        con.close()


def seed_arm(root: Path, record: Path, *, code_root: Path | None = None, data_root: Path | None = None) -> dict:
    """Copy ``SEED_PATTERNS`` from the published root into the arm root, each copy hash-verified."""
    _prelude(code_root, data_root)
    root, published = check_arm_root(root), Path(C.DEFAULT_ROOT)
    os.environ["ATX_ALPHA_PANEL_ROOT"] = str(root)  # any scratch goes below the arm root
    if record.exists():
        done = C.read_json(record)
        for item in done["files"]:
            if C.sha256_file(root / item["path"]) != item["sha256"]:
                raise ValueError(f"seed copy changed: {item['path']}")
        return {**done, "reused": True}
    files = []
    for pattern in SEED_PATTERNS:
        sources = sorted(p for p in published.glob(pattern) if p.is_file())
        if not sources:
            raise FileNotFoundError(f"no published seed matches {published / pattern}")
        for source in sources:
            rel = source.relative_to(published).as_posix()
            files.append({"path": rel, "source": str(source), **_copy_verified(source, root / rel)})
    payload = {"root": str(root), "published_root": str(published), "files": files,
               "bytes": sum(f["bytes"] for f in files), "coverage": seed_coverage(root), "created_utc": _utc()}
    C.write_json_atomic(record, payload)
    return payload


def unseed_arm(root: Path, record: Path, *, code_root: Path | None = None, data_root: Path | None = None) -> dict:
    """Delete the arm's seed copies and the stages' scratch DuckDB / spill files (never anything else)."""
    _prelude(code_root, data_root)
    root = check_arm_root(root)
    done = C.read_json(record)
    if Path(done["root"]).resolve() != root:
        raise ValueError(f"seed record {record} belongs to {done['root']}")
    removed: list[dict[str, Any]] = []
    for rel in [item["path"] for item in done["files"]] + list(STAGE_SCRATCH):
        path = root / rel
        if path.is_file():
            path = _inside(path, root)
            removed.append({"path": rel, "bytes": path.stat().st_size})
            path.unlink()
    for spill in sorted((root / "_tmp").glob("spill-*")):
        if spill.is_dir():
            removed.append({"path": _inside(spill, root).relative_to(root).as_posix(), "bytes": None})
            shutil.rmtree(spill)
    parents = sorted({(root / item["path"]).parent for item in done["files"]}, key=lambda p: len(p.parts),
                     reverse=True)
    for parent in parents:
        while parent != root and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent
    payload = {"root": str(root), "removed": removed, "bytes": sum(r["bytes"] or 0 for r in removed),
               "created_utc": _utc()}
    C.write_json_atomic(record.with_name(record.stem + ".unseeded.json"), payload)
    return payload


# ---------------------------------------------------------------------------------------------
# recover-inputs (survey X2-X4)


def write_recovered_receipt(build_json: Path, dest: Path) -> dict[str, Any]:
    """The lost receipt ``ri2_a.json``: the published ``build.json`` ``pin.inputs``, verbatim."""
    inputs = C.read_json(build_json)["pin"]["inputs"]
    if dest.exists():
        current = C.read_json(dest)
        if current["inputs"] != inputs:
            raise ValueError(f"{dest} differs from the published pin.inputs of {build_json}")
        return current
    payload = {"inputs": inputs, "recovery": {
        "source": f"{build_json} pin.inputs (verbatim)", "original_path": ORIGINAL_RECEIPT,
        "note": "the original receipt is lost; prepare reads only inputs.<name>.{path,sha256,received_at,"
                "receipt_basis}, so the original clocks are kept exactly", "created_utc": _utc()}}
    C.write_json_atomic(dest, payload)
    return C.read_json(dest)


def link_ticker_history(pinned: Path, source: Path, sha256: str) -> dict[str, Any]:
    """An NTFS hard link at the pinned TickerHistory3 path; the pinned SHA-256 is verified before linking."""
    if pinned.exists():
        status = "existing_link" if os.path.samefile(pinned, source) else "existing_file"
        actual = C.sha256_file(pinned)
        if actual != sha256:
            raise ValueError(f"{pinned} holds sha256 {actual}, not the pinned {sha256}")
        return {"status": status, "path": str(pinned), "sha256": actual, "bytes": pinned.stat().st_size}
    actual = C.sha256_file(source)
    if actual != sha256:
        raise ValueError(f"{source} holds sha256 {actual}, not the pinned {sha256}: not linked")
    pinned.parent.mkdir(parents=True, exist_ok=True)
    os.link(source, pinned)
    if not os.path.samefile(pinned, source):
        raise RuntimeError(f"hard link {pinned} does not resolve to {source}")
    return {"status": "linked", "path": str(pinned), "target": str(source), "sha256": actual,
            "bytes": pinned.stat().st_size}


def recover_documents(review: Path, fetcher: Any, *, pinned: dict[str, str] | None = None) -> dict[str, Any]:
    """Re-fetch every reviewed document body into its pinned path; stop at the first hash mismatch.

    ``fetcher`` is the committed ``SecDocumentFetcher`` (cache layout ``cache_path(url)``, cached responses spend
    no request). ``pinned`` is the published build's ``document_review.documents`` (path -> sha256), which the
    review's own pins must equal. Nothing is substituted: a mismatching URL is reported and the recovery stops.
    """
    payload = json.loads(review.read_text(encoding="utf-8"))
    docs: dict[str, tuple[Path, str]] = {}
    for row in payload["assertions"]:
        url, item = row["document_url"], (Path(row["document_path"]), row["document_sha256"])
        if docs.setdefault(url, item) != item:
            raise ValueError(f"conflicting document pins for {url}")
    if pinned is not None and ({os.path.normcase(p.resolve()): s for p, s in docs.values()}
                               != {os.path.normcase(p): s for p, s in pinned.items()}):
        raise ValueError("review document pins differ from the published build's document_review.documents")
    outside = [url for url, (path, _) in docs.items()
               if os.path.normcase(fetcher.cache_path(url).resolve()) != os.path.normcase(path.resolve())]
    if outside:
        raise ValueError(f"{len(outside)} pinned document paths are outside the fetcher's cache layout: {outside[:5]}")
    result: dict[str, Any] = {"documents": len(docs), "matched": 0, "empty_bodies": 0, "mismatched_urls": [],
                              "failed_urls": [], "not_attempted": 0, "status": "complete"}
    ordered = sorted(docs)
    for n, url in enumerate(ordered):
        path, sha = docs[url]
        response = fetcher.fetch(url)
        if response.status != 200:
            result["failed_urls"].append({"url": url, "status": response.status})
            result["status"] = "stopped_fetch_failed"
        elif C.sha256_file(path) != sha:
            result["mismatched_urls"].append(url)
            result["status"] = "stopped_hash_mismatch"
        else:
            result["matched"] += 1
            result["empty_bodies"] += path.stat().st_size == 0
            continue
        result["not_attempted"] = len(ordered) - n - 1
        break
    if hasattr(fetcher, "requests_used"):
        result["fetch_ledger_requests"] = fetcher.requests_used
    return result


def recover_inputs(base: Path, data_root: Path, ticker_source: Path) -> dict[str, Any]:
    from ..historical_identity_sources import SecDocumentFetcher

    build_json = data_root / PUBLISHED_R4 / "build.json"
    pin = C.read_json(build_json)["pin"]
    base.mkdir(parents=True, exist_ok=True)
    receipt_path = base / "ri2_a.recovered.json"
    write_recovered_receipt(build_json, receipt_path)
    ticker = pin["inputs"]["ticker_history"]
    linked = link_ticker_history(Path(ticker["path"]), ticker_source, ticker["sha256"])
    verified = {name: linked["sha256"] == item["sha256"] if name == "ticker_history" else
                C.sha256_file(Path(item["path"])) == item["sha256"] for name, item in pin["inputs"].items()}
    ops = data_root / OPS_MANIFEST
    review = Path(pin["document_review"]["path"])
    pins_ok = {"ops_manifest": C.sha256_file(ops) == pin["ops_manifest_sha256"],
               "document_review": C.sha256_file(review) == pin["document_review"]["sha256"]}
    if not all(pins_ok.values()):
        raise ValueError(f"published build pins differ: {pins_ok}")
    documents = pin["document_review"]["documents"]
    fetcher = SecDocumentFetcher(data_root / DOCUMENT_CACHE, budget=len(documents) + FETCH_RETRY_ALLOWANCE)
    recovered = recover_documents(review, fetcher, pinned=documents)
    outcome = {"published_build": {"path": str(build_json), "sha256": C.sha256_file(build_json)},
               "receipt": {"path": str(receipt_path), "sha256": C.sha256_file(receipt_path)},
               "ticker_history": linked, "inputs_verified": verified,
               "ops_manifest": {"path": str(ops), "sha256": pin["ops_manifest_sha256"]},
               "document_review": {"path": str(review), "sha256": pin["document_review"]["sha256"]},
               "document_cache": str(data_root / DOCUMENT_CACHE), "documents": recovered, "created_utc": _utc()}
    history = base / "recover-inputs" / f"{dt.datetime.now(dt.UTC).strftime('%Y%m%dT%H%M%SZ')}.json"
    C.write_json_atomic(history, outcome)
    C.write_json_atomic(base / "a0-inputs.json", outcome)
    return outcome


def preflight(base: Path, data_root: Path) -> dict[str, bool]:
    """Before any arm step: the recovered inputs still equal the published rehearsal's pins."""
    inputs = C.read_json(base / "a0-inputs.json")
    pin = C.read_json(data_root / PUBLISHED_R4 / "build.json")["pin"]
    review = Path(inputs["document_review"]["path"])
    docs = inputs["documents"]
    return {"receipt_inputs_verbatim": C.read_json(Path(inputs["receipt"]["path"]))["inputs"] == pin["inputs"],
            "ops_manifest_sha256": C.sha256_file(Path(inputs["ops_manifest"]["path"])) == pin["ops_manifest_sha256"],
            "document_review_pinned": str(review) == pin["document_review"]["path"]
                                      and C.sha256_file(review) == pin["document_review"]["sha256"],
            "documents_recovered": docs["status"] == "complete" and docs["matched"] == docs["documents"]
                                   == len(pin["document_review"]["documents"]),
            "ticker_history_present": Path(pin["inputs"]["ticker_history"]["path"]).is_file()}


# ---------------------------------------------------------------------------------------------
# run-arms: the request's procedure steps 1-2, serial, guarded, resumable


def rehearsal_root(base: Path, arm: str) -> Path:
    """``R_<arm>``, or ``R_<arm>-<n>`` once an export attempt there ran and failed.

    Stage I and the backfill read ``phases/export-001`` by name, so a failed first export moves the arm's six phases
    to a new root. Decided from this harness's own step records (guard exit 78: never admitted, never ran).
    """
    n = 1
    while True:
        name = f"R_{arm}" if n == 1 else f"R_{arm}-{n}"
        record = base / "steps" / f"{name}.rehearsal.export.json"
        attempts = C.read_json(record)["attempts"] if record.exists() else []
        if all(a["exit_code"] in (0, EXIT_NOT_ADMITTED) for a in attempts):
            return base / name
        n += 1


def plan_steps(base: Path, commit: str, *, data_root: Path, inputs: dict[str, str]) -> list[dict[str, Any]]:
    """Every step of ``run-arms`` in order: id, guard cap, environment overrides and the python argv."""
    from ..identity_links import REHEARSAL_PHASES
    from . import build

    code = base / "code" / commit[:7]
    caps = {name: cap for name, cap, _ in build.STAGES}
    common = ["--code-root", str(code), "--data-root", str(data_root)]

    def step(step_id: str, cap: float, s1: bool, s2: bool, *argv: str) -> dict[str, Any]:
        env = {"ATX_A0_S1": "1" if s1 else "0", "ATX_A0_S2": "1" if s2 else "0",
               "PYTHONPATH": str(code / "atx-db" / "src"), "PYTHONDONTWRITEBYTECODE": "1", "OPENBLAS_NUM_THREADS": "1"}
        return {"id": step_id, "cap": cap, "env": env, "argv": ["-m", A0, *argv]}

    steps = [step("preflight", 0.6, False, False, "preflight", *common, "--base", str(base))]
    rehearsals = {}
    for arm in ("A", "B2"):
        _, s1, _ = ARMS[arm]
        work, out = base / f"W_{arm}", rehearsal_root(base, arm)
        rehearsals[arm] = out
        for name in PREPARE_STEPS:
            steps.append(step(f"W_{arm}.prepare.{name}", 0.8, s1, False, "exec", *common, "--", EXEC_MODULES[0],
                              "prepare", "--work", str(work), "--receipt", inputs["receipt"], "--step", name))
        for phase in REHEARSAL_PHASES:
            steps.append(step(f"{out.name}.rehearsal.{phase}", PHASE_CAPS.get(phase, 0.8), s1, False, "exec", *common,
                              "--", EXEC_MODULES[0], "rehearsal", "--work", str(work), "--ops-manifest",
                              inputs["ops_manifest"], "--out", str(out), "--phase", phase, "--document-review",
                              inputs["document_review"]))
    for arm, (rehearsal, s1, s2) in ARMS.items():
        root, record = base / f"P_{arm}", base / "seeds" / f"P_{arm}.json"
        steps.append(step(f"P_{arm}.seed", 0.6, s1, s2, "seed", *common, "--root", str(root), "--record", str(record)))
        for stage in ID_STAGES:
            steps.append(step(f"P_{arm}.stage.{stage}", caps[stage], s1, s2, "stage", *common, "--root", str(root),
                              "--r4", str(rehearsals[rehearsal]), "--stage", stage))
        steps.append(step(f"P_{arm}.unseed", 0.6, s1, s2, "unseed", *common, "--root", str(root), "--record",
                          str(record)))
    return steps


def guard_command(step: dict[str, Any], *, receipt: Path, stdout: Path, stderr: Path, python: Path,
                  guard: Path) -> list[str]:
    return [str(python), str(guard), "--job-gb", str(step["cap"]), *GUARD_FLAGS, "--receipt", str(receipt),
            "--stdout", str(stdout), "--stderr", str(stderr), "--", str(python), *step["argv"]]


def _git(repo: Path, *args: str) -> bytes:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True).stdout


def ensure_export(repo: Path, sha: str, base: Path, record: dict[str, Any]) -> Path:
    """``git archive`` of ``atx-db/src`` at ``sha`` into ``code/<sha7>``; ``arms.json`` pins its module hashes."""
    code = base / "code" / sha[:7]
    arms_path = base / "arms.json"
    if arms_path.exists():
        arms = C.read_json(arms_path)
        if arms["commit"] != sha:
            raise SystemExit(f"{base} belongs to commit {arms['commit']}; use a new base directory")
        for rel, expected in arms["modules"].items():
            if _hashes((code / "atx-db" / "src" / rel).read_bytes()) != expected:
                raise SystemExit(f"frozen export changed: {rel}")
        return code
    if not code.exists():
        partial = code.with_name(code.name + ".partial")
        if partial.exists():
            for item in (partial, *partial.parents):
                if item == base:
                    break
                if item.is_symlink() or item.is_junction():
                    raise SystemExit(f"reparse point refused: {item}")
            shutil.rmtree(partial)
        with tarfile.open(fileobj=io.BytesIO(_git(repo, "archive", "--format=tar", sha, "--", "atx-db/src"))) as tar:
            tar.extractall(partial, filter="data")
        os.replace(partial, code)
    src = code / "atx-db" / "src"
    if not (src / "atx_db" / "alpha_panel" / "a0_check.py").is_file():
        raise SystemExit(f"commit {sha} has no a0_check harness")
    files = sorted(p for p in src.rglob("*") if p.is_file())
    tree = hashlib.sha256("".join(f"{p.relative_to(src).as_posix()}:{hashlib.sha256(p.read_bytes()).hexdigest()}\n"
                                  for p in files).encode()).hexdigest()
    arms = {"commit": sha, "code": str(code), "archive": f"git archive --format=tar {sha} -- atx-db/src",
            "core_autocrlf": _git(repo, "config", "--get", "core.autocrlf").decode().strip() or None,
            "modules": {rel: _hashes((src / rel).read_bytes()) for rel in IDENTITY_MODULES},
            "tree_files": len(files), "tree_sha256": tree, **record, "created_utc": _utc()}
    C.write_json_atomic(arms_path, arms)
    return code


def _step_env(step: dict[str, Any]) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("ATX_A0_") and k != "ATX_ALPHA_PANEL_ROOT"}
    return {**env, **step["env"]}


def run_arms(base: Path, commit: str, *, data_root: Path, python: Path, guard: Path, dry_run: bool) -> int:
    repo = C.PACKAGE_ROOT.parent
    sha = _git(repo, "rev-parse", "--verify", f"{commit}^{{commit}}").decode().strip()
    inputs_path = base / "a0-inputs.json"
    if not inputs_path.exists():
        raise SystemExit(f"{inputs_path} missing: run recover-inputs first")
    recovered = C.read_json(inputs_path)
    if recovered["documents"]["status"] != "complete":
        raise SystemExit(f"recover-inputs did not complete: {recovered['documents']['status']}")
    inputs = {"receipt": recovered["receipt"]["path"], "ops_manifest": recovered["ops_manifest"]["path"],
              "document_review": recovered["document_review"]["path"]}
    steps = plan_steps(base, sha, data_root=data_root, inputs=inputs)
    if dry_run:
        print(f"a0 run-arms plan: commit {sha}, export {base / 'code' / sha[:7]}, {len(steps)} steps, serial", flush=True)
        for n, s in enumerate(steps, 1):
            record = base / "steps" / f"{s['id']}.json"
            done = record.exists() and C.read_json(record).get("complete")
            command = guard_command(s, receipt=base / "receipts" / f"{s['id']}.01.json",
                                    stdout=base / "logs" / f"{s['id']}.01.out", stderr=base / "logs" / f"{s['id']}.01.err",
                                    python=python, guard=guard)
            print(f"{n:02d} {s['id']} cap={s['cap']} S1={s['env']['ATX_A0_S1']} S2={s['env']['ATX_A0_S2']}"
                  f"{' [complete]' if done else ''}\n   {subprocess.list2cmdline(command)}", flush=True)
        return 0
    if os.environ.get("ATX_GUARD_JOB"):
        raise SystemExit("run-arms is the controller: start it outside any guard (each step takes its own guard)")
    arms = {name: {"work": f"W_{r}", "rehearsal": rehearsal_root(base, r).name, "root": f"P_{name}",
                   "s1": s1, "s2": s2} for name, (r, s1, s2) in ARMS.items()}
    code = ensure_export(repo, sha, base, {"arms": arms, "inputs": inputs, "python": str(python), "guard": str(guard)})
    for s in steps:
        path = base / "steps" / f"{s['id']}.json"
        record = C.read_json(path) if path.exists() else {"id": s["id"], "commit": sha, "cap": s["cap"],
                                                          "env": s["env"], "argv": s["argv"], "attempts": []}
        if record["commit"] != sha:
            raise SystemExit(f"step record {path} belongs to commit {record['commit']}")
        if record.get("complete"):
            continue
        n = len(record["attempts"]) + 1
        receipt = base / "receipts" / f"{s['id']}.{n:02d}.json"
        stdout, stderr = base / "logs" / f"{s['id']}.{n:02d}.out", base / "logs" / f"{s['id']}.{n:02d}.err"
        stdout.parent.mkdir(parents=True, exist_ok=True)
        command = guard_command(s, receipt=receipt, stdout=stdout, stderr=stderr, python=python, guard=guard)
        started, t0 = _utc(), time.monotonic()
        rc = subprocess.call(command, env=_step_env(s), cwd=str(code / "atx-db"), stdin=subprocess.DEVNULL)
        wall = round(time.monotonic() - t0, 1)
        g = C.read_json(receipt) if receipt.exists() else {}
        record["attempts"].append({"attempt": n, "command": command, "exit_code": rc, "started_utc": started,
                                   "wall_seconds": wall, "run_seconds": g.get("run_seconds"),
                                   "peak_bytes": g.get("native_peak_job_memory_bytes"),
                                   "guard_status": g.get("status"), "receipt": str(receipt),
                                   "stdout": str(stdout), "stderr": str(stderr)})
        record["complete"] = rc == 0
        C.write_json_atomic(path, record)
        _print({"step": s["id"], "attempt": n, "exit_code": rc, "wall_seconds": wall,
                "peak_bytes": g.get("native_peak_job_memory_bytes"), "guard_status": g.get("status")})
        if rc != 0:
            print(f"a0 run-arms stopped at {s['id']} (exit {rc}); re-run to resume", flush=True)
            return rc
    return 0


# ---------------------------------------------------------------------------------------------
# report pieces: row 11 and the procedure step-3 sanity checks


def symbol_snapshot_items(work: Path) -> int:
    """``symbol_snapshot_match`` items (``EV_SYMBOL``) over every evidence row of a preparation work dir."""
    import pyarrow.parquet as pq

    from ..identity_reconstruction import EV_SYMBOL

    files = sorted((work / "evidence").glob("*.parquet"))
    if not files:
        raise FileNotFoundError(f"no evidence parts below {work / 'evidence'}")
    total = 0
    for path in files:
        for value in pq.read_table(path, columns=["value_json"]).column("value_json").to_pylist():
            items = json.loads(value).get("items") if value else None
            total += sum(1 for item in items or () if item.get("kind") == EV_SYMBOL)
    return total


def _read(path: Path) -> Any:
    return C.read_json(path) if path.exists() else None


def collect_arm_facts(base: Path) -> dict[str, dict[str, Any]]:
    facts = {}
    for arm, (rehearsal, s1, s2) in ARMS.items():
        root, work = base / f"P_{arm}", base / f"W_{rehearsal}"
        identity = _read(root / "identity" / "manifest.json") or {}
        backfill = _read(root / "identity" / "backfill_manifest.json") or {}
        preparation = _read(work / "preparation.json") or {}
        try:
            items = symbol_snapshot_items(work)
        except FileNotFoundError:
            items = None
        facts[arm] = {"work": work.name, "s1": s1, "s2": s2, "backfill_rows": backfill.get("backfill_rows"),
                      "name_tier": backfill.get("name_tier"), "a0_s2_recorded": backfill.get("a0_s2") is True,
                      "a0_s1_pinned": preparation.get("pin", {}).get("a0_s1") is True,
                      "preparation_found": bool(preparation), "validation": identity.get("validation"),
                      "symbol_snapshot_match": items}
    return facts


def row_11(facts: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {arm: {"backfill_rows": facts[arm]["backfill_rows"],
                  "symbol_snapshot_match": facts[arm]["symbol_snapshot_match"],
                  "name_tier": facts[arm]["name_tier"],
                  "validation_got": (facts[arm]["validation"] or {}).get("got")} for arm in ("B1", "B2", "B12")}


def sanity_checks(base: Path, facts: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    """Procedure step 3 (a zero must not be vacuous), plus the switch bindings and the export-001 rule."""
    checks = []

    def check(name: str, ok: bool, detail: Any) -> None:
        checks.append({"name": name, "ok": bool(ok), "detail": detail})

    for arm, f in facts.items():
        rows = f["backfill_rows"]
        if f["s2"]:
            check(f"{arm}: backfill_rows = 0", rows == 0, rows)
        else:
            check(f"{arm}: backfill_rows > 0", isinstance(rows, int) and rows > 0, rows)
    for arm, want_zero in (("A", False), ("B2", True)):
        items = facts[arm]["symbol_snapshot_match"]
        check(f"W_{arm}: symbol_snapshot_match items {'= 0' if want_zero else '> 0'}",
              items == 0 if want_zero else isinstance(items, int) and items > 0, items)
    for arm, f in facts.items():
        check(f"{arm}: backfill_manifest name_tier true", f["name_tier"] is True, f["name_tier"])
    for arm, f in facts.items():
        v = f["validation"]
        check(f"{arm}: Stage I validation entry (S4) present", isinstance(v, dict) and
              set(v) >= {"seal", "expected", "got", "pass"}, None if v is None else {"pass": v.get("pass")})
    for arm in ("A", "B2"):
        f = facts[arm]
        check(f"W_{arm}: preparation pin a0_s1 {'on' if f['s1'] else 'absent'}",
              f["preparation_found"] and f["a0_s1_pinned"] == f["s1"], f["a0_s1_pinned"])
    for arm, f in facts.items():
        check(f"{arm}: backfill_manifest a0_s2 {'recorded' if f['s2'] else 'absent'}", f["a0_s2_recorded"] == f["s2"],
              f["a0_s2_recorded"])
    for arm in ("A", "B2"):
        out = rehearsal_root(base, arm)
        check(f"{out.name}: export sealed at attempt 001", _export_sealed(out), str(out / "phases" / "export.json"))
    return checks


def sanity_lines(checks: list[dict[str, Any]]) -> list[str]:
    return [f"{'PASS' if c['ok'] else 'FAIL'} {c['name']} (got {c['detail']})" for c in checks]


# ---------------------------------------------------------------------------------------------
# report: windows, D0, provenance, rows 1-10, B2 attribution aid


def pinned_latest_session(calendar: Path, sha256: str, con: Any) -> dt.date:
    """The last session of the calendar the arms were seeded with, re-verified against its seed-time hash."""
    if C.sha256_file(calendar) != sha256:
        raise ValueError(f"{calendar} no longer matches its seed-time sha256 {sha256}")
    return con.execute(f"SELECT max(session_date) FROM read_parquet('{calendar.as_posix()}')").fetchone()[0]


def partial_year(latest: dt.date) -> bool:
    return (latest.month, latest.day) < (12, 28)


def window_for(name: str, latest: dt.date, observed_at: dt.datetime | None = None):
    from .private_identity_arm_comparison import Window

    if name == "original":
        return Window()
    if name != "2020-latest":
        raise ValueError(f"unknown window {name!r}; one of {WINDOWS}")
    years = range(EXTENDED_FROM.year, latest.year + 1)
    return Window(first_session=EXTENDED_FROM, last_session=latest, cutoff=latest + dt.timedelta(days=1),
                  row_7_buckets=tuple((str(y), y, y) for y in years), panel_from_year=EXTENDED_FROM.year,
                  observed_at=observed_at)


def _files_sha(entries: Any, *, table: str | None = None, name: str | None = None) -> str:
    if table is not None:
        (sha,) = [e["sha256"] for e in entries if e["table"] == table]
        return sha
    return entries[name]["sha256"]


def discover_pit(roots: list[Path]) -> str:
    """The single ``export/identity-bridge-*-pit`` directory every arm root wrote (no hard-coded version)."""
    names = {tuple(sorted(p.name for p in (root / "export").glob("identity-bridge-*-pit") if p.is_dir()))
             for root in roots}
    if len(names) != 1 or len(next(iter(names))) != 1:
        raise ValueError(f"arm roots do not share exactly one pit export directory: {sorted(names)}; pass --pit-export")
    return next(iter(names))[0]


def d0_section(con: Any, *, rehearsal_a: Path, arm_a: Path, published: Path, published_r4: Path,
               pit: str) -> dict[str, Any]:
    from .private_identity_differential import compare_d0

    links = Path("phases") / "export-001" / "security_company_links.part-0000.parquet"
    out = {"r4": compare_d0(
        con, rebuilt=rehearsal_a / links, published=published_r4 / links, kind="r4",
        rebuilt_sha256=_files_sha(C.read_json(rehearsal_a / "phases" / "export.json")["files"],
                                  table="security_company_links"),
        published_sha256=_files_sha(C.read_json(published_r4 / "manifest.json")["files"],
                                    table="security_company_links"))}
    for kind, rel, manifest, name in (
            ("pit", Path("export") / pit / "links.parquet", Path("export") / pit / "manifest.json", "links.parquet"),
            ("link_table", Path("identity") / "link_table.parquet", Path("identity") / "link_table_manifest.json",
             "link_table.parquet")):
        out[kind] = compare_d0(con, rebuilt=arm_a / rel, published=published / rel, kind=kind,
                               rebuilt_sha256=_files_sha(C.read_json(arm_a / manifest)["files"], name=name),
                               published_sha256=_files_sha(C.read_json(published / manifest)["files"], name=name))
    validation = C.read_json(arm_a / "identity" / "manifest.json")["validation"]
    out["stage_i_validation"] = ({"pass": True} if validation["pass"] else
                                 {"pass": False, "got": validation["got"], "expected": validation["expected"]})
    out["published_pit_manifest_sha256"] = C.sha256_file(published / "export" / pit / "manifest.json")
    return out


def provenance_section(*, published: Path, published_r4: Path, pit: str, repo: Path) -> dict[str, Any]:
    """Recorded code identities of the published manifests beside the same files at ``PROVENANCE_COMMIT``."""
    def at_commit(rel: str) -> dict[str, str]:
        blob = _git(repo, "show", f"{PROVENANCE_COMMIT}:{rel}")
        return {**_hashes(blob), "sha256_crlf": hashlib.sha256(
            blob.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")).hexdigest()}

    def compare(recorded: dict[str, str], rel: str) -> dict[str, Any]:
        mine = at_commit(rel)
        lf = recorded.get("sha256_lf")
        match = (recorded["sha256"] in mine.values()) or (lf is not None and lf == mine["sha256_lf"])
        return {"path": rel, "recorded": recorded, PROVENANCE_COMMIT: mine, "match": match}

    def head(sha: str | None) -> dict[str, Any]:
        if not sha:
            return {"git_head": sha}
        has = subprocess.run(["git", "-C", str(repo), "cat-file", "-e", f"{sha}:atx-db/src/atx_db/alpha_panel"],
                             capture_output=True).returncode == 0
        return {"git_head": sha, "contains_alpha_panel": has}

    alpha = "atx-db/src/atx_db/alpha_panel/"
    out: dict[str, Any] = {}
    for label, manifest in (("pit_export", published / "export" / pit / "manifest.json"),
                            ("link_table_manifest", published / "identity" / "link_table_manifest.json")):
        code = C.read_json(manifest)["code"]
        out[label] = {"manifest": str(manifest), **head(code.get("git_head")),
                      "files": {n: compare(r, alpha + n) for n, r in code.items() if n != "git_head"}}
    stage_i = C.read_json(published / "identity" / "manifest.json")
    out["stage_i_manifest"] = {"code_version": stage_i["code_version"], "sources": stage_i["sources"],
                               "no_code_hash_recorded": True,
                               "identity_links.py": {"path": alpha + "identity_links.py",
                                                     PROVENANCE_COMMIT: at_commit(alpha + "identity_links.py")}}
    out["backfill_and_name_manifests"] = {"no_code_identity_recorded": True, **{
        name: {"path": alpha + name, PROVENANCE_COMMIT: at_commit(alpha + name)}
        for name in ("identity_backfill.py", "identity_names.py")}}
    r4 = C.read_json(published_r4 / "manifest.json")
    out["r4_manifest"] = {"code_sha256": compare({"sha256": r4["code_sha256"]}, "atx-db/src/atx_db/identity_links.py"),
                          "code_files": {n: compare({"sha256": s}, f"atx-db/src/atx_db/{n}")
                                         for n, s in r4["code_files"].items()}}
    unmatched = [f"{label}:{n}" for label in ("pit_export", "link_table_manifest")
                 for n, c in out[label]["files"].items() if not c["match"]]
    unmatched += [f"r4:{n}" for n, c in out["r4_manifest"]["code_files"].items() if not c["match"]]
    unmatched += [] if out["r4_manifest"]["code_sha256"]["match"] else ["r4:code_sha256"]
    heads = {out[label]["git_head"] for label in ("pit_export", "link_table_manifest")}
    if unmatched:
        contains = out["pit_export"].get("contains_alpha_panel")
        statement = (f"No producing commit can be named: {len(unmatched)} recorded code hashes ({', '.join(unmatched)}) "
                     f"match no line-ending variant of the file at {PROVENANCE_COMMIT}, and the recorded git_head "
                     f"{sorted(map(str, heads))} {'contains' if contains else 'has no'} alpha_panel/ directory: "
                     "the published bridge was built from code no commit holds.")
    else:
        statement = (f"Every recorded code hash matches the file at {PROVENANCE_COMMIT} (raw or line-ending variant), "
                     f"but the recorded git_head is {sorted(map(str, heads))}: {PROVENANCE_COMMIT} is consistent with "
                     "these manifests, not proven to have built them.")
    out["unmatched"], out["producing_commit_statement"] = unmatched, statement
    return out


def observation_clocks(con: Any, *, receipt: Path, rehearsal_a: Path) -> dict[str, dt.datetime]:
    """Observation clocks of the two removed sources, from the pinned arm inputs.

    s1: the SEC ticker snapshot's receipt clock (``company_tickers.received_at``, the ``symbol_snapshot_match`` clock);
    s2: the earliest knowledge clock of the backfill candidates (r4 ``current_ticker_verified`` P/J links open at the
    snapshot) in arm A's rehearsal output.
    """
    s1 = dt.datetime.fromisoformat(C.read_json(receipt)["inputs"]["company_tickers"]["received_at"])
    links = rehearsal_a / "phases" / "export-001" / "security_company_links.part-0000.parquet"
    s2 = con.execute(f"""SELECT min(available_at) FROM read_parquet('{links.as_posix()}')
        WHERE link_basis = 'current_ticker_verified' AND link_primary IN ('P', 'J') AND valid_until IS NULL""").fetchone()[0]
    if s2 is None:
        raise ValueError(f"{links} holds no backfill candidates: no S2 observation clock")
    return {"s1": s1.replace(tzinfo=None), "s2": s2}


def attribution_aid(con: Any, *, r4_a: Path, r4_b2: Path, links_a: Path, links_b2: Path, window: Any) -> dict:
    """B2 only: r4 links keyed on ``TABLE_KEYS`` and Stage I links, in A only / in B2 only."""
    from ..identity_links import TABLE_KEYS

    keys = ", ".join(TABLE_KEYS["security_company_links"])
    first = window.first_session
    r4_cond = f"link_start < DATE '{window.cutoff}'" + (
        f" AND coalesce(link_end, DATE '9999-12-31') >= DATE '{first}'" if first else "")
    i_cond = (f"\"start\" <= DATE '{window.last_session}' AND end_incl >= DATE '{first}'" if first else "TRUE")

    def diff(path_a: Path, path_b: Path, cond: str, exclude: str) -> tuple[int, int]:
        a = f"SELECT * {exclude} FROM read_parquet('{path_a.as_posix()}') WHERE {cond}"
        b = f"SELECT * {exclude} FROM read_parquet('{path_b.as_posix()}') WHERE {cond}"
        return tuple(con.execute(f"SELECT count(*) FROM ({x} EXCEPT ALL {y})").fetchone()[0]
                     for x, y in ((a, b), (b, a)))

    key_a, key_b = (f"SELECT DISTINCT {keys} FROM read_parquet('{p.as_posix()}') WHERE {r4_cond}" for p in (r4_a, r4_b2))
    keyed = [con.execute(f"SELECT count(*) FROM ({x} EXCEPT {y})").fetchone()[0] for x, y in ((key_a, key_b), (key_b, key_a))]
    rows = diff(r4_a, r4_b2, r4_cond, "EXCLUDE (created_at)")
    stage_i = diff(links_a, links_b2, i_cond, "")
    return {"r4_security_company_links": {"filter": r4_cond, "keys": TABLE_KEYS["security_company_links"],
                                          "keys_in_a_only": keyed[0], "keys_in_b2_only": keyed[1],
                                          "rows_in_a_only": rows[0], "rows_in_b2_only": rows[1],
                                          "sha256": {"A": C.sha256_file(r4_a), "B2": C.sha256_file(r4_b2)}},
            "stage_i_links": {"filter": i_cond, "rows_in_a_only": stage_i[0], "rows_in_b2_only": stage_i[1],
                              "sha256": {"A": C.sha256_file(links_a), "B2": C.sha256_file(links_b2)}}}


def zero_gate(rows: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """Request line 445 on the original window: B12 vs A is zero in rows 1-4, 7 and 8 (Task 5 acceptance)."""
    r = rows["B12 vs A"]
    parts = {"rows_1_4": sum(r["rows_1_4"].values()),
             "row_7": sum(v["a_only"] + v["b_only"] for v in r["row_7"].values()),
             "row_8": r["row_8"]["a_only"] + r["row_8"]["b_only"]}
    return {"pass": not any(parts.values()), "counts": parts}


def _report_connection(out: Path) -> Any:
    import duckdb

    con = duckdb.connect(str(out / "a0-report.duckdb"), config={
        "memory_limit": "128MB", "threads": 1, "temp_directory": str(out / "a0-report.spill"),
        "preserve_insertion_order": False})
    con.execute("SET TimeZone='UTC'")
    return con


def report(base: Path, *, data_root: Path, out: Path | None = None, windows: tuple[str, ...] = WINDOWS,
           pit_export: str | None = None) -> dict[str, Any]:
    from .private_identity_arm_comparison import compare_arms

    out = out or base / "report"
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"{out} is not empty: pass --out <new directory>")
    arms_record = C.read_json(base / "arms.json")
    published, published_r4 = data_root / PUBLISHED_ROOT, data_root / PUBLISHED_R4
    roots = {arm: base / f"P_{arm}" for arm in ARMS}
    rehearsals = {arm: rehearsal_root(base, arm) for arm in ("A", "B2")}
    pit = pit_export or discover_pit(list(roots.values()))
    seeds = {arm: C.read_json(base / "seeds" / f"P_{arm}.json") for arm in ARMS}
    shared = {f["path"]: (Path(f["source"]), f["sha256"]) for f in seeds["A"]["files"]}
    for arm, seed in seeds.items():
        if {f["path"]: (Path(f["source"]), f["sha256"]) for f in seed["files"]} != shared:
            raise ValueError(f"P_{arm} was seeded from different published inputs than P_A")
    calendar, calendar_sha = shared["calendar.parquet"]
    members = sorted((src, sha) for rel, (src, sha) in shared.items() if rel.startswith("_tmp/panel_member/"))
    out.mkdir(parents=True, exist_ok=True)
    (out / "identifiers").mkdir()
    con = _report_connection(out)
    try:
        latest = pinned_latest_session(calendar, calendar_sha, con)
        clocks = observation_clocks(con, receipt=Path(arms_record["inputs"]["receipt"]), rehearsal_a=rehearsals["A"])
        results: dict[str, Any] = {
            "version": "p9-a0-report-v1", "commit": arms_record["commit"], "base": str(base), "pit_export": pit,
            "coverage": {"pinned_calendar": {"path": str(calendar), "sha256": calendar_sha,
                                             "last_session": latest.isoformat(),
                                             "latest_year_partial": partial_year(latest)},
                         "seed": seeds["A"]["coverage"],
                         "rehearsal_snapshot_date": {a: (_read(r / "manifest.json") or {}).get("snapshot_date")
                                                     for a, r in rehearsals.items()},
                         "observation_clocks": {k: v.isoformat(sep=" ") for k, v in clocks.items()},
                         "stages": {arm: {p.stem: C.read_json(p)["coverage"] for p in sorted((root / "_a0").glob("stage-*.json"))}
                                    for arm, root in roots.items()}},
            "d0": d0_section(con, rehearsal_a=rehearsals["A"], arm_a=roots["A"], published=published,
                             published_r4=published_r4, pit=pit),
            "provenance": provenance_section(published=published, published_r4=published_r4, pit=pit,
                                             repo=C.PACKAGE_ROOT.parent)}
        facts = collect_arm_facts(base)
        results["row_11"] = row_11(facts)
        results["sanity"] = sanity_lines(sanity_checks(base, facts))
        results["windows"] = {}
        for name in windows:
            (out / "identifiers" / name).mkdir()
            rows = {}
            for variant, baseline in PAIRS:
                clock = min(clocks[s] for s in PAIR_SWITCHES[(variant, baseline)])
                window = window_for(name, latest, clock)
                files = {arm: {"arm": arm, "pit": str((roots[arm] / "export" / pit / "links.parquet").resolve()),
                               "link_table": str((roots[arm] / "identity" / "link_table.parquet").resolve())}
                         for arm in (variant, baseline)}
                pins = {str(calendar.resolve()): calendar_sha, **{str(p.resolve()): s for p, s in members}}
                for arm in (variant, baseline):
                    root = roots[arm]
                    pins[files[arm]["pit"]] = _files_sha(C.read_json(root / "export" / pit / "manifest.json")["files"],
                                                         name="links.parquet")
                    pins[files[arm]["link_table"]] = _files_sha(C.read_json(
                        root / "identity" / "link_table_manifest.json")["files"], name="link_table.parquet")
                rows[f"{variant} vs {baseline}"] = compare_arms(
                    con, baseline=files[baseline], variant=files[variant], calendar=str(calendar.resolve()),
                    members=[str(p.resolve()) for p, _ in members], pins=pins,
                    output=out / "identifiers" / name / f"{variant}-vs-{baseline}", window=window)
            entry = {"window": rows["B12 vs A"]["window"], "rows": rows,
                     "b2_attribution_aid": attribution_aid(
                         con, r4_a=rehearsals["A"] / "phases" / "export-001" / "security_company_links.part-0000.parquet",
                         r4_b2=rehearsals["B2"] / "phases" / "export-001" / "security_company_links.part-0000.parquet",
                         links_a=roots["A"] / "identity" / "links.parquet",
                         links_b2=roots["B2"] / "identity" / "links.parquet", window=window_for(name, latest))}
            if name == "original":
                entry["zero_gate_b12_vs_a"] = zero_gate(rows)
            results["windows"][name] = entry
    finally:
        con.close()
        for path in (out / "a0-report.duckdb", out / "a0-report.duckdb.wal"):
            path.unlink(missing_ok=True)
        shutil.rmtree(out / "a0-report.spill", ignore_errors=True)
    results["created_utc"] = _utc()
    C.write_json_atomic(out / "a0-results.json", results)
    (out / "a0-results.md").write_text(markdown(results), encoding="utf-8")
    return results


def _pair(d: dict[str, int], a: str = "a_only", b: str = "b_only") -> str:
    return f"{d[a]} / {d[b]}"


def _split(r: dict[str, Any], key: str, keys: tuple[str, ...]) -> str:
    side = r.get(key)
    if not side:
        return ""
    return "<br>(before obs. clock " + " / ".join(str(sum(side["before"][k] for k in ks)) for ks in keys) + \
           "; at or after " + " / ".join(str(sum(side["at_or_after"][k] for k in ks)) for ks in keys) + ")"


def markdown(results: dict[str, Any]) -> str:
    """Request table (lines 351-364): its row numbering and column order, once per window."""
    d0 = results["d0"]
    cov = results["coverage"]
    lines = [f"# A0 results (commit {results['commit']})", "",
             f"Pinned calendar last session: {cov['pinned_calendar']['last_session']}"
             f"{' (latest year partial)' if cov['pinned_calendar']['latest_year_partial'] else ''}; "
             f"observation clocks: S1 {cov['observation_clocks']['s1']}, S2 {cov['observation_clocks']['s2']}.", "",
             "## D0 (arm A vs published)", ""]
    for kind in ("r4", "pit", "link_table"):
        r = d0[kind]
        lines.append(f"- {kind}: rebuilt only {r['rebuilt_only_rows']}, published only {r['published_only_rows']}, "
                     f"schema match {r['schema_matches']}, d0_pass {r['d0_pass']}")
    v = d0["stage_i_validation"]
    lines.append(f"- Stage I seal-2025 validation pass {v['pass']}"
                 + ("" if v["pass"] else f"; got {v['got']} vs expected {v['expected']}"))
    lines += ["", "## Provenance", "", results["provenance"]["producing_commit_statement"], ""]
    lines += ["## Sanity (procedure step 3)", "", *[f"- {line}" for line in results["sanity"]], ""]
    cols = [f"{v} vs {b}" for v, b in PAIRS]
    for name, entry in results["windows"].items():
        w = entry["window"]
        rows = entry["rows"]
        lines += [f"## Window {name}: {w['start_session']} .. {w['last_session']} (rows 8/9 cutoff {w['cutoff']})", "",
                  "| # | count | " + " | ".join(cols) + " |", "|---|---|" + "---|" * len(cols)]

        def row(number: str, label: str, cell, rows: dict[str, Any] = rows, lines: list[str] = lines) -> None:
            lines.append(f"| {number} | {label} | " + " | ".join(cell(rows[c]) for c in cols) + " |")

        lines.append(f"| D0 | A vs published: r4 {_pair(d0['r4'], 'rebuilt_only_rows', 'published_only_rows')}; "
                     f"pit {_pair(d0['pit'], 'rebuilt_only_rows', 'published_only_rows')}; link_table "
                     f"{_pair(d0['link_table'], 'rebuilt_only_rows', 'published_only_rows')}; Stage I pass {v['pass']} "
                     "| (once) | | | |")
        row("1", "in A only / in B only", lambda r: _pair(r["rows_1_4"], "1_a_only", "1_b_only")
            + _split(r, "rows_1_4_by_observation", (("1_a_only",), ("1_b_only",))))
        row("2", "different CIKs", lambda r: str(r["rows_1_4"]["2_cik"])
            + _split(r, "rows_1_4_by_observation", (("2_cik",),)))
        row("3", "P in A, J in B / J in A, P in B", lambda r: _pair(r["rows_1_4"], "3_p_to_j", "3_j_to_p")
            + _split(r, "rows_1_4_by_observation", (("3_p_to_j",), ("3_j_to_p",))))
        row("4", "different class_status", lambda r: str(r["rows_1_4"]["4_class"])
            + _split(r, "rows_1_4_by_observation", (("4_class",),)))
        row("5", "distinct sr_id", lambda r: str(r["row_5_distinct_sr_id"]))
        row("6", "distinct CIKs", lambda r: str(r["row_6_distinct_cik"]))
        row("7", "eligible member cells: A only / B only / total A", lambda r: "<br>".join(
            f"{k}: {x['a_only']} / {x['b_only']} / {x['a_total']}" for k, x in r["row_7"].items())
            + _split(r, "row_7_by_observation", (("a_only",), ("b_only",))))
        row("8", "rows start & available_at < cutoff: A only / B only", lambda r: _pair(r["row_8"]))
        row("9", "rows start < cutoff, available_at >= cutoff", lambda r: _pair(r["row_9"]))
        row("10", "panel CIK differs, per year", lambda r: "<br>".join(f"{y}: {n}" for y, n in r["row_10"].items()))
        r11 = results["row_11"]
        lines.append("| 11 | backfill_rows; symbol_snapshot_match; name_tier; Stage I got | " + " | ".join(
            f"{r11[a]['backfill_rows']}; {r11[a]['symbol_snapshot_match']}; {r11[a]['name_tier']}; {r11[a]['validation_got']}"
            for a in ("B1", "B2", "B12")) + " | n/a |")
        aid = entry["b2_attribution_aid"]
        lines += ["", f"B2 attribution aid: r4 keys A only / B2 only {aid['r4_security_company_links']['keys_in_a_only']} / "
                      f"{aid['r4_security_company_links']['keys_in_b2_only']} ({aid['r4_security_company_links']['filter']}); "
                      f"Stage I rows A only / B2 only {aid['stage_i_links']['rows_in_a_only']} / "
                      f"{aid['stage_i_links']['rows_in_b2_only']}."]
        if "zero_gate_b12_vs_a" in entry:
            g = entry["zero_gate_b12_vs_a"]
            lines.append(f"{'PASS' if g['pass'] else 'FAIL'} zero gate B12 vs A rows 1-4, 7, 8 (request line 445; "
                         f"Task 5 acceptance): {g['counts']}")
        lines.append("")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------
# CLI


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ["exec"]:  # exec [--code-root C] [--data-root D] -- MODULE ARGS...
        if "--" not in argv:
            raise SystemExit("exec needs -- MODULE ARGS")
        split = argv.index("--")
        ex = argparse.ArgumentParser(prog="a0_check exec")
        ex.add_argument("--code-root", type=Path)
        ex.add_argument("--data-root", type=Path)
        opts = ex.parse_args(argv[1:split])
        rest = argv[split + 1:]
        return run_module(rest[0], rest[1:], code_root=opts.code_root, data_root=opts.data_root)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    data = C.PACKAGE_ROOT / "data"
    r = sub.add_parser("recover-inputs")
    r.add_argument("--base", type=Path, default=BASE)
    r.add_argument("--data-root", type=Path, default=data)
    r.add_argument("--ticker-source", type=Path, default=TICKER_SOURCE)
    a = sub.add_parser("run-arms")
    a.add_argument("--commit", required=True)
    a.add_argument("--base", type=Path, default=BASE)
    a.add_argument("--data-root", type=Path, default=data)
    a.add_argument("--python", type=Path, default=Path(sys.executable))
    a.add_argument("--guard", type=Path, default=GUARD)
    a.add_argument("--dry-run", action="store_true")
    for name in ("preflight", "seed", "unseed", "stage"):
        p = sub.add_parser(name)
        p.add_argument("--code-root", type=Path)
        p.add_argument("--data-root", type=Path)
        if name == "preflight":
            p.add_argument("--base", type=Path, default=BASE)
        else:
            p.add_argument("--root", type=Path, required=True)
        if name in ("seed", "unseed"):
            p.add_argument("--record", type=Path, required=True)
        if name == "stage":
            p.add_argument("--r4", type=Path, required=True)
            p.add_argument("--stage", required=True)
    rp = sub.add_parser("report")
    rp.add_argument("--base", type=Path, default=BASE)
    rp.add_argument("--data-root", type=Path, default=data)
    rp.add_argument("--out", type=Path)
    rp.add_argument("--window", action="append", choices=WINDOWS)
    rp.add_argument("--pit-export")
    args = ap.parse_args(argv)
    if args.cmd == "recover-inputs":
        outcome = recover_inputs(args.base, args.data_root, args.ticker_source)
        _print(outcome)
        return 0 if outcome["documents"]["status"] == "complete" else 1
    if args.cmd == "run-arms":
        return run_arms(args.base, args.commit, data_root=args.data_root, python=args.python, guard=args.guard,
                        dry_run=args.dry_run)
    if args.cmd == "stage":
        return run_stage(args.root, args.r4, args.stage, data_root=args.data_root, code_root=args.code_root)
    if args.cmd == "preflight":
        _prelude(args.code_root, args.data_root)
        checks = preflight(args.base, Path(args.data_root) if args.data_root else data)
        _print(checks)
        return 0 if all(checks.values()) else 1
    if args.cmd in ("seed", "unseed"):
        fn = seed_arm if args.cmd == "seed" else unseed_arm
        payload = fn(args.root, args.record, code_root=args.code_root, data_root=args.data_root)
        _print({k: v for k, v in payload.items() if k not in ("files", "removed")} | {
            "files": len(payload.get("files") or payload.get("removed") or [])})
        return 0
    results = report(args.base, data_root=args.data_root, out=args.out, windows=tuple(args.window or WINDOWS),
                     pit_export=args.pit_export)
    for line in results["sanity"]:
        print(line, flush=True)
    print(f"a0 report: {(args.out or args.base / 'report') / 'a0-results.md'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
