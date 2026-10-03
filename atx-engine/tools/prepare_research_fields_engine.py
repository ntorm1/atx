"""The research field builder with the engine path for its registry ``kind: engine`` fields (platform core migration
slice 3, PM8-12; P9 contracts K-P9-1 and K-P9-3).

  python atx-engine/tools/prepare_research_fields_engine.py <the builder's argv>
      [--engine-fields si_shares,si_dtc,vol_126 --engine-exe <build>/bin/atx-research-fields.exe]

Without ``--engine-fields`` this is exactly ``prepare_research_fields.py`` (same argv, same code, every byte of every
output identical: the plain builder runs, and neither the field registry nor the executable is read). With it, each
named field is computed by the C++ executable ``atx-research-fields``
(``atx-engine/include/atx/engine/research/fields/research_fields_cli.hpp``) and every other field by the Python
builder, in one run and one manifest. The named fields must be rows of kind ``engine`` of the field registry
(``field_registry.json``, K-P9-1, whose engine rows name the executable's builder kind) that this wrapper can route.
The engine is wired in at the builder's own producer calls, the way its registry hooks extend it
(``research_fields_holdings.register``): ``finra_field`` and ``research_fields_price.volume_mean_rows`` are routed to
the executable for the named fields only, and the builder's own run() then writes the manifest entries, digests,
quantiles and reuse block. The builder entry (``prepare_research_fields.py --registry``) calls ``engine_path`` the same
way for its engine rows when it is given an executable.

What the engine path claims (fields review FD-1): each engine field's payload bytes and coverage equal the Python
builder's, and its manifest entry says who produced them. Every engine entry carries the producer identity of K-P9-3,
``{"kind": "engine", "exe_sha256", "git_sha", "build_type", "receipt_sha256"}`` (``exe_sha256`` of the executable run,
which the receipt must report; ``git_sha`` and ``build_type`` from the receipt; ``receipt_sha256`` of the receipt
bytes), in place of the Python producer. So the manifest differs from the Python path's in the engine entries' producer
blocks only, which ``atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py`` checks. The
builder's ``--reuse`` never carries an engine entry through its source fingerprint (ruling P6); the executable's
registry build reuses engine entries keyed on that identity (``research/fields/reuse.hpp``).

The plain builder is not edited: its code identity (``code_sha256*``, recorded in every manifest and used by
``--reuse``) does not move, as with the draft modules' wrappers (``prepare_research_fields_ohlc.py``).

Per engine field, before the builder sees it: the executable's receipt (``atx.research-fields-receipt/v1``) must be
complete and bound to this role, and report this executable's SHA-256; the payload must hash to the receipt's SHA-256;
numpy's member quantiles of the payload (the builder's ``digest_and_quantiles``) must equal the engine's bit for bit;
the engine's formula fingerprint must equal this builder's ``formula_id`` of the field (spec text drift refuses); its
sources must equal the inputs this builder reads. The spec and receipt of each call stay in ``<output>/engine_fields/``
(the manifest lists only payloads).
"""
from __future__ import annotations

import argparse
import contextlib
import copy
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_price as price      # same directory: vol_126's module (bound into the builder)

REGISTRY = Path(__file__).resolve().parent / "field_registry.json"   # K-P9-1 (the engine rows: kind "engine")
REGISTRY_SCHEMA = "atx.field-registry/v1"
ROUTES = ("si_shares", "si_dtc", "vol_126")   # the producer calls routed here: finra_field, volume_mean_rows
SPEC_SCHEMA = "atx.research-fields-spec/v1"
RECEIPT_SCHEMA = "atx.research-fields-receipt/v1"
RECEIPT_DIR = "engine_fields"
PRODUCER_KIND = "engine"                      # K-P9-3 (a producer without "kind" is the Python shape)
COVERAGE_KEYS = ("member_cells", "finite_member_cells", "finite_member_frac", "score_window", "per_year",
                 "finite_cells_all", "member_finite_min", "member_finite_max", "member_finite_mean")


class EngineError(ValueError):
    """The engine path refused: the executable failed or its output disagrees with this builder."""


@dataclass(frozen=True)
class EngineExe:
    """The executable of one engine path and the SHA-256 of its bytes (the producer identity's ``exe_sha256``)."""
    path: Path
    sha256: str


class EngineWriter:
    """The surface of a builder FieldWriter that run() and the field modules read, for a payload the engine wrote."""

    def __init__(self, path: Path, coverage: dict):
        self.path = path
        self.vcount = int(coverage["finite_member_cells"])
        self._coverage = {k: coverage[k] for k in COVERAGE_KEYS}

    def coverage(self) -> dict:
        return copy.deepcopy(self._coverage)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def engine_rows(registry=None) -> list:
    """The names of the field registry's ``kind: engine`` rows, in registration order (K-P9-1)."""
    path = Path(REGISTRY if registry is None else registry)
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise EngineError(f"the field registry {path} cannot be read: {exc}") from exc
    rows = doc.get("fields") if isinstance(doc, dict) and doc.get("schema") == REGISTRY_SCHEMA else None
    if not isinstance(rows, list):
        raise EngineError(f"{path} is not an {REGISTRY_SCHEMA} field registry")
    return [r["name"] for r in rows if isinstance(r, dict) and r.get("kind") == PRODUCER_KIND
            and isinstance(r.get("name"), str)]


def routable(names) -> list:
    """``names`` when this wrapper routes every one of them, else refuse."""
    names = list(names)
    unrouted = [x for x in names if x not in ROUTES]
    if unrouted:
        raise EngineError(f"no engine route in this wrapper for {', '.join(unrouted)} (routes: {', '.join(ROUTES)})")
    return names


def parse_names(text: str, registry=None) -> list:
    """The ``--engine-fields`` names: distinct ``kind: engine`` rows of the field registry, each routable here."""
    names = [x.strip() for x in text.split(",") if x.strip()]
    rows = engine_rows(registry)
    if not names or len(set(names)) != len(names) or any(x not in rows for x in names):
        raise EngineError("--engine-fields must be distinct names of the field registry's kind engine rows "
                          f"({', '.join(rows) or 'none'})")
    return routable(names)


def producer_of(receipt: dict, exe: EngineExe, blob: bytes, name: str) -> dict:
    """The K-P9-3 producer block of the engine entries a receipt (``blob``, parsed ``receipt``) describes."""
    engine = receipt.get("engine") if isinstance(receipt.get("engine"), dict) else {}
    if engine.get("exe_sha256") != exe.sha256:
        raise EngineError(f"{name}: the receipt's exe_sha256 is not the SHA-256 of {exe.path}")
    if not all(isinstance(engine.get(k), str) and engine[k] for k in ("git_sha", "build_type")):
        raise EngineError(f"{name}: the receipt carries no engine git_sha / build_type")
    return {"kind": PRODUCER_KIND, "exe_sha256": exe.sha256, "git_sha": engine["git_sha"],
            "build_type": engine["build_type"], "receipt_sha256": builder.sha_bytes(blob)}


def build(exe: EngineExe, name: str, role, output: Path, budget, finra: Path | None = None) -> tuple:
    """Run ``exe build`` for one field: (its checked receipt entry, its producer block), <output>/<name>.f64 written."""
    work = Path(output) / RECEIPT_DIR
    work.mkdir(exist_ok=True)
    spec = {"schema": SPEC_SCHEMA, "role": {"dir": str(Path(role.dir).resolve()),
                                            "manifest_sha256": role.manifest_sha256},
            "output_dir": str(Path(output).resolve()), "fields": [name]}
    if finra is not None:
        spec["finra"] = str(Path(finra).resolve())
    spec_path, receipt_path = work / f"{name}.spec.json", work / f"{name}.receipt.json"
    with spec_path.open("xb") as f:
        f.write((json.dumps(spec, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    budget.check(f"{name}-engine")
    try:
        done = subprocess.run([str(exe.path), "build", "--spec", str(spec_path), "--receipt", str(receipt_path)],
                              capture_output=True, text=True, timeout=max(1.0, budget.deadline - time.monotonic()))
    except (OSError, subprocess.SubprocessError) as exc:
        raise EngineError(f"{name}: {exe.path} failed to run: {exc}") from exc
    if done.returncode != 0:
        raise EngineError(f"{name}: {exe.path} exit {done.returncode}: {(done.stderr or '')[-400:]}")
    blob = receipt_path.read_bytes()
    receipt = json.loads(blob)
    if receipt.get("schema") != RECEIPT_SCHEMA or receipt.get("status") != "complete" or \
            receipt.get("role", {}).get("manifest_sha256") != role.manifest_sha256 or \
            [e.get("name") for e in receipt.get("fields", [])] != [name]:
        raise EngineError(f"{name}: the receipt is not a complete {RECEIPT_SCHEMA} of this role and field")
    producer = producer_of(receipt, exe, blob, name)
    entry = receipt["fields"][0]
    path = Path(output) / f"{name}.f64"
    if entry.get("file") != path.name or not path.is_file():
        raise EngineError(f"{name}: the engine did not write {path.name}")
    digest, quantiles = builder.digest_and_quantiles(path, role, int(entry["coverage"]["finite_member_cells"]), budget)
    if digest != entry["sha256"]:
        raise EngineError(f"{name}: payload SHA-256 {digest} differs from the receipt's {entry['sha256']}")
    if builder.canonical(quantiles) != builder.canonical(entry["coverage"]["member_finite_quantiles"]):
        raise EngineError(f"{name}: the engine's member quantiles differ from numpy's (identity broken)")
    formula = builder.formula_id(name, builder.spec_definition(name, price.LAG_SESSIONS))
    if entry.get("formula_sha256") != formula:
        raise EngineError(f"{name}: the engine's formula fingerprint differs from this builder's (spec text drift)")
    return entry, producer


def same_sources(name: str, got: list, want: list) -> list:
    """``want`` (this builder's own source records) when the engine read exactly those bytes, else refuse."""
    if [(s.get("bytes"), s.get("sha256")) for s in got] != [(s["bytes"], s["sha256"]) for s in want]:
        raise EngineError(f"{name}: the engine read other source bytes than this builder's")
    return want


def finra_producer(base, exe: EngineExe, names):
    """``finra_field`` with the named fields computed by the engine (the builder's return shape)."""
    def finra_field(name, finra, role, output, schedule, budget):
        if name not in names:
            return base(name, finra, role, output, schedule, budget)
        entry, producer = build(exe, name, role, output, budget, finra=finra)
        asof = Path(finra) / "asof"
        csv, receipt = asof / builder.FIELDS[name]["source_file"], asof / "manifest.json"
        want = [{"path": str(p.resolve()), "bytes": len(blob), "sha256": builder.sha_bytes(blob)}
                for p, blob in ((p, p.read_bytes()) for p in (csv, receipt))] + [schedule[2]]
        sources = same_sources(name, entry["sources"], want)
        coverage = {k: entry["coverage"][k] for k in COVERAGE_KEYS}
        coverage["vintage_risk"] = entry["coverage"]["vintage_risk"]
        extra = dict(entry["extra"], producer=producer)   # run() spreads the extras into the entry (K-P9-3)
        return EngineWriter(Path(output) / entry["file"], entry["coverage"]), sources, coverage, \
            entry["source_checks"], extra
    return finra_field


def volume_producer(exe: EngineExe):
    """``research_fields_price.volume_mean_rows`` computed by the engine (the module's return shape)."""
    def volume_mean_rows(h, role, output, budget):
        entry, producer = build(exe, "vol_126", role, output, budget)
        files = role.manifest["files"]
        want = [{"path": str((Path(role.dir) / f).resolve()), "bytes": files[f]["bytes"], "sha256": files[f]["sha256"]}
                for f in ("volume.f64", "present.u8")]
        sources = same_sources("vol_126", entry["sources"], want)
        # The module spreads this extra after its own producer, so the entry names the engine (K-P9-3).
        return {"vol_126": (EngineWriter(Path(output) / entry["file"], entry["coverage"]), sources,
                            {"producer": producer})}
    return volume_mean_rows


@contextlib.contextmanager
def engine_path(exe: Path, names):
    """Route the named fields' producers to the engine for the duration of one build."""
    exe = Path(exe)
    if not exe.is_file():
        raise EngineError(f"--engine-exe {exe} is not a file")
    names = routable(names)
    engine = EngineExe(exe, file_sha256(exe))
    ns = vars(builder)
    base_finra, base_volume = ns["finra_field"], price.volume_mean_rows
    ns["finra_field"] = finra_producer(base_finra, engine, set(names))
    if "vol_126" in names:
        price.volume_mean_rows = volume_producer(engine)
    try:
        yield
    finally:
        ns["finra_field"], price.volume_mean_rows = base_finra, base_volume


def run(role_dir, role_sha256, output, fields, *, engine_fields=None, engine_exe=None, **kw):
    """``prepare_research_fields.run`` with ``engine_fields`` (a subset of ``fields``) computed by ``engine_exe``."""
    if not engine_fields:
        return builder.run(role_dir, role_sha256, output, fields, **kw)
    names = parse_names(",".join(engine_fields))
    if any(x not in fields for x in names):
        raise EngineError("--engine-fields must name fields of --fields")
    with engine_path(engine_exe, names):
        return builder.run(role_dir, role_sha256, output, fields, **kw)


def main(argv=None):
    p = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    p.add_argument("--engine-fields")
    p.add_argument("--engine-exe", type=Path)
    a, rest = p.parse_known_args(sys.argv[1:] if argv is None else argv)
    if a.engine_fields is None:
        if a.engine_exe is not None:
            raise SystemExit("--engine-exe needs --engine-fields")
        return builder.main(rest)
    if a.engine_exe is None:
        raise SystemExit("--engine-fields needs --engine-exe (the atx-research-fields executable)")
    names = parse_names(a.engine_fields)
    with engine_path(a.engine_exe, names):
        return builder.main(rest)


if __name__ == "__main__":
    main()
