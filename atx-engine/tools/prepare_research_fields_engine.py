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
quantiles and reuse block. The vendor-panel fields (P9 lane A3: ret_overnight, ret_intraday, ceq_iss_5y on the price
module's source panel; open_adj, high_adj, low_adj on the ohlc module's bar panel) are routed at
``research_fields_price.open_return_rows`` / ``ceq_iss_rows`` and ``research_fields_ohlc.bar_rows``: the module still
reads its own panel (so the manifest's source checks are unchanged), the engine builds the routed fields of one panel in
one call (``--price-source`` passed as the spec's ``price_source``), and its read statistics must equal the module's
panel's (``same_panel``), else the engine path refuses. The builder entry (``prepare_research_fields.py --registry``)
calls ``engine_path`` the same way for its engine rows when it is given an executable.

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
import research_fields_ohlc as ohlc        # same directory: the bar fields' draft module (bound by its shim)
import research_fields_price as price      # same directory: vol_126's module (bound into the builder)

REGISTRY = Path(__file__).resolve().parent / "field_registry.json"   # K-P9-1 (the engine rows: kind "engine")
REGISTRY_SCHEMA = "atx.field-registry/v1"
# The fields this wrapper can route (its producer hooks: finra_field, volume_mean_rows, and for the vendor-panel fields
# of P9 lane A3 open_return_rows / ceq_iss_rows and bar_rows); the registry decides which of them are engine rows.
# Public name kept: the builder entry (lane A1) validates engine rows against it.
PRICE_FIELDS = ("ret_overnight", "ret_intraday", "ceq_iss_5y")     # research_fields_price.py, on its source panel
BAR_FIELDS = ("open_adj", "high_adj", "low_adj")                   # research_fields_ohlc.py, on its bar panel
ENGINE_FIELDS = ("si_shares", "si_dtc", "vol_126") + PRICE_FIELDS + BAR_FIELDS
# The engine's name of a Python vendor-panel read statistic where the two differ (sources/vendor_panel.hpp).
SCAN_KEYS = {"rows_scanned": "rows_in_file", "rows_on_or_after_seal_skipped": "rows_sealed_dropped"}
# The read statistics both Python panels always record (research_fields_price.source_panel, research_fields_ohlc.
# bar_panel): a panel guard without them would compare nothing, so it refuses (fail closed).
PANEL_KEYS = ("rows_scanned", "rows_on_or_after_seal_skipped", "rows_selected", "rows_off_calendar",
              "duplicate_keys_quarantined")
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
    unrouted = [x for x in names if x not in ENGINE_FIELDS]
    if unrouted:
        raise EngineError(f"no engine route in this wrapper for {', '.join(unrouted)} "
                          f"(routes: {', '.join(ENGINE_FIELDS)})")
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


def lag_of(name: str) -> int:
    """The declared lag the field's own module fingerprints its spec at (its compute's LAG_SESSIONS)."""
    return ohlc.LAG_SESSIONS if name in BAR_FIELDS else price.LAG_SESSIONS


def build_fields(exe: EngineExe, names, role, output: Path, budget, finra: Path | None = None,
                 price_source: Path | None = None) -> dict:
    """Run ``exe build`` once for ``names`` (in that order; the vendor-panel fields of one call share one panel):
    {name: (its checked receipt entry, the call's producer block)}, each <output>/<name>.f64 written."""
    names = list(names)
    tag = "+".join(names)
    work = Path(output) / RECEIPT_DIR
    work.mkdir(exist_ok=True)
    spec = {"schema": SPEC_SCHEMA, "role": {"dir": str(Path(role.dir).resolve()),
                                            "manifest_sha256": role.manifest_sha256},
            "output_dir": str(Path(output).resolve()), "fields": names}
    if finra is not None:
        spec["finra"] = str(Path(finra).resolve())
    if price_source is not None:
        spec["price_source"] = str(Path(price_source).resolve())
    spec_path, receipt_path = work / f"{tag}.spec.json", work / f"{tag}.receipt.json"
    with spec_path.open("xb") as f:
        f.write((json.dumps(spec, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    budget.check(f"{tag}-engine")
    try:
        done = subprocess.run([str(exe.path), "build", "--spec", str(spec_path), "--receipt", str(receipt_path)],
                              capture_output=True, text=True, timeout=max(1.0, budget.deadline - time.monotonic()))
    except (OSError, subprocess.SubprocessError) as exc:
        raise EngineError(f"{tag}: {exe.path} failed to run: {exc}") from exc
    if done.returncode != 0:
        raise EngineError(f"{tag}: {exe.path} exit {done.returncode}: {(done.stderr or '')[-400:]}")
    blob = receipt_path.read_bytes()
    receipt = json.loads(blob)
    if receipt.get("schema") != RECEIPT_SCHEMA or receipt.get("status") != "complete" or \
            receipt.get("role", {}).get("manifest_sha256") != role.manifest_sha256 or \
            [e.get("name") for e in receipt.get("fields", [])] != names:
        raise EngineError(f"{tag}: the receipt is not a complete {RECEIPT_SCHEMA} of this role and these fields")
    producer = producer_of(receipt, exe, blob, tag)
    out = {}
    for name, entry in zip(names, receipt["fields"]):
        path = Path(output) / f"{name}.f64"
        if entry.get("file") != path.name or not path.is_file():
            raise EngineError(f"{name}: the engine did not write {path.name}")
        digest, quantiles = builder.digest_and_quantiles(path, role, int(entry["coverage"]["finite_member_cells"]),
                                                         budget)
        if digest != entry["sha256"]:
            raise EngineError(f"{name}: payload SHA-256 {digest} differs from the receipt's {entry['sha256']}")
        if builder.canonical(quantiles) != builder.canonical(entry["coverage"]["member_finite_quantiles"]):
            raise EngineError(f"{name}: the engine's member quantiles differ from numpy's (identity broken)")
        formula = builder.formula_id(name, builder.spec_definition(name, lag_of(name)))
        if entry.get("formula_sha256") != formula:
            raise EngineError(f"{name}: the engine's formula fingerprint differs from this builder's (spec text drift)")
        out[name] = (entry, producer)
    return out


def build(exe: EngineExe, name: str, role, output: Path, budget, finra: Path | None = None) -> tuple:
    """Run ``exe build`` for one field: (its checked receipt entry, its producer block), <output>/<name>.f64 written."""
    return build_fields(exe, [name], role, output, budget, finra=finra)[name]


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


def same_panel(name: str, entry: dict, stats: dict) -> None:
    """Refuse unless the engine read the vendor file as this builder's panel did: every read statistic the Python panel
    records (its rule text aside) equals the engine's (``SCAN_KEYS`` names the two that differ). Equal statistics mean
    the same axis (sessions before the role, first session), the same rows selected, quarantined and dropped, and the
    same factor-break-v1 outcome. The price fields' bits depend on the panel's history (a repaired step before the role
    divides both factors of a ratio), and the engine sets that history from the fields of its own call, so a run whose
    Python panel reaches further (coskew_60m without ceq_iss_5y, or ceq_iss_5y left to Python) is refused here. Fail
    closed: statistics that are not a dict, or lack any of ``PANEL_KEYS``, refuse (an empty comparison proves nothing),
    as does an engine entry without its panel's statistics."""
    if not isinstance(stats, dict) or any(k not in stats for k in PANEL_KEYS):
        raise EngineError(f"{name}: this builder's vendor panel recorded no read statistics "
                          f"({', '.join(PANEL_KEYS)} needed); refusing")
    checks = entry.get("source_checks")
    got = checks.get("source") if isinstance(checks, dict) and isinstance(checks.get("source"), dict) else {}
    if not got:
        raise EngineError(f"{name}: the engine's receipt carries no vendor panel statistics; refusing")
    for key, want in stats.items():
        other = SCAN_KEYS.get(key, key)
        if key == "rule" or (want is None and other not in got):
            continue
        if got.get(other) != want:
            raise EngineError(f"{name}: the engine's vendor panel differs from this builder's ({key}: "
                              f"{got.get(other)!r}, this builder {want!r})")


def price_producers(base_open, base_ceq, exe: EngineExe, names):
    """``research_fields_price.open_return_rows`` / ``ceq_iss_rows`` with the named fields computed by the engine (the
    module's return shapes). The module still loads its own source panel (its read statistics stay the manifest's
    source checks); the engine builds every routed price field the panel serves in ONE call, so its panel has the
    Python panel's history (ceq_iss_5y's 1,261 sessions and 490-day share lookback when it is requested; ``same_panel``
    refuses otherwise)."""
    routed = [x for x in PRICE_FIELDS if x in names]
    pending = {}   # the ceq_iss_5y entry the open-return call built, for ceq_iss_rows of the same panel

    def engine_call(fields, panel, role, output, budget) -> dict:
        built = build_fields(exe, fields, role, output, budget, price_source=Path(panel["source"]["path"]))
        for name, (entry, _) in built.items():
            same_panel(name, entry, panel["stats"])
        return built

    def result(name, built, panel, output, extra) -> tuple:
        entry, producer = built[name]
        sources = same_sources(name, entry["sources"], [panel["source"]])
        return EngineWriter(Path(output) / entry["file"], entry["coverage"]), sources, dict(extra, producer=producer)

    def open_return_rows(h, panel, role, output, budget, wanted):
        mine = [x for x in wanted if x in routed]
        out = base_open(h, panel, role, output, budget, [x for x in wanted if x not in mine]) \
            if len(mine) < len(wanted) else {}
        if not mine:
            return out
        # compute() asks for the open returns first; ceq_iss_5y is wanted exactly when the panel carries shares
        ceq = ["ceq_iss_5y"] if "ceq_iss_5y" in routed and panel["shares"] is not None else []
        built = engine_call(mine + ceq, panel, role, output, budget)
        pending.update(panel=id(panel), built=built)
        for x in mine:
            out[x] = result(x, built, panel, output,
                            {"guarded_member_cells": built[x][0]["extra"]["guarded_member_cells"]})
        return out

    def ceq_iss_rows(h, panel, role, output, budget):
        if "ceq_iss_5y" not in routed:
            return base_ceq(h, panel, role, output, budget)
        done = pending.get("built") if pending.get("panel") == id(panel) else None
        built = done if done and "ceq_iss_5y" in done else engine_call(["ceq_iss_5y"], panel, role, output, budget)
        pending.clear()
        entry = built["ceq_iss_5y"][0]
        if entry["extra"].get("domain") != list(price.CEQ_DOMAIN):
            raise EngineError("ceq_iss_5y: the engine's declared domain differs from this builder's")
        return {"ceq_iss_5y": result("ceq_iss_5y", built, panel, output,
                                     {"domain": list(price.CEQ_DOMAIN),
                                      "outside_domain_member_cells": entry["extra"]["outside_domain_member_cells"]})}

    return open_return_rows, ceq_iss_rows


def bar_producers(base_panel, base_rows, exe: EngineExe, names):
    """``research_fields_ohlc.bar_panel`` / ``bar_rows`` with the named bar fields computed by the engine in one call
    (the module's return shapes). The module still reads its own bar panel (its read statistics stay the manifest's
    source checks, and ``same_panel`` holds the engine's to them); ``bar_panel`` only records the vendor path."""
    routed = [x for x in BAR_FIELDS if x in names]

    def bar_panel(h, path, captured, role, budget):
        panel = base_panel(h, path, captured, role, budget)
        panel["engine_price_source"] = Path(path).resolve()   # read by bar_rows below only
        return panel

    def bar_rows(h, panel, role, output, budget, wanted):
        mine = [x for x in wanted if x in routed]
        out = base_rows(h, panel, role, output, budget, [x for x in wanted if x not in mine]) \
            if len(mine) < len(wanted) else {}
        if not mine:
            return out
        built = build_fields(exe, mine, role, output, budget, price_source=panel["engine_price_source"])
        files = role.manifest["files"]
        want = [{"path": str((Path(role.dir) / f).resolve()), "bytes": files[f]["bytes"], "sha256": files[f]["sha256"]}
                for f in ("close.f64", "raw_close.f64", "present.u8")]
        for x in mine:
            entry, producer = built[x]
            same_panel(x, entry, panel["stats"])
            vendor = entry["sources"][0] if entry.get("sources") else {}
            if vendor.get("sha256") != role.source_sha256:
                raise EngineError(f"{x}: the engine read another vendor file than the role's source")
            sources = same_sources(x, entry["sources"][1:], want)   # compute() puts the vendor pin first
            extra = {k: entry["extra"][k] for k in ("order_violation_member_cells", "missing_bar_member_cells")}
            out[x] = (EngineWriter(Path(output) / entry["file"], entry["coverage"]), sources,
                      dict(extra, producer=producer))
        return out

    return bar_panel, bar_rows


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
    base_price = price.open_return_rows, price.ceq_iss_rows
    base_bars = ohlc.bar_panel, ohlc.bar_rows
    ns["finra_field"] = finra_producer(base_finra, engine, set(names))
    if "vol_126" in names:
        price.volume_mean_rows = volume_producer(engine)
    if set(names) & set(PRICE_FIELDS):
        price.open_return_rows, price.ceq_iss_rows = price_producers(*base_price, engine, names)
    if set(names) & set(BAR_FIELDS):
        ohlc.bar_panel, ohlc.bar_rows = bar_producers(*base_bars, engine, names)
    try:
        yield
    finally:
        ns["finra_field"], price.volume_mean_rows = base_finra, base_volume
        price.open_return_rows, price.ceq_iss_rows = base_price
        ohlc.bar_panel, ohlc.bar_rows = base_bars


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
