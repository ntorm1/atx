"""The research field builder with the engine path for its ported fields (platform core migration slice 3, PM8-12).

  python atx-engine/tools/prepare_research_fields_engine.py <the builder's argv>
      [--engine-fields si_shares,si_dtc,vol_126 --engine-exe <build>/bin/atx-research-fields.exe]

Without ``--engine-fields`` this is exactly ``prepare_research_fields.py`` (same argv, same code, every byte of every
output identical: the plain builder runs). With it, each named field is computed by the C++ executable
``atx-research-fields`` (``atx-engine/include/atx/engine/research/fields/research_fields_cli.hpp``) and every other field
by the Python builder, in one run and one manifest. The engine is wired in at the builder's own producer calls, the way
its registry hooks extend it (``research_fields_holdings.register``): ``finra_field`` and ``research_fields_price.
volume_mean_rows`` are routed to the executable for the named fields only, and the builder's own run() then writes the
manifest entries, digests, quantiles and reuse block unchanged. So the manifest is byte-identical to the Python path's
when the engine reproduces the Python payload and coverage, which ``test_prepare_research_fields_engine.py`` checks.

The plain builder is not edited: its code identity (``code_sha256*``, recorded in every manifest and used by
``--reuse``) does not move, as with the draft modules' wrappers (``prepare_research_fields_ohlc.py``).

Per engine field, before the builder sees it: the executable's receipt (``atx.research-fields-receipt/v1``) must be
complete and bound to this role; the payload must hash to the receipt's SHA-256; numpy's member quantiles of the payload
(the builder's ``digest_and_quantiles``) must equal the engine's bit for bit; the engine's formula fingerprint must equal
this builder's ``formula_id`` of the field (spec text drift refuses); its sources must equal the inputs this builder
reads. The spec and receipt of each call stay in ``<output>/engine_fields/`` (the manifest lists only payloads).
"""
from __future__ import annotations

import argparse
import contextlib
import copy
import json
from pathlib import Path
import subprocess
import sys
import time

import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_price as price      # same directory: vol_126's module (bound into the builder)

ENGINE_FIELDS = ("si_shares", "si_dtc", "vol_126")   # ported and identity-tested (gtest ResearchFieldsFixture.*)
SPEC_SCHEMA = "atx.research-fields-spec/v1"
RECEIPT_SCHEMA = "atx.research-fields-receipt/v1"
RECEIPT_DIR = "engine_fields"
COVERAGE_KEYS = ("member_cells", "finite_member_cells", "finite_member_frac", "score_window", "per_year",
                 "finite_cells_all", "member_finite_min", "member_finite_max", "member_finite_mean")


class EngineError(ValueError):
    """The engine path refused: the executable failed or its output disagrees with this builder."""


class EngineWriter:
    """The surface of a builder FieldWriter that run() and the field modules read, for a payload the engine wrote."""

    def __init__(self, path: Path, coverage: dict):
        self.path = path
        self.vcount = int(coverage["finite_member_cells"])
        self._coverage = {k: coverage[k] for k in COVERAGE_KEYS}

    def coverage(self) -> dict:
        return copy.deepcopy(self._coverage)


def parse_names(text: str) -> list:
    names = [x.strip() for x in text.split(",") if x.strip()]
    if not names or len(set(names)) != len(names) or any(x not in ENGINE_FIELDS for x in names):
        raise EngineError(f"--engine-fields must be distinct names from {', '.join(ENGINE_FIELDS)}")
    return names


def build(exe: Path, name: str, role, output: Path, budget, finra: Path | None = None) -> dict:
    """Run ``exe build`` for one field and return its checked receipt entry (``<output>/<name>.f64`` written)."""
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
        done = subprocess.run([str(exe), "build", "--spec", str(spec_path), "--receipt", str(receipt_path)],
                              capture_output=True, text=True, timeout=max(1.0, budget.deadline - time.monotonic()))
    except (OSError, subprocess.SubprocessError) as exc:
        raise EngineError(f"{name}: {exe} failed to run: {exc}") from exc
    if done.returncode != 0:
        raise EngineError(f"{name}: {exe} exit {done.returncode}: {(done.stderr or '')[-400:]}")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if receipt.get("schema") != RECEIPT_SCHEMA or receipt.get("status") != "complete" or \
            receipt.get("role", {}).get("manifest_sha256") != role.manifest_sha256 or \
            [e.get("name") for e in receipt.get("fields", [])] != [name]:
        raise EngineError(f"{name}: the receipt is not a complete {RECEIPT_SCHEMA} of this role and field")
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
    return entry


def same_sources(name: str, got: list, want: list) -> list:
    """``want`` (this builder's own source records) when the engine read exactly those bytes, else refuse."""
    if [(s.get("bytes"), s.get("sha256")) for s in got] != [(s["bytes"], s["sha256"]) for s in want]:
        raise EngineError(f"{name}: the engine read other source bytes than this builder's")
    return want


def finra_producer(base, exe: Path, names):
    """``finra_field`` with the named fields computed by the engine (the builder's return shape)."""
    def finra_field(name, finra, role, output, schedule, budget):
        if name not in names:
            return base(name, finra, role, output, schedule, budget)
        entry = build(exe, name, role, output, budget, finra=finra)
        asof = Path(finra) / "asof"
        csv, receipt = asof / builder.FIELDS[name]["source_file"], asof / "manifest.json"
        want = [{"path": str(p.resolve()), "bytes": len(blob), "sha256": builder.sha_bytes(blob)}
                for p, blob in ((p, p.read_bytes()) for p in (csv, receipt))] + [schedule[2]]
        sources = same_sources(name, entry["sources"], want)
        coverage = {k: entry["coverage"][k] for k in COVERAGE_KEYS}
        coverage["vintage_risk"] = entry["coverage"]["vintage_risk"]
        return EngineWriter(Path(output) / entry["file"], entry["coverage"]), sources, coverage, \
            entry["source_checks"], entry["extra"]
    return finra_field


def volume_producer(exe: Path):
    """``research_fields_price.volume_mean_rows`` computed by the engine (the module's return shape)."""
    def volume_mean_rows(h, role, output, budget):
        entry = build(exe, "vol_126", role, output, budget)
        files = role.manifest["files"]
        want = [{"path": str((Path(role.dir) / f).resolve()), "bytes": files[f]["bytes"], "sha256": files[f]["sha256"]}
                for f in ("volume.f64", "present.u8")]
        sources = same_sources("vol_126", entry["sources"], want)
        return {"vol_126": (EngineWriter(Path(output) / entry["file"], entry["coverage"]), sources, {})}
    return volume_mean_rows


@contextlib.contextmanager
def engine_path(exe: Path, names):
    """Route the named fields' producers to the engine for the duration of one build."""
    exe = Path(exe)
    if not exe.is_file():
        raise EngineError(f"--engine-exe {exe} is not a file")
    ns = vars(builder)
    base_finra, base_volume = ns["finra_field"], price.volume_mean_rows
    ns["finra_field"] = finra_producer(base_finra, exe, set(names))
    if "vol_126" in names:
        price.volume_mean_rows = volume_producer(exe)
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
