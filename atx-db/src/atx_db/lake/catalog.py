"""``data/catalog.duckdb``: the rebuildable serving layer over the stage lake (task S1.3, ruling D1).

Built from scratch on every run from the registry, the published manifests and the Parquet schemas (no data is
copied; the database holds views, macros, comments and two small registry tables):

* one view per registered stage output: ``SELECT * FROM read_parquet('<root>/<glob>', hive_partitioning = <glob
  has key=* segments>, union_by_name = true)``;
* ``<view>_as_of(ts)`` table macro per clocked output, ``ts`` a naive UTC TIMESTAMP: rows whose clock < ts; for
  ``vintage`` outputs the latest row per key visible at ts (clock, then the registry ``order`` columns, descending);
  the clock is the output's ``clock_sql`` or its column (TIMESTAMPTZ converted to UTC, DATE + 22:00 UTC);
* ``lake_cutoff(d)``: 22:00 UTC of the session before ``d`` (the decision cutoff of session ``d``);
* ``COMMENT ON`` every view (stage, schema, clock, vintage policy, staleness, manifest SHA-256), macro and every
  documented column (``registry.COLUMN_DOCS`` and the output's ``columns``);
* tables ``lake_stages`` and ``lake_outputs`` (registry rows with the manifest SHA-256 at build time).

The database is written under ``<dest dir>/_catalog_build/<dest name>`` and moved into place (a DuckDB file stores
its own name, so building under the final name keeps rebuilds byte-identical); consumers open it
``read_only=True``. ``--dump`` prints the canonical dump (sorted DDL, view SQL, comments, table rows) and its SHA.

    python -m atx_db.lake catalog [--root R] [--out data/catalog.duckdb] [--dump]
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import time
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import duckdb

from .contract import PACKAGE_ROOT, VINTAGE_POLICIES, Output, Stage, manifest_sha, output_files

CATALOG = PACKAGE_ROOT / "data" / "catalog.duckdb"


def _q(ident: str) -> str:
    return '"' + ident.replace('"', '""') + '"'


def _lit(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def clock_expr(out: Output, types: Mapping[str, str]) -> str | None:
    """SQL for the output's clock as a naive UTC TIMESTAMP (None: no usable clock)."""
    if out.clock is None:
        return None
    if out.clock_sql:
        return out.clock_sql
    t = types.get(out.clock, "").upper()
    col = _q(out.clock)
    if t.startswith("TIMESTAMP WITH TIME ZONE"):
        return f"timezone('UTC', {col})"
    if t.startswith("TIMESTAMP"):
        return col
    if t == "DATE":
        return f"CAST({col} AS TIMESTAMP) + INTERVAL 22 HOUR"
    return None


def _source(root: Path, out: Output) -> str:
    glob = f"{root.as_posix()}/{out.glob}"
    return (f"read_parquet({_lit(glob)}, hive_partitioning = {'true' if out.hive else 'false'}, "
            f"union_by_name = true)")


def build(root: Path, stages: list[Stage], dest: Path = CATALOG, memory: str = "256MB",
          column_docs: Mapping[str, str] | None = None) -> dict[str, Any]:
    """Rebuild the catalog database from scratch; returns a receipt (counts, skipped outputs, SHA-256)."""
    from .registry import COLUMN_DOCS

    docs = dict(COLUMN_DOCS if column_docs is None else column_docs)
    t0 = time.perf_counter()
    work = dest.parent / "_catalog_build"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    tmp = work / dest.name
    con = duckdb.connect(str(tmp), config={"memory_limit": memory, "threads": "1"})
    receipt: dict[str, Any] = {"root": root.as_posix(), "views": [], "macros": [], "skipped": []}
    try:
        con.execute("SET TimeZone='UTC'")
        con.execute("""CREATE TABLE lake_stages (name VARCHAR, lane VARCHAR, schema VARCHAR, module VARCHAR,
            inputs VARCHAR[], manifest VARCHAR, manifest_sha256 VARCHAR, planned BOOLEAN, vintage VARCHAR,
            staleness VARCHAR, guard_gb DOUBLE, built_by VARCHAR, doc VARCHAR)""")
        con.execute("""CREATE TABLE lake_outputs (stage VARCHAR, view VARCHAR, file_glob VARCHAR, clock VARCHAR,
            clock_sql VARCHAR, vintage VARCHAR, keys VARCHAR[], files INTEGER, doc VARCHAR)""")
        for s in sorted(stages, key=lambda x: x.name):
            msha = manifest_sha(root, s.manifest)
            con.execute("INSERT INTO lake_stages VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        [s.name, s.lane, s.schema, s.module, list(s.inputs), s.manifest, msha, s.planned, s.vintage,
                         s.staleness, s.guard_gb, s.built_by, s.doc])
            files = output_files(root, s)
            for o in s.outputs:
                pol = s.output_vintage(o)
                n = len(files[o.view])
                if not n:
                    receipt["skipped"].append({"view": o.view, "reason": f"no file matches {o.glob}"})
                    con.execute("INSERT INTO lake_outputs VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                [s.name, o.view, o.glob, o.clock, None, pol, list(o.keys), 0, o.doc])
                    continue
                con.execute(f"CREATE VIEW {_q(o.view)} AS SELECT * FROM {_source(root, o)}")
                types = {r[0]: r[1] for r in con.execute(f"DESCRIBE {_q(o.view)}").fetchall()}
                clk = clock_expr(o, types)
                con.execute("INSERT INTO lake_outputs VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            [s.name, o.view, o.glob, o.clock, clk, pol, list(o.keys), n, o.doc])
                comment = (f"{o.doc} | stage {s.name} (schema {s.schema or 'none'}, lane {s.lane}); "
                           f"clock {o.clock or 'none'}" + (f" = {clk}" if clk and clk != _q(o.clock or '') else "") +
                           f"; vintage {pol}: {VINTAGE_POLICIES[pol]}; staleness: {s.staleness or 'n/a'}; "
                           f"{n} file(s); manifest {s.manifest} sha256 {msha or 'absent'}")
                con.execute(f"COMMENT ON VIEW {_q(o.view)} IS {_lit(comment)}")
                receipt["views"].append(o.view)
                for col in sorted(types):
                    text = o.columns.get(col) or docs.get(col)
                    if text:
                        con.execute(f"COMMENT ON COLUMN {_q(o.view)}.{_q(col)} IS {_lit(text)}")
                if clk is None:
                    if o.clock is not None:
                        receipt["skipped"].append({"view": f"{o.view}_as_of", "reason": f"clock {o.clock!r} type "
                                                   f"{types.get(o.clock)!r} not a timestamp or date"})
                    continue
                if pol == "vintage":
                    order = ", ".join([f"{clk} DESC"] + [f"{_q(c)} DESC" for c in o.order])
                    body = (f"SELECT * FROM {_q(o.view)} WHERE {clk} < CAST(ts AS TIMESTAMP) QUALIFY row_number() "
                            f"OVER (PARTITION BY {', '.join(_q(k) for k in o.keys)} ORDER BY {order}) = 1")
                    what = f"latest row per ({', '.join(o.keys)}) with clock < ts"
                else:
                    body = f"SELECT * FROM {_q(o.view)} WHERE {clk} < CAST(ts AS TIMESTAMP)"
                    what = "rows with clock < ts"
                macro = f"{o.view}_as_of"
                con.execute(f"CREATE MACRO {_q(macro)}(ts) AS TABLE {body}")
                con.execute(f"COMMENT ON MACRO TABLE {_q(macro)} IS "
                            f"{_lit(f'{o.view} as of ts (naive UTC TIMESTAMP): {what}; clock = {clk}')}")
                receipt["macros"].append(macro)
        if "calendar" in receipt["views"]:
            con.execute("CREATE MACRO lake_cutoff(d) AS (SELECT CAST(max(session_date) AS TIMESTAMP) + INTERVAL 22 "
                        "HOUR FROM calendar WHERE session_date < CAST(d AS DATE))")
            con.execute("COMMENT ON MACRO lake_cutoff IS 'decision cutoff of session d: 22:00 UTC of the previous "
                        "session (a value is visible at d only if its clock < lake_cutoff(d))'")
            receipt["macros"].append("lake_cutoff")
        con.execute("CHECKPOINT")
    finally:
        con.close()
    dest.parent.mkdir(parents=True, exist_ok=True)
    for p in (dest.with_name(dest.name + ".wal"),):
        if p.exists():
            p.unlink()
    os.replace(tmp, dest)
    shutil.rmtree(work, ignore_errors=True)
    receipt["seconds"] = round(time.perf_counter() - t0, 2)
    receipt["sha256"] = hashlib.sha256(dest.read_bytes()).hexdigest()
    receipt["bytes"] = dest.stat().st_size
    return receipt


def canonical_dump(path: Path) -> str:
    """Sorted, byte-stable text of everything the catalog defines (read-only open)."""
    con = duckdb.connect(str(path), read_only=True)
    try:
        lines: list[str] = []
        for name, sql, comment in con.execute(
                "SELECT view_name, sql, comment FROM duckdb_views() WHERE NOT internal ORDER BY view_name").fetchall():
            lines.append(f"VIEW {name}\n  {sql}\n  -- {comment}")
        for name, ftype, params, body, comment in con.execute(
                "SELECT function_name, function_type, parameters, macro_definition, comment FROM duckdb_functions() "
                "WHERE function_type IN ('macro', 'table_macro') AND NOT internal ORDER BY function_name").fetchall():
            lines.append(f"{ftype.upper()} {name}({', '.join(params)})\n  {body}\n  -- {comment}")
        for t, c, comment in con.execute(
                "SELECT table_name, column_name, comment FROM duckdb_columns() WHERE NOT internal AND comment IS NOT "
                "NULL ORDER BY table_name, column_name").fetchall():
            lines.append(f"COLUMN {t}.{c} -- {comment}")
        for t in ("lake_stages", "lake_outputs"):
            for row in con.execute(f"SELECT * FROM {t} ORDER BY ALL").fetchall():
                lines.append(f"ROW {t} " + json.dumps(row, default=str))
        return "\n".join(lines) + "\n"
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    import argparse

    from . import registry
    from .contract import default_root

    ap = argparse.ArgumentParser(prog="python -m atx_db.lake catalog", description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=CATALOG)
    ap.add_argument("--dump", action="store_true", help="print the canonical dump SHA-256 (and write <out>.dump.txt)")
    args = ap.parse_args(argv)
    root = args.root or default_root()
    rec = build(root, registry.load(strict=False), args.out)
    if args.dump:
        text = canonical_dump(args.out)
        rec["dump_sha256"] = hashlib.sha256(text.encode("utf-8")).hexdigest()
        args.out.with_name(args.out.name + ".dump.txt").write_text(text, encoding="utf-8")
    print(json.dumps({k: (v if k not in ("views", "macros") else len(v)) for k, v in rec.items()}, indent=1))
    return 0
