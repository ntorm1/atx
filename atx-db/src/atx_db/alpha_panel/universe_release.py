"""Independent universe_release_v1 candidate admission, bounded resume and rollback.

Always NOT_PRODUCTION_READY. Byte/key admission contains Task2 R5 findings;
it neither repairs source classification nor certifies economic source quality.
Real commands require controller review and an exclusive guarded job slot.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
from pathlib import Path
import types
import uuid
from contextlib import contextmanager
from typing import Any

from . import common as C
from . import release_inputs as R
from . import universe as U

STAGE = "universe_release_v1"
SCHEMA = "atx.alpha-panel.universe-release/v1"
REQUIRED = tuple(range(2018, 2027))
OUTPUT_COLUMNS = ("session_date", "security_id", "cik", "shrcd_like", "exchcd_like", "exchcd_basis",
                  "is_adr", "is_reit", "is_spac", "is_cef_etf", "is_foreign_issuer", "filer_regime", "me",
                  "univ_all_common", "univ_r3000p", "univ_r1000p", "univ_r2000p", "univ_sp500p", "available_at")


def native_adapter(root: Path, generation: Path):
    """Clone native function globals, mapping only its output stage privately."""
    namespace = dict(vars(U))
    def path(*parts):
        return ((generation if parts[0] == U.STAGE else root / parts[0]).joinpath(*parts[1:])).as_posix()
    # calendar_sql lives in market_common and otherwise calls the shared C root.
    market = types.SimpleNamespace(**vars(U.M))
    market.calendar_sql = lambda: ("(SELECT session_date, CAST(row_number() OVER (ORDER BY session_date) AS INTEGER) AS sidx, "
                                    "lag(session_date) OVER (ORDER BY session_date) AS prev_session "
                                    f"FROM read_parquet('{(root / 'calendar.parquet').as_posix()}'))")
    namespace["M"] = market
    private_common = types.SimpleNamespace(**vars(C))
    private_common.build_root = lambda: root
    private_common.calendar_path = lambda: root / "calendar.parquet"
    namespace["C"] = private_common
    for name, value in vars(U).items():
        if isinstance(value, types.FunctionType) and value.__module__ == U.__name__:
            namespace[name] = types.FunctionType(value.__code__, namespace, name, value.__defaults__, value.__closure__)
            namespace[name].__kwdefaults__ = value.__kwdefaults__
    namespace["_p"] = path
    return types.SimpleNamespace(**namespace)


def parquet_sql(paths: list[Path]) -> str:
    if not paths:
        raise RuntimeError("empty parquet product")
    # SQL paths are internal resolved paths; quote even apostrophes correctly.
    names = ",".join("'" + p.as_posix().replace("'", "''") + "'" for p in paths)
    return f"read_parquet([{names}], union_by_name=true, hive_partitioning=false)"


def validate_keys(con, payload: Path, panel: list[Path], calendar: Path, year: int,
                  expected_schema: list[list[str]] | None = None) -> dict[str, Any]:
    """Typed unique exact panel keys, calendar membership and native close clocks."""
    for name, paths in (("release_u", [payload]), ("release_p", panel), ("release_cal", [calendar])):
        con.execute(f"CREATE OR REPLACE TEMP VIEW {name} AS SELECT * FROM {parquet_sql(paths)}")
    schema = [list(row[:2]) for row in con.execute("DESCRIBE release_u").fetchall()]
    panel_schema = dict(row[:2] for row in con.execute("DESCRIBE release_p").fetchall())
    types_u = dict(schema)
    if tuple(types_u) != OUTPUT_COLUMNS:
        raise RuntimeError("unsupported/keyless universe output schema")
    for name in ("shrcd_like", "exchcd_like"):
        if types_u[name] != "INTEGER":
            raise RuntimeError("unsupported universe code type")
    for name in OUTPUT_COLUMNS[6:11] + OUTPUT_COLUMNS[13:18]:
        if types_u[name] != "BOOLEAN":
            raise RuntimeError("unsupported universe flag type")
    if types_u["exchcd_basis"] != "VARCHAR" or types_u["filer_regime"] != "VARCHAR" or types_u["me"] != "DOUBLE":
        raise RuntimeError("unsupported universe value schema")
    if types_u["cik"] not in ("INTEGER", "BIGINT", "UINTEGER", "UBIGINT"):
        raise RuntimeError("unsupported issuer id schema")
    if types_u.get("session_date") != "DATE" or types_u.get("security_id") not in ("BIGINT", "INTEGER", "UBIGINT", "UINTEGER"):
        raise RuntimeError("unsupported candidate key types")
    if panel_schema.get("session_date") != "DATE" or panel_schema.get("security_id") != types_u["security_id"]:
        raise RuntimeError("panel key schema mismatch")
    if types_u.get("available_at") not in ("TIMESTAMP", "TIMESTAMP WITH TIME ZONE"):
        raise RuntimeError("missing/unsupported clock")
    if expected_schema is not None and schema != expected_schema:
        raise RuntimeError("candidate schema mismatch")
    for name in ("release_u", "release_p"):
        bad = con.execute(f"""SELECT count(*) FROM {name} WHERE session_date IS NULL OR security_id IS NULL
            OR year(session_date) <> {year} OR session_date NOT IN (SELECT session_date FROM release_cal)""").fetchone()[0]
        dup = con.execute(f"SELECT count(*) FROM (SELECT session_date,security_id FROM {name} GROUP BY 1,2 HAVING count(*)<>1)").fetchone()[0]
        if bad or dup:
            raise RuntimeError(f"invalid null/year/session/duplicate keys in {name}: {bad}/{dup}")
    cal_bad = con.execute("SELECT count(*) FROM (SELECT session_date FROM release_cal GROUP BY 1 HAVING session_date IS NULL OR count(*)<>1)").fetchone()[0]
    if cal_bad:
        raise RuntimeError("invalid calendar keys")
    differences = con.execute("""SELECT count(*) FROM (
        (SELECT session_date,security_id FROM release_p EXCEPT SELECT session_date,security_id FROM release_u)
        UNION ALL (SELECT session_date,security_id FROM release_u EXCEPT SELECT session_date,security_id FROM release_p))""").fetchone()[0]
    clocks = con.execute("""SELECT count(*) FROM release_u WHERE available_at IS NULL
        OR available_at <> CAST(session_date AS TIMESTAMP) + INTERVAL 22 HOUR""").fetchone()[0]
    rows = con.execute("SELECT count(*) FROM release_u").fetchone()[0]
    if not rows or differences or clocks:
        raise RuntimeError(f"empty/mismatched panel or invalid clocks: {rows}/{differences}/{clocks}")
    dates = [row[0].isoformat() for row in con.execute("SELECT DISTINCT session_date FROM release_p ORDER BY 1").fetchall()]
    # Ordered hash computed in batches, without collecting the key axis in Python.
    import hashlib
    checksum = hashlib.sha256()
    cursor = con.execute("SELECT session_date,security_id FROM release_p ORDER BY 1,2")
    while batch := cursor.fetchmany(8192):
        for date, security in batch:
            checksum.update(f"{date.isoformat()}|{security}\n".encode())
    return {"rows": rows, "schema": schema, "dates": dates, "panel_keys_sha256": checksum.hexdigest()}


class Candidate:
    def __init__(self, directory: Path, frozen: dict[str, Any], root: Path):
        self.directory, self.frozen, self.root = directory, frozen, root
        stamp = directory / "snapshot.json"
        if stamp.exists():
            if C.read_json(stamp) != frozen:
                raise RuntimeError("stale candidate source/code identity")
        elif directory.exists() and any(directory.iterdir()):
            raise RuntimeError("unbound candidate directory")
        else:
            C.write_json_atomic(stamp, frozen)

    def payload(self, year):
        return self.directory / f"year={year}" / "universe.parquet"

    def panel(self, year):
        members = self.frozen["products"][f"panel_{year}"]["files"]
        return [R.safe_path(self.root, member) for member in sorted(members)]

    def admit(self, con, year, schema=None):
        receipt = self.directory / f"year={year}" / "receipt.json"
        payload = self.payload(year)
        if not receipt.exists():
            if payload.parent.exists() and any(payload.parent.iterdir()):
                raise RuntimeError("interrupted unit retained; use a new candidate generation")
            return None
        row = C.read_json(receipt)
        if row.get("snapshot_sha256") != R.digest(self.frozen) or row.get("year") != year or row.get("payload") != R.identity(payload):
            raise RuntimeError("corrupt/stale unit receipt")
        keys = validate_keys(con, payload, self.panel(year), self.root / "calendar.parquet", year, schema)
        if row.get("keys") != keys:
            raise RuntimeError("unit key identity changed")
        self.check_daily(row["daily"], keys, con)
        if row.get("daily_sha256") != R.digest(row["daily"]):
            raise RuntimeError("unit daily receipt changed")
        native = native_adapter(self.root, self.directory)
        native.load_issuer_tables(con)
        native.build_year(con, year)
        replay = {r[0].isoformat(): dict(zip(U.DAILY_COLS, r[1:])) for r in native.year_receipt(con)}
        if replay != row["daily"]:
            raise RuntimeError("native diagnostic counters differ from authenticated replay")
        return row

    @staticmethod
    def check_daily(daily, keys, con):
        if set(daily) != set(keys["dates"]):
            raise RuntimeError("daily receipt date set mismatch")
        if any(set(row) != set(U.DAILY_COLS) or any(type(v) is not int or v < 0 or v > keys["rows"] for v in row.values()) for row in daily.values()):
            raise RuntimeError("daily receipt counters invalid")
        rows = con.execute("""SELECT session_date, count(*) FILTER(WHERE univ_all_common),
            count(*) FILTER(WHERE univ_all_common AND exchcd_like=1),
            count(*) FILTER(WHERE univ_all_common AND shrcd_like IS NOT NULL AND exchcd_like IS NOT NULL),
            count(*) FILTER(WHERE shrcd_like IS NULL) FROM release_u GROUP BY 1""").fetchall()
        columns = ("all_common", "nyse_all_common", "all_common_with_codes", "rows_without_shrcd")
        for date, *counts in rows:
            if any(daily[date.isoformat()][name] != count for name, count in zip(columns, counts)):
                raise RuntimeError("daily receipt differs from actual payload counters")

    def seal(self, con, year, daily, schema):
        R.recheck(self.root, self.frozen)
        keys = validate_keys(con, self.payload(year), self.panel(year), self.root / "calendar.parquet", year, schema)
        self.check_daily(daily, keys, con)
        row = {"snapshot_sha256": R.digest(self.frozen), "year": year, "keys": keys,
               "daily": daily, "daily_sha256": R.digest(daily), "payload": R.identity(self.payload(year))}
        C.write_json_atomic(self.directory / f"year={year}" / "receipt.json", row)
        return self.admit(con, year, schema)


def measurements(con, native, daily):
    codes = {}
    for year in U.GATE_YEARS:
        rows = [row for date, row in daily.items() if date.startswith(str(year))]
        denominator = sum(row["all_common"] for row in rows)
        codes[str(year)] = sum(row["all_common_with_codes"] for row in rows) / denominator if denominator else None
    sample = native.hand_check_sample(con)
    gates = {"daily_band": U.band_gate(daily),
             "daily_no_price_screen": U.band_gate(daily, col="all_common_no_price_screen"),
             "french_nyse": native.measure_french(con), "r2000": native.measure_r2000(con),
             "codes_on_all_common": {"pass": all(v is not None and v >= U.CODES_FLOOR for v in codes.values()), "per_year": codes,
                                     "criterion": "codes >=99% on all_common, each year 2020-2025"},
             "hand_check": U.hand_check_gate(sample=sample)}
    return {"schema": "atx.alpha-panel.universe-release.validation/v1", "measurement_contract": measurement_contract(), "gates": gates, "coverage": native.coverage(con), "counts": native.measure_counts(con),
            "sample": {"rows": sample, "sha256": U.sample_hash(sample), "seed": U.HAND_SEED},
            "daily_per_year": {col: U.band_gate(daily, years=U.YEARS, col=col)["per_year"] for col in U.DAILY_COLS}}


def measurement_contract():
    return {"required_years": list(REQUIRED), "gate_years": list(U.GATE_YEARS),
            "gate_definitions": U.GATES, "rule": U.RULE, "french_start": "2018-01", "french_end": str(U.FRENCH_END),
            "french_months": 60, "r2000_population": "every R2000P recon_summary effective_from",
            "coverage_fields": [list(row) for row in U.FIELDS], "coverage_universes": U.COVERAGE_UNIVERSES,
            "hand_seed": U.HAND_SEED, "hand_strata": {k: list(v) for k,v in U.HAND_STRATA.items()}}


def code_paths():
    return [Path(U.__file__).with_name(module + ".py") for module in (*U.MODULES, "release_inputs", "universe_release")]


def validate_snapshot_contract(root, frozen):
    if frozen.get("version") != "reader_snapshot_v1" or frozen.get("contract") != R.reader_contract(REQUIRED):
        raise RuntimeError("unsupported/incomplete independent reader contract")
    if set(frozen.get("code", {})) != {str(path.resolve()) for path in code_paths()}:
        raise RuntimeError("unsupported/incomplete independent code inventory")
    external = frozen.get("external", {})
    if set(external) != {"binding_document", "hand_verdicts", "french"}:
        raise RuntimeError("unsupported/incomplete independent external inventory")
    for name, path in (("hand_verdicts", U.HAND_CHECK), ("french", U.FRENCH_BREAKPOINTS)):
        if external[name].get("path") != str(path.resolve()):
            raise RuntimeError("substituted external evidence")
    binding_path = Path(external["binding_document"]["path"])
    doc = C.read_json(binding_path)
    if doc.get("version") != "reader_binding_v1" or doc.get("products") != frozen.get("products"):
        raise RuntimeError("unsupported/substituted independent binding document")
    if set(frozen.get("products", {})) != set(R.reader_contract(REQUIRED)):
        raise RuntimeError("incomplete reader products")


def publish_pointer(stage: Path, generation: Path, manifest: Path):
    """Publish only a complete immutable generation. Previous pointer is retained."""
    doc = C.read_json(manifest)
    expected = {f"year={year}/universe.parquet" for year in REQUIRED}
    if (doc.get("stage") != STAGE or doc.get("status") != "complete"
            or doc.get("readiness") != "NOT_PRODUCTION_READY" or doc.get("required_years") != list(REQUIRED)
            or set(doc.get("files", {})) != expected):
        raise RuntimeError("incomplete generation cannot publish")
    row = {"generation": generation.name, "manifest": R.identity(manifest), "status": "NOT_PRODUCTION_READY"}
    pointer = stage / "current.json"
    pointer_transaction(stage, row, C.read_json(pointer) if pointer.exists() else None)


def pointer_transaction(stage, current, previous):
    journal = stage / "pointer-transaction.json"
    if journal.exists():
        raise RuntimeError("unfinished pointer transaction; recover under writer lock")
    C.write_json_atomic(journal, {"version": "pointer_transaction_v1", "current": current, "previous": previous})
    finish_pointer_transaction(stage)


def finish_pointer_transaction(stage):
    journal = stage / "pointer-transaction.json"
    row = C.read_json(journal)
    if row.get("version") != "pointer_transaction_v1":
        raise RuntimeError("unsupported pointer transaction")
    if row["previous"] is not None:
        C.write_json_atomic(stage / "previous.json", row["previous"])
    C.write_json_atomic(stage / "current.json", row["current"])
    journal.unlink()


@contextmanager
def writer_lock(stage):
    stage.mkdir(parents=True, exist_ok=True)
    lock = stage / ".writer.lock"
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError as exc:
        raise RuntimeError("release writer active or interrupted; inspect retained lock, never bypass") from exc
    try:
        os.write(fd, str(os.getpid()).encode())
        yield
    finally:
        os.close(fd)
        lock.unlink()


def build(bindings_path: Path, generation_id: str, years=REQUIRED, max_new_years=None):
    with writer_lock(C.build_root() / STAGE):
        if (C.build_root() / STAGE / "pointer-transaction.json").exists():
            raise RuntimeError("unfinished pointer transaction; recover before building")
        return _build(bindings_path, generation_id, years, max_new_years)


def _build(bindings_path: Path, generation_id: str, years=REQUIRED, max_new_years=None):
    if type(max_new_years) is not int and max_new_years is not None or max_new_years is not None and max_new_years < 0:
        raise ValueError("max-new-years must be nonnegative")
    if not set(years).issubset(REQUIRED):
        raise ValueError("years outside original inventory")
    if not generation_id or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in generation_id):
        raise ValueError("invalid generation id")
    root = C.build_root()
    contract = R.reader_contract(REQUIRED)
    code = code_paths()
    binding_document = C.read_json(bindings_path)
    if binding_document.get("version") != "reader_binding_v1":
        raise RuntimeError("unsupported binding document version")
    frozen = R.snapshot(root, contract, binding_document["products"], code,
                        {"binding_document": bindings_path, "hand_verdicts": U.HAND_CHECK, "french": U.FRENCH_BREAKPOINTS})
    validate_snapshot_contract(root, frozen)
    stage = root / STAGE
    generation = stage / "generations" / generation_id
    if (generation / "manifest.json").exists():
        raise RuntimeError("immutable completed generation; validate or create a new generation")
    candidate = Candidate(generation, frozen, root)
    native = native_adapter(root, generation)
    # Distinct DB avoids touching legacy or another lane's scratch.
    con = C.connect(memory="380MB", threads=1, temp_dir=root / "_tmp" / f"universe-release-{generation_id}", db_file=f"release-{uuid.uuid4().hex}.duckdb")
    daily, units, new = {}, {}, 0
    try:
        schema = None
        # Output schema is established from actual native output query per build;
        # a resumed schema is additionally pinned by each admitted unit receipt.
        for year in REQUIRED:
            R.recheck(root, frozen)
            done = candidate.admit(con, year, schema)
            if done is None and year in years and (max_new_years is None or new < max_new_years):
                if new == 0:
                    native.load_issuer_tables(con)
                native.build_year(con, year)
                schema_new = [list(row[:2]) for row in con.execute(f"DESCRIBE ({native.output_sql()})").fetchall()]
                if schema is not None and schema_new != schema:
                    raise RuntimeError("native output schema changed across years")
                schema = schema_new
                yd = {row[0].isoformat(): dict(zip(U.DAILY_COLS, row[1:])) for row in native.year_receipt(con)}
                C.copy_to_parquet(con, native.output_sql(), candidate.payload(year), row_group_size=32768)
                done = candidate.seal(con, year, yd, schema)
                new += 1
            if done:
                schema = done["keys"]["schema"]
                units[str(year)] = done
                daily.update(done["daily"])
        R.recheck(root, frozen)
        missing = [year for year in REQUIRED if str(year) not in units]
        if missing:
            return {"status": "partial_cache_only", "published": False, "missing_years": missing,
                    "qualification": "BYTE_IDENTITY_BOUND", "readiness": "NOT_PRODUCTION_READY"}
        evidence = measurements(con, native, daily)
        C.write_json_atomic(generation / "validation.json", evidence)
        R.recheck(root, frozen)
        for year in REQUIRED:
            candidate.admit(con, year, schema)
        expected_outputs = {f"year={year}/universe.parquet": units[str(year)]["payload"] for year in REQUIRED}
        if C.output_hashes(generation) != expected_outputs:
            raise RuntimeError("unsupported/changed generation output inventory")
        manifest = {"stage": STAGE, "schema": SCHEMA, "status": "complete", "readiness": "NOT_PRODUCTION_READY",
                    "qualification": "BYTE_IDENTITY_BOUND", "source_quality_certified": False,
                    "rule": U.RULE, "required_years": list(REQUIRED), "original_gates": U.GATES,
                    "snapshot_sha256": R.digest(frozen), "snapshot": R.identity(generation / "snapshot.json"),
                    "files": expected_outputs,
                    "receipts": {f"year={year}/receipt.json": R.identity(generation / f"year={year}" / "receipt.json") for year in REQUIRED},
                    "validation": R.identity(generation / "validation.json"),
                    "gate_status": {key: "PASS" if row["pass"] else "FAIL" for key, row in evidence["gates"].items()},
                    "unresolved": ["Task2 R5-1/R5-2 legacy cache defects remain", "French historical 10/60 FAIL; must remeasure", "golden/labels/evaluation/consumer acceptance not implemented"]}
        C.write_json_atomic(generation / "manifest.json", manifest)
        R.recheck(root, frozen)
        publish_pointer(stage, generation, generation / "manifest.json")
        return {"status": "candidate_published", "published": True, "readiness": "NOT_PRODUCTION_READY", "gate_status": manifest["gate_status"]}
    finally:
        con.close()


def validate_generation(root: Path, generation: Path, con):
    manifest_identity = R.identity(generation / "manifest.json")
    manifest = C.read_json(generation / "manifest.json")
    frozen = C.read_json(generation / "snapshot.json")
    validate_snapshot_contract(root, frozen)
    invariants = {"stage": STAGE, "schema": SCHEMA, "status": "complete", "readiness": "NOT_PRODUCTION_READY",
                  "qualification": "BYTE_IDENTITY_BOUND", "source_quality_certified": False, "rule": U.RULE,
                  "original_gates": U.GATES}
    if any(R.digest(manifest.get(name)) != R.digest(value) for name, value in invariants.items()) or manifest.get("source_quality_certified") is not False:
        raise RuntimeError("unsupported generation qualification/schema/gates")
    if manifest["required_years"] != list(REQUIRED) or manifest["snapshot_sha256"] != R.digest(frozen):
        raise RuntimeError("incomplete/stale generation")
    if set(manifest["files"]) != {f"year={year}/universe.parquet" for year in REQUIRED} or C.output_hashes(generation) != manifest["files"]:
        raise RuntimeError("unsupported/changed generation inventory")
    for field in ("snapshot", "validation"):
        if R.identity(generation / ("snapshot.json" if field == "snapshot" else "validation.json")) != manifest[field]:
            raise RuntimeError(f"changed {field}")
    if set(manifest.get("receipts", {})) != {f"year={year}/receipt.json" for year in REQUIRED}:
        raise RuntimeError("unsupported receipt inventory")
    evidence = C.read_json(generation / "validation.json")
    if (evidence.get("schema") != "atx.alpha-panel.universe-release.validation/v1"
            or set(evidence) != {"schema", "measurement_contract", "gates", "coverage", "counts", "sample", "daily_per_year"}
            or R.digest(evidence.get("measurement_contract")) != R.digest(measurement_contract())):
        raise RuntimeError("unsupported measurement schema/criteria/populations")
    R.recheck(root, frozen)
    candidate = Candidate(generation, frozen, root)
    schema, daily = None, {}
    for year in REQUIRED:
        done = candidate.admit(con, year, schema)
        if done is None or done["payload"] != manifest["files"].get(f"year={year}/universe.parquet"):
            raise RuntimeError("missing/changed complete unit")
        receipt = f"year={year}/receipt.json"
        if R.identity(generation / receipt) != manifest["receipts"].get(receipt):
            raise RuntimeError("changed receipt")
        schema = done["keys"]["schema"]
        daily.update(done["daily"])
    regenerated = measurements(con, native_adapter(root, generation), daily)
    if R.digest(regenerated) != R.digest(evidence):
        raise RuntimeError("measurement evidence differs from independent native replay")
    gate_status = {name: "PASS" if row["pass"] else "FAIL" for name, row in regenerated["gates"].items()}
    if manifest.get("gate_status") != gate_status:
        raise RuntimeError("manifest gate status differs from measured evidence")
    R.recheck(root, frozen)
    if (R.identity(generation / "manifest.json") != manifest_identity
            or C.output_hashes(generation) != manifest["files"]
            or R.identity(generation / "validation.json") != manifest["validation"]
            or R.identity(generation / "snapshot.json") != manifest["snapshot"]
            or any(R.identity(generation / relative) != recorded for relative, recorded in manifest["receipts"].items())):
        raise RuntimeError("generation changed during admission")
    return manifest


def resolve_current(root: Path):
    """Consumers use explicit immutable stage/path; no legacy fallback."""
    stage = root / STAGE
    if (stage / "pointer-transaction.json").exists():
        raise RuntimeError("unfinished pointer transaction; consumer admission held")
    pointer = C.read_json(stage / "current.json")
    generation = R.safe_path(stage / "generations", pointer["generation"])
    if R.identity(generation / "manifest.json") != pointer["manifest"]:
        raise RuntimeError("pointer manifest mismatch")
    return generation


def rollback(root: Path, con):
    stage = root / STAGE
    with writer_lock(stage):
        _rollback(root, con)


def _rollback(root, con):
    stage = root / STAGE
    if (stage / "pointer-transaction.json").exists():
        raise RuntimeError("unfinished pointer transaction; recover first")
    previous = C.read_json(stage / "previous.json")
    generation = R.safe_path(stage / "generations", previous["generation"])
    if R.identity(generation / "manifest.json") != previous["manifest"]:
        raise RuntimeError("rollback manifest mismatch")
    validate_generation(root, generation, con)
    current = C.read_json(stage / "current.json")
    pointer_transaction(stage, previous, current)


def recover(root, con):
    stage = root / STAGE
    with writer_lock(stage):
        row = C.read_json(stage / "pointer-transaction.json")
        for index, pointer in enumerate((row["current"], row["previous"])):
            if pointer is not None:
                generation = R.safe_path(stage / "generations", pointer["generation"])
                if R.identity(generation / "manifest.json") != pointer["manifest"]:
                    raise RuntimeError("transaction manifest mismatch")
                if index == 0:
                    # Only the new current target is being admitted. The retained
                    # prior pointer may correctly refer to an older source snapshot;
                    # rollback will fully validate it before any future admission.
                    validate_generation(root, generation, con)
        finish_pointer_transaction(stage)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("contract", "build", "validate", "rollback", "recover"))
    parser.add_argument("--bindings", type=Path)
    parser.add_argument("--generation")
    parser.add_argument("--years", type=U._years, default=REQUIRED)
    parser.add_argument("--max-new-years", type=int)
    args = parser.parse_args(argv)
    if args.command == "contract":
        import json
        print(json.dumps({"version": "reader_binding_v1", "contract": R.reader_contract()}, indent=2))
    elif args.command == "build":
        if not args.bindings or not args.generation:
            parser.error("build requires --bindings and --generation")
        print(build(args.bindings, args.generation, args.years, args.max_new_years))
    else:
        root = C.build_root()
        con = C.connect(memory="380MB", threads=1, db_file=f"release-check-{uuid.uuid4().hex}.duckdb")
        try:
            if args.command == "rollback":
                rollback(root, con)
            elif args.command == "recover":
                recover(root, con)
            else:
                validate_generation(root, resolve_current(root), con)
        finally:
            con.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
