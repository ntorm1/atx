"""Licensed-data adapter contract (tier1-v3 S8.1; ruling D3: buy nothing, ship adapters and substitutes).

Each adapter (``licensed/<name>.py``) turns a vendor delivery, landed as served under ``raw_dir`` with a
``receipts.jsonl``, into stage tables with one shared contract (``docs/LICENSED_ADAPTERS.md``):

* **Keys.** ``security_id`` (TickerHistory3 securityID) and ``cik``, resolved by ``identity.IdentityResolver`` from
  the vendor's native id, CUSIP or ticker on ``id_date``; ``link_tier`` says how (``vendor_native``,
  ``cusip_dated``, ``cusip_undated``, ``ticker_dated``, ``ambiguous``, ``unmapped``) and ``cik_link_tier`` repeats
  the lake link table's tier. Unmapped and ambiguous rows are kept (no survivorship by identity).
* **Clocks** (naive UTC). ``vendor_snapshot_at`` is the vendor's own as-of time of the record;
  ``delivered_at`` the file's receipt time; ``available_at`` the point-in-time clock. The adapter computes a
  product rule clock (``rule_available_at``: e.g. STATPERS + publication lag, activation time, delivery schedule).
  ``available_at`` = rule clock for rows of a ``pit_archive`` file (the vendor attests an as-first-published
  history), and ``greatest(rule, delivered_at)`` for ``daily`` pulls and ``backfill`` files. A file without a
  receipt is a ``backfill`` delivered at build time, so a vendor as-of backfill can never reach the past.
  ``clock_basis`` records which clock won; ``vintage_risk`` marks backfill rows and rule-level risks.
* **Vintages.** Revisions, restatements and new versions are new rows. ``validate`` requires unique keys, non-null
  clocks, ``vendor_snapshot_at <= available_at <= build_time`` (no future clocks), ``available_at``
  non-decreasing along each vintage series in snapshot order, and a consistent ``link_tier``.
* **Output.** ``load(raw_dir) -> Stage``: Parquet per table (row groups <= 32768) and a manifest with the schema id,
  the code SHA-256, every output's SHA-256, every raw input's SHA-256 and the identity input manifests' SHA-256.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import tempfile
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, ClassVar

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

PACKAGE_DIR = Path(__file__).resolve().parent
LAKE_ROOT = PACKAGE_DIR.parents[2] / "data" / "alpha_panel" / "v1"
LINK_TIERS = ("vendor_native", "cusip_dated", "cusip_undated", "ticker_dated", "ambiguous", "unmapped")
MAPPED_TIERS = LINK_TIERS[:4]
HISTORY_MODES = ("pit_archive", "daily", "backfill")
CLOCK_BASES = ("vendor_pit", "publication_lag", "floor", "delivery", "structure_guard")
ROW_GROUP = 32768
MARK_HOUR_UTC = 22

ID_FIELDS = [
    pa.field("security_id", pa.int64()), pa.field("cik", pa.int64()),
    pa.field("link_tier", pa.string()), pa.field("cik_link_tier", pa.string()),
    pa.field("vendor_security_id", pa.string()), pa.field("cusip", pa.string()), pa.field("ticker", pa.string()),
    pa.field("id_date", pa.date32()),
]
CLOCK_FIELDS = [
    pa.field("vendor_snapshot_at", pa.timestamp("us")), pa.field("delivered_at", pa.timestamp("us")),
    pa.field("available_at", pa.timestamp("us")), pa.field("clock_basis", pa.string()),
    pa.field("history_mode", pa.string()), pa.field("vintage_risk", pa.bool_()),
]
LINEAGE_FIELDS = [pa.field("source_file", pa.string()), pa.field("source_file_sha256", pa.string())]
# columns every adapter table SQL must return besides its payload
RULE_COLUMNS = ("_file", "cusip", "ticker", "vendor_security_id", "native_security_id", "id_date",
                "vendor_snapshot_at", "rule_available_at", "rule_basis", "rule_vintage_risk", "_reject")

MACROS = r"""
CREATE OR REPLACE MACRO lic_str(x) AS nullif(trim(CAST(x AS VARCHAR)), '');
CREATE OR REPLACE MACRO lic_num(x) AS TRY_CAST(lic_str(x) AS DOUBLE);
CREATE OR REPLACE MACRO lic_int(x) AS CAST(round(TRY_CAST(lic_str(x) AS DOUBLE)) AS BIGINT);
CREATE OR REPLACE MACRO lic_date(x) AS coalesce(TRY_CAST(lic_str(x) AS DATE),
    CAST(try_strptime(lic_str(x), '%Y%m%d') AS DATE), CAST(try_strptime(lic_str(x), '%m/%d/%Y') AS DATE));
CREATE OR REPLACE MACRO lic_time(x) AS coalesce(TRY_CAST(lic_str(x) AS TIME),
    CAST(try_strptime(lpad(lic_str(x), 6, '0'), '%H%M%S') AS TIME));
CREATE OR REPLACE MACRO lic_ts(x) AS TRY_CAST(replace(replace(lic_str(x), 'T', ' '), 'Z', '') AS TIMESTAMP);
CREATE OR REPLACE MACRO lic_utc(d, t, zone) AS CAST(timezone(zone, CAST(d AS DATE) + t) AS TIMESTAMP);
CREATE OR REPLACE MACRO lic_next_weekday(d) AS
    CAST(d AS DATE) + CAST(CASE isodow(CAST(d AS DATE)) WHEN 5 THEN 3 WHEN 6 THEN 2 ELSE 1 END AS INTEGER);
CREATE OR REPLACE MACRO lic_ticker_key(x) AS nullif(regexp_replace(trim(CAST(x AS VARCHAR)), '[./\s-]', '', 'g'), '');
"""


class ContractViolation(RuntimeError):
    """Raised by ``Adapter.load(strict=True)`` when validation fails."""

    def __init__(self, report: ValidationReport) -> None:
        self.report = report
        super().__init__("; ".join(f"{c['table']}.{c['check']}: {c['n_bad']}" for c in report.failures))


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0)


def connect(memory: str = "250MB", threads: int = 2) -> duckdb.DuckDBPyConnection:
    """In-memory DuckDB (UTC session, bounded) with the ``lic_*`` parsing and clock macros."""
    spill = Path(tempfile.gettempdir()) / f"atx-licensed-spill-{os.getpid()}"
    con = duckdb.connect(config={"memory_limit": memory, "threads": str(threads), "preserve_insertion_order": "true",
                                 "temp_directory": str(spill)})
    con.execute("SET TimeZone='UTC'")
    con.execute(MACROS)
    return con


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as fh:
        for blob in iter(lambda: fh.read(1 << 22), b""):
            h.update(blob)
    return h.hexdigest()


def code_identity() -> dict[str, Any]:
    """SHA-256 of every ``licensed/*.py`` (raw and LF-normalised bytes)."""
    out: dict[str, Any] = {}
    for path in sorted(PACKAGE_DIR.glob("*.py")):
        blob = path.read_bytes()
        out[path.name] = {"sha256": hashlib.sha256(blob).hexdigest(),
                          "sha256_lf": hashlib.sha256(blob.replace(b"\r\n", b"\n")).hexdigest()}
    return out


# ---------------------------------------------------------------------------------------------------- receipts


def write_receipt(raw_dir: Path, path: Path, *, fetched_at: dt.datetime, history_mode: str,
                  url: str | None = None, http_status: int | None = None) -> dict[str, Any]:
    """Append the lane-rule receipt (url, bytes, sha256, http_status, fetched_at) plus ``history_mode``."""
    if history_mode not in HISTORY_MODES:
        raise ValueError(f"history_mode {history_mode!r} not in {HISTORY_MODES}")
    rec = {"file": Path(path).name, "url": url, "bytes": Path(path).stat().st_size, "sha256": sha256_file(path),
           "http_status": http_status, "fetched_at": fetched_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
           "history_mode": history_mode}
    with (Path(raw_dir) / "receipts.jsonl").open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(rec, sort_keys=True) + "\n")
    return rec


def read_receipts(raw_dir: Path, files: Iterable[Path]) -> pa.Table:
    """One row per raw file: receipt fields when present, else sha256 computed here and no delivery clock."""
    recs: dict[str, dict[str, Any]] = {}
    path = Path(raw_dir) / "receipts.jsonl"
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                rec = json.loads(line)
                recs[rec["file"]] = rec  # the last receipt of a file wins (a re-landing)
    rows = {"file": [], "sha256": [], "delivered_at": [], "history_mode": []}
    for f in files:
        rec = recs.get(f.name, {})
        sha = sha256_file(f)
        if rec and rec.get("sha256") not in (None, sha):
            raise ValueError(f"{f.name}: receipt sha256 {rec['sha256']} != file {sha}")
        fetched = rec.get("fetched_at")
        rows["file"].append(f.name)
        rows["sha256"].append(sha)
        rows["delivered_at"].append(dt.datetime.fromisoformat(fetched.replace("Z", "")) if fetched else None)
        rows["history_mode"].append(rec.get("history_mode"))
    return pa.table(rows, schema=pa.schema([("file", pa.string()), ("sha256", pa.string()),
                                            ("delivered_at", pa.timestamp("us")), ("history_mode", pa.string())]))


# ---------------------------------------------------------------------------------------------------- specs


@dataclass(frozen=True)
class TableSpec:
    """One adapter output table: payload columns, unique key, vintage series and the raw files it reads."""

    name: str
    payload: tuple[pa.Field, ...]
    key: tuple[str, ...]
    series: tuple[str, ...]
    raw_glob: str
    raw_columns: tuple[str, ...]
    aliases: dict[str, str] = field(default_factory=dict)
    stale_days: int | None = None
    pit_rule: str = ""

    @property
    def schema(self) -> pa.Schema:
        return pa.schema(ID_FIELDS + list(self.payload) + CLOCK_FIELDS + LINEAGE_FIELDS)

    def empty(self) -> pa.Table:
        return self.schema.empty_table()


@dataclass(frozen=True)
class Substitute:
    """The free lake stage that stands in for a licensed product until D3 is revisited."""

    stage: str
    owner: str
    keys: tuple[str, ...]
    columns: dict[str, str]
    note: str

    def resolve(self, root: Path | None = None) -> dict[str, Any]:
        """Existence, files, schema coverage and manifest status of the substitute stage under ``root``."""
        root = Path(root or os.environ.get("ATX_ALPHA_PANEL_ROOT", LAKE_ROOT))
        path = root / self.stage
        files = [path] if path.is_file() else sorted(path.glob("**/*.parquet")) if path.is_dir() else []
        cols: list[str] = pq.read_schema(files[0]).names if files else []
        manifest = (path.parent if path.is_file() else path) / "manifest.json"
        status = json.loads(manifest.read_text(encoding="utf-8")).get("status") if manifest.exists() else None
        wanted = list(self.keys) + list(self.columns.values())
        return {"stage": self.stage, "owner": self.owner, "exists": bool(files), "files": len(files),
                "manifest_status": status, "columns_present": [c for c in wanted if c in cols],
                "columns_missing": [c for c in wanted if c not in cols]}


@dataclass
class ValidationReport:
    build_time: dt.datetime
    checks: list[dict[str, Any]] = field(default_factory=list)
    stats: dict[str, Any] = field(default_factory=dict)

    def add(self, table: str, check: str, n_bad: int, detail: Any = None, fatal: bool = True) -> None:
        self.checks.append({"table": table, "check": check, "n_bad": int(n_bad), "ok": int(n_bad) == 0,
                            "fatal": fatal, "detail": detail})

    @property
    def failures(self) -> list[dict[str, Any]]:
        return [c for c in self.checks if c["fatal"] and not c["ok"]]

    def failed(self, table: str, check: str) -> bool:
        return any(c["table"] == table and c["check"] == check and not c["ok"] for c in self.checks)

    def to_json(self) -> dict[str, Any]:
        return {"build_time": self.build_time.isoformat(), "checks": self.checks, "stats": self.stats,
                "failures": len(self.failures)}


@dataclass
class Stage:
    adapter: str
    tables: dict[str, pa.Table]
    report: ValidationReport
    rejects: dict[str, dict[str, int]]
    out_dir: Path | None = None
    manifest: dict[str, Any] | None = None


# ---------------------------------------------------------------------------------------------------- adapter


def aliased_select(con: duckdb.DuckDBPyConnection, files: list[Path], columns: tuple[str, ...],
                   aliases: dict[str, str]) -> str:
    """SELECT the raw files' columns renamed to ``columns`` (VARCHAR; absent ones NULL) plus ``_file``.

    A header name maps through ``aliases`` on its lower-case form, then on that form without underscores; a name
    already in ``columns`` maps to itself. The first raw column claiming a canonical name wins."""
    paths = "[" + ", ".join("'" + f.as_posix() + "'" for f in files) + "]"
    if all(f.suffix.lower() == ".parquet" for f in files):
        reader = f"read_parquet({paths}, union_by_name = true, filename = true)"
    else:
        # DuckDB's default max_line_size (2 MB) bounds one transcript component; its read buffer is 16x that
        reader = f"read_csv({paths}, all_varchar = true, header = true, union_by_name = true, filename = true)"
    names = [r[0] for r in con.execute(f"DESCRIBE SELECT * FROM {reader}").fetchall() if r[0] != "filename"]
    chosen: dict[str, str] = {}
    for raw in names:
        low = raw.strip().lower()
        canon = aliases.get(low) or aliases.get(low.replace("_", "")) or (low if low in columns else None)
        if canon in columns and canon not in chosen:
            chosen[canon] = raw
    sel = [(f'CAST("{chosen[c]}" AS VARCHAR)' if c in chosen else "CAST(NULL AS VARCHAR)") + f' AS "{c}"'
           for c in columns]
    return f"SELECT {', '.join(sel)}, regexp_extract(filename, '[^/\\\\]+$') AS _file FROM {reader}"


class Adapter:
    """Base adapter. Subclasses set the class attributes and implement ``table_sql`` and ``mock``."""

    name: str = ""
    product: str = ""
    pit_rule: str = ""
    purchase: str = ""
    tables: ClassVar[dict[str, TableSpec]] = {}
    schema_version: int = 1

    @property
    def schema_id(self) -> str:
        return f"atx.licensed.{self.name}/v{self.schema_version}"

    @property
    def stage_name(self) -> str:
        return f"licensed_{self.name}"

    # -- to implement -------------------------------------------------------------------------------------------
    def table_sql(self, table: str, raw: str) -> str:
        """SQL over the aliased raw view ``raw`` returning ``RULE_COLUMNS`` plus the table's payload columns."""
        raise NotImplementedError

    def mock(self, raw_dir: Path, universe: Any = None, seed: int = 0) -> list[Path]:
        raise NotImplementedError

    def substitute(self) -> Substitute | None:
        return None

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        """Adapter-specific validation on the registered table ``t_<table>``."""

    # -- shared machinery ---------------------------------------------------------------------------------------
    def _normalize(self, con: duckdb.DuckDBPyConnection, spec: TableSpec, files: list[Path], identity: Any,
                   build_time: dt.datetime) -> tuple[pa.Table, dict[str, int]]:
        con.execute(f"CREATE OR REPLACE TEMP VIEW raw_{spec.name} AS "
                    f"{aliased_select(con, files, spec.raw_columns, spec.aliases)}")
        con.execute(f"CREATE OR REPLACE TEMP TABLE _n0 AS {self.table_sql(spec.name, f'raw_{spec.name}')}")
        rejects = {r[0]: int(r[1]) for r in con.execute(
            "SELECT _reject, count(*) FROM _n0 WHERE _reject IS NOT NULL GROUP BY 1").fetchall()}
        con.execute("CREATE OR REPLACE TEMP TABLE _n AS "
                    "SELECT row_number() OVER () AS _rid, * FROM _n0 WHERE _reject IS NULL")
        identity.resolve(con, "_n", "_m")
        payload = ", ".join(f'm."{f.name}"' for f in spec.payload)
        bt = f"TIMESTAMP '{build_time.isoformat(sep=' ')}'"
        sql = f"""
            WITH j AS (
                SELECT m.*, r.delivered_at, r.sha256, coalesce(r.history_mode, 'backfill') AS hm
                FROM _m m LEFT JOIN lic_receipts r ON r.file = m._file
            )
            SELECT m.security_id, m.cik, m.link_tier, m.cik_link_tier, m.vendor_security_id, m.cusip, m.ticker,
                   m.id_date, {payload},
                   m.vendor_snapshot_at, m.delivered_at,
                   CASE WHEN m.hm = 'pit_archive' THEN m.rule_available_at
                        ELSE greatest(m.rule_available_at, coalesce(m.delivered_at, {bt})) END AS available_at,
                   CASE WHEN m.hm <> 'pit_archive' AND coalesce(m.delivered_at, {bt}) > m.rule_available_at
                        THEN 'delivery' ELSE m.rule_basis END AS clock_basis,
                   m.hm AS history_mode,
                   (m.hm = 'backfill') OR coalesce(m.rule_vintage_risk, false) AS vintage_risk,
                   m._file AS source_file, m.sha256 AS source_file_sha256
            FROM j m ORDER BY m._rid
        """
        table = con.execute(sql).arrow().read_all()
        return table.cast(spec.schema), rejects

    def normalize(self, raw_dir: Path, identity: Any, build_time: dt.datetime | None = None
                  ) -> tuple[dict[str, pa.Table], dict[str, dict[str, int]], pa.Table]:
        build_time = build_time or utc_now()
        raw_dir = Path(raw_dir)
        con = connect()
        try:
            all_files: dict[str, list[Path]] = {n: sorted(raw_dir.glob(s.raw_glob)) for n, s in self.tables.items()}
            receipts = read_receipts(raw_dir, sorted({f for fs in all_files.values() for f in fs}))
            con.register("lic_receipts", receipts)
            identity.register(con)
            out: dict[str, pa.Table] = {}
            rejects: dict[str, dict[str, int]] = {}
            for name, spec in self.tables.items():
                if not all_files[name]:
                    out[name], rejects[name] = spec.empty(), {}
                    continue
                out[name], rejects[name] = self._normalize(con, spec, all_files[name], identity, build_time)
            return out, rejects, receipts
        finally:
            con.close()

    def validate(self, tables: dict[str, pa.Table], build_time: dt.datetime | None = None) -> ValidationReport:
        """The contract checks (see module docstring) plus ``extra_checks``; stats per table."""
        build_time = build_time or utc_now()
        report = ValidationReport(build_time=build_time)
        con = connect()
        bt = f"TIMESTAMP '{build_time.isoformat(sep=' ')}'"
        try:
            for name, spec in self.tables.items():
                t = tables.get(name, spec.empty())
                if t.schema != spec.schema:
                    report.add(name, "schema", 1, {"expected": spec.schema.names, "got": t.schema.names})
                    continue
                con.register(f"t_{name}", t)
                k = ", ".join(spec.key)
                dup = con.execute(f"SELECT count(*) FROM (SELECT {k} FROM t_{name} GROUP BY ALL HAVING count(*) > 1)"
                                  ).fetchone()[0]
                report.add(name, "keys_unique", dup, list(spec.key))
                nulls = con.execute(f"SELECT count(*) FROM t_{name} WHERE available_at IS NULL "
                                    "OR vendor_snapshot_at IS NULL OR clock_basis IS NULL").fetchone()[0]
                report.add(name, "clocks_present", nulls)
                report.add(name, "snapshot_before_available", con.execute(
                    f"SELECT count(*) FROM t_{name} WHERE vendor_snapshot_at > available_at").fetchone()[0])
                report.add(name, "no_future_clocks", con.execute(
                    f"SELECT count(*) FROM t_{name} WHERE available_at > {bt} OR vendor_snapshot_at > {bt} "
                    f"OR delivered_at > {bt}").fetchone()[0], build_time.isoformat())
                s = ", ".join(spec.series)
                report.add(name, "clocks_monotone_per_vintage", con.execute(f"""
                    SELECT count(*) FROM (
                        SELECT available_at, lag(available_at) OVER (PARTITION BY {s}
                               ORDER BY vendor_snapshot_at, available_at) AS prev
                        FROM t_{name}) WHERE available_at < prev""").fetchone()[0], list(spec.series))
                tiers = ", ".join(f"'{x}'" for x in LINK_TIERS)
                mapped = ", ".join(f"'{x}'" for x in MAPPED_TIERS)
                report.add(name, "link_tier_consistent", con.execute(f"""
                    SELECT count(*) FROM t_{name}
                    WHERE link_tier IS NULL OR link_tier NOT IN ({tiers})
                       OR (link_tier IN ({mapped})) <> (security_id IS NOT NULL)""").fetchone()[0])
                modes = ", ".join(f"'{x}'" for x in HISTORY_MODES)
                bases = ", ".join(f"'{x}'" for x in CLOCK_BASES)
                report.add(name, "clock_domains", con.execute(
                    f"SELECT count(*) FROM t_{name} WHERE history_mode NOT IN ({modes}) "
                    f"OR clock_basis NOT IN ({bases})").fetchone()[0])
                self.extra_checks(con, name, report)
                tier_counts = dict(con.execute(f"SELECT link_tier, count(*) FROM t_{name} GROUP BY 1").fetchall())
                n = t.num_rows
                report.stats[name] = {
                    "rows": n, "link_tiers": tier_counts,
                    "mapped_share": round(sum(v for k2, v in tier_counts.items() if k2 in MAPPED_TIERS) / n, 4) if n else None,
                    "cik_share": round(con.execute(f"SELECT count(cik) FROM t_{name}").fetchone()[0] / n, 4) if n else None,
                    "clock_basis": dict(con.execute(f"SELECT clock_basis, count(*) FROM t_{name} GROUP BY 1").fetchall()),
                }
        finally:
            con.close()
        return report

    def load(self, raw_dir: Path, *, identity: Any = None, out_dir: Path | None = None,
             build_time: dt.datetime | None = None, strict: bool = True) -> Stage:
        """Normalize ``raw_dir`` -> validated tables; write the stage to ``out_dir`` when given."""
        from .identity import IdentityResolver  # local: identity imports this module

        build_time = build_time or utc_now()
        identity = identity or IdentityResolver.from_lake()
        tables, rejects, receipts = self.normalize(raw_dir, identity, build_time)
        report = self.validate(tables, build_time)
        stage = Stage(adapter=self.name, tables=tables, report=report, rejects=rejects)
        if strict and report.failures:
            raise ContractViolation(report)
        if out_dir is not None:
            self.write(stage, Path(out_dir), receipts, identity)
        return stage

    def write(self, stage: Stage, out_dir: Path, receipts: pa.Table, identity: Any) -> dict[str, Any]:
        out_dir.mkdir(parents=True, exist_ok=True)
        files: dict[str, Any] = {}
        for name, t in stage.tables.items():
            dest = out_dir / f"{name}.parquet"
            tmp = dest.with_name(dest.name + ".partial")
            pq.write_table(t, tmp, row_group_size=ROW_GROUP, compression="zstd")
            os.replace(tmp, dest)
            files[dest.name] = {"bytes": dest.stat().st_size, "sha256": sha256_file(dest), "rows": t.num_rows}
        manifest = {
            "schema": self.schema_id, "status": "complete", "stage": self.stage_name, "adapter": self.name,
            "product": self.product, "pit_rule": self.pit_rule,
            "tables": {n: {"key": list(s.key), "series": list(s.series), "stale_days": s.stale_days,
                           "pit_rule": s.pit_rule} for n, s in self.tables.items()},
            "code": code_identity(), "files": files,
            "raw_inputs": {r["file"]: {"sha256": r["sha256"], "history_mode": r["history_mode"],
                                       "delivered_at": str(r["delivered_at"]) if r["delivered_at"] else None}
                           for r in receipts.to_pylist()},
            "input_manifests_sha256": getattr(identity, "inputs", {}),
            "rejects": stage.rejects, "validation": stage.report.to_json(),
        }
        tmp = out_dir / "manifest.json.partial"
        tmp.write_text(json.dumps(manifest, indent=2, sort_keys=True, default=str), encoding="utf-8")
        os.replace(tmp, out_dir / "manifest.json")
        stage.out_dir, stage.manifest = out_dir, manifest
        return manifest


def csv_write(path: Path, header: list[str], rows: Iterable[Iterable[Any]]) -> Path:
    """Write a vendor-layout CSV (mocks): ``None`` -> empty field; no quoting needed for mock content."""
    import csv

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        for r in rows:
            w.writerow(["" if v is None else v for v in r])
    return path
