"""Request section 5: producer-side panel metrics, read as metadata (nothing here reads a return-conditioned value).

1. Coverage per field per year: finite share over ``member_equity`` cells, over CIK-linked member_equity cells
   (any tier, strict only, point-in-time tiers strict + name) and, for fundamental items with a structural rule,
   over member_equity cells whose ``fin_template`` does not make the item structurally absent.
2. Distribution per field per year over finite member_equity values: p0.1, p1, p50, p99, p99.9, share of exact
   zeros, share of negatives. Return fields (``RETURN_FIELDS``) get no distribution for windows from 2023 on
   (request section 6: validation and holdout years carry no statistic of returns).
3. Vintage audit per source per year: share of member_equity cells whose visible value is a republication or
   carries a vintage risk (short interest settled before 2021-06, amended filings, fallback filing clocks, the
   survivorship-biased backfill identity tier, FINRA short-volume files downloaded after the fact, and every
   ``*_vintage_risk`` column of the panel).
4. Identity audit: ``identity/audit.json`` (identity_table.py), summarised here.
5. Field change log: panel columns added / dropped / retyped against the previous panel build
   (``panel_schema_history.json``), with code SHA-256 of both builds.

Outputs: ``metrics/coverage.parquet``, ``metrics/distribution.parquet``, ``metrics/vintage.json``,
``metrics/changelog.json``, ``docs/ALPHA_PANEL_METRICS.md``. Windows: calendar years 2018-2026 and the
consumer's TRAIN score window 2020-01-01..2022-12-31 (``train_2020_2022``).
"""

from __future__ import annotations

import datetime as dt
import sys
from typing import Any

from . import common as C

KEYS = {"session_date", "security_id", "sidx", "ticker", "member", "member_equity", "cik"}
NUMERIC = ("DOUBLE", "FLOAT", "BIGINT", "INTEGER", "SMALLINT", "TINYINT", "HUGEINT", "UBIGINT")
QUANTILES = (0.001, 0.01, 0.5, 0.99, 0.999)
# fields per scan: each batch is first materialised (its columns + the basis flags) into the file-backed scratch
# database, so the wide panel parquet is decoded once per batch and the aggregates run over a narrow table
BATCH = 12
BASIS_COLS = ("cik", "link_tier", "currency", "fin_template")
REPUBLICATION_SI_BEFORE = "2021-06-01"
# request section 6: no statistic of returns for validation / holdout years; coverage is still reported
RETURN_FIELDS = {"ret", "mkt_ret", "ret_intraday", "ret_overnight"}
OOS_FROM = "2023-01-01"
# linked cells whose visible filing reports in USD (or has no filing): money items of other reporting currencies
# are NaN by rule (no point-in-time FX source), so this basis separates that structural gap from missing values
USD_BASIS = "cik IS NOT NULL AND coalesce(currency, 'USD') = 'USD'"
# fin_template values under which an item is structurally absent (fundamentals lane; extended from its manifest)
DEFAULT_STRUCTURAL = {"gp_ttm": ("bank", "insurer"), "cogs_ttm": ("bank", "insurer"), "cogs": ("bank", "insurer"),
                      "invt": ("bank", "insurer"), "inventory": ("bank", "insurer")}


def _schema(con, glob: str) -> list[tuple[str, str]]:
    return [(r[0], r[1]) for r in
            con.execute(f"DESCRIBE SELECT * FROM read_parquet('{glob}', union_by_name = true)").fetchall()]


def _windows() -> list[tuple[str, str, str]]:
    w = [(str(y), f"{y}-01-01", f"{y}-12-31") for y in range(2018, 2027)]
    w.append(("train_2020_2022", "2020-01-01", "2022-12-31"))
    return w


def _structural(root) -> dict[str, tuple[str, ...]]:
    m = root / "fundamentals" / "manifest.json"
    rules = dict(DEFAULT_STRUCTURAL)
    if m.exists():
        got = C.read_json(m).get("structural_na")
        if isinstance(got, dict):
            rules.update({k: tuple(v) for k, v in got.items()})
    return rules


def measure(con, glob: str, schema: list[tuple[str, str]], structural: dict[str, tuple[str, ...]],
            windows: list[tuple[str, str, str]] | None = None):
    names = {n for n, _ in schema}
    fields = [(n, t) for n, t in schema if n not in KEYS]
    has_template = "fin_template" in names
    cov_rows: list[dict[str, Any]] = []
    dist_rows: list[dict[str, Any]] = []
    for label, lo, hi in (windows if windows is not None else _windows()):
        # the window's member_equity cells only (plus the flags the bases need); columns read per batch
        where = f"member_equity AND session_date BETWEEN DATE '{lo}' AND DATE '{hi}'"
        n_cells = con.execute(f"SELECT count(*) FROM read_parquet('{glob}', union_by_name = true) WHERE {where}"
                              ).fetchone()[0]
        if not n_cells:
            continue
        for i in range(0, len(fields), BATCH):
            batch = fields[i:i + BATCH]
            parts = []
            for n, t in batch:
                q = f'"{n}"'
                numeric = any(t.upper().startswith(k) for k in NUMERIC) or t.upper().startswith("DECIMAL")
                ok = f"isfinite(CAST({q} AS DOUBLE))" if numeric else f"{q} IS NOT NULL"
                parts.append(f"count(*) FILTER (WHERE {ok}) AS \"{n}__all\"")
                parts.append(f"count(*) FILTER (WHERE {ok} AND cik IS NOT NULL) AS \"{n}__linked\"")
                parts.append(f"count(*) FILTER (WHERE {ok} AND link_tier = 'strict') AS \"{n}__strict\"")
                parts.append(f"count(*) FILTER (WHERE {ok} AND link_tier IN ('strict', 'name')) AS \"{n}__pit\"")
                parts.append(f"count(*) FILTER (WHERE {ok} AND {USD_BASIS}) AS \"{n}__usd\"")
                if has_template and n in structural:
                    lst = ", ".join(f"'{x}'" for x in structural[n])
                    parts.append(f"count(*) FILTER (WHERE {ok} AND coalesce(fin_template, '') NOT IN ({lst})) AS \"{n}__ns\"")
                    parts.append(f"count(*) FILTER (WHERE coalesce(fin_template, '') NOT IN ({lst})) AS \"{n}__nsden\"")
                if numeric and not (n in RETURN_FIELDS and lo >= OOS_FROM):
                    v = f"CAST({q} AS DOUBLE)"
                    qs = ", ".join(str(x) for x in QUANTILES)
                    parts.append(f"quantile_cont({v}, [{qs}]) FILTER (WHERE {ok}) AS \"{n}__q\"")
                    parts.append(f"count(*) FILTER (WHERE {ok} AND {v} = 0) AS \"{n}__zero\"")
                    parts.append(f"count(*) FILTER (WHERE {ok} AND {v} < 0) AS \"{n}__neg\"")
            need = [c for c in BASIS_COLS if c in names] + [n for n, _ in batch if n not in BASIS_COLS]
            con.execute("DROP TABLE IF EXISTS w")
            con.execute(f"""CREATE TABLE w AS SELECT {", ".join(f'"{c}"' for c in need)}
                FROM read_parquet('{glob}', union_by_name = true) WHERE {where}""")
            base = con.execute(f"""
                SELECT count(*) AS n, count(*) FILTER (WHERE cik IS NOT NULL) AS nl,
                       count(*) FILTER (WHERE link_tier = 'strict') AS ns,
                       count(*) FILTER (WHERE link_tier IN ('strict', 'name')) AS np,
                       count(*) FILTER (WHERE {USD_BASIS}) AS nu, {", ".join(parts)}
                FROM w
            """)
            row = dict(zip([d[0] for d in base.description], base.fetchone(), strict=True))
            for n, t in batch:
                a = row[f"{n}__all"]

                def share(k, d):
                    return round(k / d, 4) if d else None
                rec = {"window": label, "field": n, "type": t, "cells": row["n"],
                       "member_equity": share(a, row["n"]), "linked": share(row[f"{n}__linked"], row["nl"]),
                       "linked_strict": share(row[f"{n}__strict"], row["ns"]),
                       "linked_pit": share(row[f"{n}__pit"], row["np"]),
                       "linked_usd": share(row[f"{n}__usd"], row["nu"]),
                       "excl_structural": share(row.get(f"{n}__ns") or 0, row.get(f"{n}__nsden") or 0)
                       if f"{n}__ns" in row else None}
                cov_rows.append(rec)
                if f"{n}__q" in row and a:
                    q = row[f"{n}__q"] or [None] * len(QUANTILES)
                    dist_rows.append({"window": label, "field": n, "n": a,
                                      **{f"p{str(p * 100).rstrip('0').rstrip('.')}": q[j] for j, p in enumerate(QUANTILES)},
                                      "share_zero": share(row[f"{n}__zero"], a), "share_negative": share(row[f"{n}__neg"], a)})
        print(label, n_cells, flush=True)
    return cov_rows, dist_rows


def vintage(con, glob: str, names: set[str]) -> dict[str, Any]:
    exprs = {
        "short_interest_republished": (f"si_settlement < DATE '{REPUBLICATION_SI_BEFORE}'", "si_shares IS NOT NULL"),
        "short_volume_downloaded_later": ("TRUE", "sv_total_volume IS NOT NULL"),
        "fundamentals_amended_filing": ("fund_form LIKE '%/A'", '"at" IS NOT NULL OR be IS NOT NULL'),
        "fundamentals_fallback_clock": ("lower(fund_clock_basis) LIKE '%fc1%'", '"at" IS NOT NULL OR be IS NOT NULL'),
        "identity_backfill_tier": ("link_tier = 'backfill'", "cik IS NOT NULL"),
    }
    for n in sorted(names):
        if n.endswith("_vintage_risk"):
            exprs[n] = (f'"{n}"', f'"{n}" IS NOT NULL')
    out: dict[str, Any] = {}
    for key, (num, den) in exprs.items():
        cols = {c for c in ("si_settlement", "si_shares", "sv_total_volume", "fund_form", "fund_clock_basis", "at",
                            "be", "link_tier", "cik") if c in num or c in den}
        if not cols <= names and not key.endswith("_vintage_risk"):
            continue
        rows = con.execute(f"""
            SELECT year(session_date), count(*) FILTER (WHERE ({den}) AND ({num})), count(*) FILTER (WHERE {den})
            FROM read_parquet('{glob}', union_by_name = true) WHERE member_equity GROUP BY 1 ORDER BY 1
        """).fetchall()
        out[key] = {str(y): {"share": round(a / b, 4) if b else None, "cells_with_value": b} for y, a, b in rows}
    return out


def identity_from_panel(con, glob: str) -> dict[str, Any]:
    """Per-year link-tier shares over member_equity cells of the assembled panel; dead lines = delisting_date set."""
    rows = con.execute(f"""
        SELECT year(session_date), delisting_date IS NOT NULL AS dead, count(*),
               count(*) FILTER (WHERE link_tier = 'strict'), count(*) FILTER (WHERE link_tier = 'name'),
               count(*) FILTER (WHERE link_tier = 'backfill'), count(*) FILTER (WHERE cik IS NULL)
        FROM read_parquet('{glob}', union_by_name = true) WHERE member_equity GROUP BY ALL ORDER BY ALL
    """).fetchall()
    out: dict[str, Any] = {}
    for y, dead, n, st, nm, bf, un in rows:
        slot = out.setdefault(str(y), {"cells": 0, "strict": 0, "name": 0, "backfill": 0, "unlinked": 0})
        for k, v in (("cells", n), ("strict", st), ("name", nm), ("backfill", bf), ("unlinked", un)):
            slot[k] += v
        if dead:
            slot["dead_cells"], slot["dead_unlinked"] = n, un
    for slot in out.values():
        c = max(slot["cells"], 1)
        slot["share_strict"] = round(slot["strict"] / c, 4)
        slot["share_strict_or_name"] = round((slot["strict"] + slot["name"]) / c, 4)
        slot["share_any"] = round(1 - slot["unlinked"] / c, 4)
        if slot.get("dead_cells"):
            slot["dead_share_any"] = round(1 - slot["dead_unlinked"] / slot["dead_cells"], 4)
    return out


def changelog(root, schema: list[tuple[str, str]]) -> dict[str, Any]:
    hist_path = root / "panel_schema_history.json"
    hist = C.read_json(hist_path) if hist_path.exists() else {}
    prev = {n: t for n, t in hist.get("v1_2022", {}).get("columns", [])}
    cur = dict(schema)
    return {"previous": {"build": "panel v1 (2026-09-27, panel.py before v2)", "manifest_sha256":
                         hist.get("v1_manifest_sha256"), "columns": len(prev)},
            "current": {"build": "panel v2", "code": C.code_identity("panel", "security_master", "identity_table"),
                        "columns": len(cur)},
            "added": sorted(set(cur) - set(prev)), "dropped": sorted(set(prev) - set(cur)),
            "retyped": sorted(n for n in set(cur) & set(prev) if cur[n] != prev[n]),
            "redefined": {
                "link_primary": "dropped; issuer primary is is_issuer_primary (max prior-63-session dollar volume)",
                "cik / link_tier": "links now come from identity/link_table.parquet and are used only once known "
                                   "(strict and name tiers: available_at <= session 22:00 UTC; backfill: evidence_at)",
                "fundamental items": "every item column of fundamentals/events.parquet is carried (fund lane FUND2 "
                                     "fallback chains; see docs/ALPHA_PANEL_FUNDAMENTALS.md)"}}


def run() -> dict[str, Any]:
    root = C.build_root()
    glob = (root / "panel" / "*" / "*.parquet").as_posix()

    def scratch():
        return C.connect(memory="560MB", threads=1, db_file="metrics.duckdb")
    con = scratch()
    schema = _schema(con, glob)
    names = {n for n, _ in schema}
    con.close()
    structural = _structural(root)
    cov: list[dict[str, Any]] = []
    dist: list[dict[str, Any]] = []
    for w in _windows():
        # one connection per window: the native heap of a long-lived DuckDB process keeps growing across windows and
        # crossed the 1 GiB guard cap at the sixth (2023) window (2026-09-30); a fresh process heap per window does not
        con = scratch()
        c, d = measure(con, glob, schema, structural, [w])
        cov += c
        dist += d
        con.close()
    con = scratch()
    out = C.stage_dir("metrics")
    import pyarrow as pa
    import pyarrow.parquet as pq

    for name, rows in (("coverage", cov), ("distribution", dist)):
        tbl = pa.Table.from_pylist(rows)
        tmp = out / f"{name}.parquet.partial"
        pq.write_table(tbl, tmp, compression="zstd")
        tmp.replace(out / f"{name}.parquet")
    vin = vintage(con, glob, names)
    C.write_json_atomic(out / "vintage.json", vin)
    log = changelog(root, schema)
    C.write_json_atomic(out / "changelog.json", log)
    audit_path = root / "identity" / "audit.json"
    audit = C.read_json(audit_path) if audit_path.exists() else {}
    audit["panel_identity"] = identity_from_panel(con, glob)
    C.write_json_atomic(out / "identity.json", audit["panel_identity"])
    C.write_stage_manifest("metrics", "atx.alpha-panel.metrics/v1", ("metrics", "common"), {
        "rule_text": __doc__, "panel_files": len(list((root / "panel").glob("*/*.parquet"))),
        "panel_manifest": C.output_hashes(root / "panel", "manifest.json")}, pattern="*")
    write_doc(cov, dist, vin, log, audit)
    return {"coverage_rows": len(cov), "distribution_rows": len(dist)}


def write_doc(cov, dist, vin, log, audit) -> None:
    years = [str(y) for y in range(2018, 2027)] + ["train_2020_2022"]
    by = {(r["field"], r["window"]): r for r in cov}
    fields = sorted({r["field"] for r in cov})
    lines = ["# Alpha panel metrics (request section 5)", "",
             f"Generated {dt.datetime.now(dt.UTC):%Y-%m-%d %H:%M} UTC by `atx_db.alpha_panel.metrics`. Full tables: "
             "`data/alpha_panel/v1/metrics/{coverage,distribution}.parquet`, `vintage.json`, `changelog.json`. "
             "No statistic here conditions on returns.", "",
             "## 1. Coverage over member_equity cells (finite share, %)", "",
             "Bases in the parquet: `member_equity`, `linked` (any link tier), `linked_strict`, `linked_pit` "
             "(strict + name), `linked_usd` (linked, visible filing in USD: money items of other reporting "
             "currencies are NaN by rule, no point-in-time FX source), `excl_structural` (fundamental items with a "
             "structural-absence rule). Return fields carry no distribution from 2023 on (request section 6).", "",
             "| field | " + " | ".join(years) + " |", "|---|" + "---:|" * len(years)]
    for f in fields:
        cells = []
        for y in years:
            r = by.get((f, y))
            v = r["member_equity"] if r else None
            cells.append("" if v is None else f"{100 * v:.1f}")
        lines.append(f"| `{f}` | " + " | ".join(cells) + " |")
    lines += ["", "## 1b. Issuer fields on linked member_equity cells, TRAIN 2020-2022 (%)", "",
              "| field | any tier | strict | strict+name | linked USD reporter | excl. structural |",
              "|---|---:|---:|---:|---:|---:|"]
    for f in fields:
        r = by.get((f, "train_2020_2022"))
        if not r or r["linked"] is None:
            continue
        fmt = [("" if r[k] is None else f"{100 * r[k]:.1f}") for k in ("linked", "linked_strict", "linked_pit",
                                                                        "linked_usd", "excl_structural")]
        lines.append(f"| `{f}` | " + " | ".join(fmt) + " |")
    lines += ["", "## 2. Distributions over finite member_equity values, TRAIN 2020-2022", "",
              "| field | n | p0.1 | p1 | p50 | p99 | p99.9 | zero | negative |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]

    def g(x):
        return "" if x is None else f"{x:.4g}"
    for r in dist:
        if r["window"] != "train_2020_2022":
            continue
        lines.append(f"| `{r['field']}` | {r['n']} | {g(r['p0.1'])} | {g(r['p1'])} | {g(r['p50'])} | {g(r['p99'])} | "
                     f"{g(r['p99.9'])} | {g(r['share_zero'])} | {g(r['share_negative'])} |")
    lines += ["", "## 3. Vintage audit (share of member_equity cells with a value)", "",
              "| source | " + " | ".join(years[:-1]) + " |", "|---|" + "---:|" * (len(years) - 1)]
    for k, per in vin.items():
        lines.append(f"| {k} | " + " | ".join("" if per.get(y, {}).get("share") is None else f"{per[y]['share']:.3f}"
                                              for y in years[:-1]) + " |")
    lines += ["", "## 4. Identity audit (member_equity cells of the panel; a link counts once it is known)", "",
              "| year | cells | strict | strict+name | any tier | dead-line cells linked |", "|---|---:|---:|---:|---:|---:|"]
    for y, s in sorted(audit.get("panel_identity", {}).items()):
        lines.append(f"| {y} | {s['cells']} | {s['share_strict']} | {s['share_strict_or_name']} | {s['share_any']} | "
                     f"{s.get('dead_share_any')} |")
    lines += ["", f"Ambiguous line-days by year: {audit.get('ambiguous_line_days_by_year')}. Multi-line issuers "
              f"(>= 21 sessions with 2+ linked lines): {audit.get('multi_class_issuers')} "
              "(`identity/multi_class_issuers.parquet`).", "",
              "## 5. Field change log vs the previous panel build", "",
              f"Added ({len(log['added'])}): " + ", ".join(f"`{x}`" for x in log["added"]), "",
              f"Dropped ({len(log['dropped'])}): " + ", ".join(f"`{x}`" for x in log["dropped"]), "",
              f"Retyped: {log['retyped']}", "", "Redefined:"] + [f"* `{k}`: {v}" for k, v in log["redefined"].items()]
    (C.PACKAGE_ROOT / "docs" / "ALPHA_PANEL_METRICS.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    print(run(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
