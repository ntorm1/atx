"""Consumer export of stage F in the ``atx.fundamental-events/v1`` contract (mega-alpha field loader).

Reads the published ``fundamentals/`` stage (``events.parquet``, ``sic_events.parquet``, ``manifest.json``) and
writes ``<build root>/export/fundamental-events-v1/``:

* ``fundamental_events.parquet``: ``cik, accession, accepted_utc`` (timestamp[us, tz=UTC]), ``clock_basis``
  (``fsds_accepted_utc`` or ``cf_fc1``), ``filed, form, period_end, fiscal_year, fiscal_period,
  staleness_days`` (int32, 200 or 400), every item column (float64, NaN when not derivable, never null),
  ``zero_filled`` and the v2 descriptors (``currency, fin_template, sale_src, gp_src, oi_src, shrs_src,
  xrd_reported_zero``); sorted ``(cik, accepted_utc, accession)``.
* ``sic_events.parquet``: ``cik, accession, accepted_utc, clock_basis, filed, form, sic`` (+ ``sic_basis, sic2,
  ff12, ff49``); sorted ``(cik, accepted_utc, accession)``.
* ``manifest.json`` (written last): ``schema``, ``status = complete``, ``files {name: {bytes, sha256, rows}}``,
  ``items``, ``item_units``, ``values_label``, ``rehearsal_identity``, code SHAs, the stage manifest SHA,
  parameters, counts and caveats.

``verify`` loads the export with the consumer's own ``pinned_manifest`` / ``read_listed`` / ``load_events``
imported read-only from the pool-2 worktree file (never modified).

Usage (guarded)::

    python -m atx_db.alpha_panel.fund_export build
    python -m atx_db.alpha_panel.fund_export verify [--consumer C:/atx-wt/pool-2/atx-engine/tools/prepare_research_fields.py]
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import sys
import time
from pathlib import Path
from typing import Any

from . import common
from . import fund_items as fi
from . import fundamentals as fu

SCHEMA = "atx.fundamental-events/v1"
EXPORT_NAME = "fundamental-events-v1"
CONSUMER = Path(r"C:\atx-wt\pool-2\atx-engine\tools\prepare_research_fields.py")
DESCRIPTORS = ("currency", "fin_template", "sale_src", "gp_src", "oi_src", "shrs_src", "xrd_reported_zero")
UNITS = {c: "USD" for c in fi.MONEY_ITEMS}
UNITS.update({"shrs_q": "shares", "shrs_q_lag4": "shares", "sue": "unitless", "fscore": "count 0-9",
              "fscore_n": "count 0-9", "fscore_partial": "count 0-9"})
UNITS.update({t: "0/1" for t in fi.FSCORE_TERMS})
CAVEATS = [
    "values modeled/unaccepted, derived from the SEC Company Facts archive snapshot 2026-09-20 (us-gaap, ifrs-full, dei) "
    "and FSDS v2 (SUB acceptance clocks and SIC, NUM class-of-stock share sums, PRE statement lines)",
    "CIK scope: every Company Facts CIK with periodic-form facts (not only rehearsal-bridge CIKs); rehearsal_identity=false",
    "money columns are USD facts only (no point-in-time FX source): filings reporting in another currency have NaN money "
    "items; their currency-invariant items (sue, f_* terms, fscore, fscore_n, fscore_partial) are computed in the "
    "reporting currency; `currency` names it",
    "no producer seal: rows run to the 2026-09-20 snapshot; the consumer applies its own seal (accepted_utc >= 2025-01-01 "
    "dropped by prepare_research_fields.load_events)",
    "FC1 fallback clock (filed + 46 h, clock_basis cf_fc1) where FSDS SUB lacks the accession (all filings after 2026q2)",
    "zero fills are listed per row in zero_filled (dvc/prstkc/sstk as v1; v2: xrd_ttm, capx_ttm, txt_q, txt_q_lag4, "
    "sale_ttm, xint, dvt, gdwl, intan, mib, pstk); xrd_reported_zero flags the R&D zero",
    "structural NaN by fin_template (SIC in force): see parameters.structural_na; coverage excluding structural NaN is in "
    "the stage manifest coverage_by_year",
    "item definitions are the atx-db stage F rules (parameters.item_rules), which differ in detail from the atx-engine "
    "producer's schema doc: shrs_q is the dei cover count first (then balance-sheet, weighted-average, class-sum shares; "
    "shrs_q_lag4 uses the same source, split-adjusted), noa = (at - che) - (at - debt - mib - pstk - common equity), "
    "period_end is the latest main-statement period end (no FSDS period snap)",
]


def export_dir() -> Path:
    path = common.build_root() / "export" / EXPORT_NAME
    path.mkdir(parents=True, exist_ok=True)
    return path


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    stage = fu.fund_dir()
    stage_manifest = stage / "manifest.json"
    sm = common.read_json(stage_manifest)
    if sm.get("status") != "complete" or sm.get("rule") != fu.RULE:
        raise SystemExit("fundamentals stage manifest is not a complete fund-events-pit-v2 publication")
    out = export_dir()
    man_path = out / "manifest.json"
    if man_path.exists():
        man_path.unlink()  # publish last
    ev, sic = (stage / "events.parquet").as_posix(), (stage / "sic_events.parquet").as_posix()
    items = list(fi.ALL_ITEMS)
    item_sql = ", ".join(f'CAST(coalesce("{c}", \'NaN\'::DOUBLE) AS DOUBLE) AS "{c}"' for c in items)
    con = common.connect(memory="450MB", threads=2)
    try:
        n_ev = common.copy_to_parquet(
            con,
            f"""
            SELECT cik, accession, CAST(clock_utc AS TIMESTAMPTZ) AS accepted_utc,
                   CASE WHEN clock_basis = 'fsds_accepted_utc' THEN 'fsds_accepted_utc' ELSE 'cf_fc1' END AS clock_basis,
                   filed, form, period_end, fiscal_year, fiscal_period, CAST(staleness_days AS INTEGER) AS staleness_days,
                   {item_sql}, zero_filled, {", ".join(DESCRIPTORS)}
            FROM read_parquet('{ev}') ORDER BY cik, accepted_utc, accession
            """,
            out / "fundamental_events.parquet")
        n_sic = common.copy_to_parquet(
            con,
            f"""
            SELECT s.cik, s.accession, CAST(s.clock_utc AS TIMESTAMPTZ) AS accepted_utc,
                   CASE WHEN e.clock_basis = 'fsds_accepted_utc' THEN 'fsds_accepted_utc' ELSE 'cf_fc1' END AS clock_basis,
                   e.filed, e.form, CAST(s.sic AS INTEGER) AS sic, s.sic_basis, s.sic2, s.ff12, s.ff49
            FROM read_parquet('{sic}') s JOIN read_parquet('{ev}') e USING (cik, accession)
            ORDER BY s.cik, accepted_utc, s.accession
            """,
            out / "sic_events.parquet")
        f = (out / "fundamental_events.parquet").as_posix()
        counts: dict[str, Any] = {}
        counts["rows"], counts["ciks"], counts["fc1_rows"] = con.execute(
            f"SELECT count(*), count(DISTINCT cik), sum((clock_basis = 'cf_fc1')::INT) FROM read_parquet('{f}')"
        ).fetchone()
        counts["rows_by_year"] = {str(y): n for y, n in con.execute(
            f"SELECT year(accepted_utc), count(*) FROM read_parquet('{f}') GROUP BY 1 ORDER BY 1").fetchall()}
        counts["staleness_days"] = {str(k): v for k, v in con.execute(
            f"SELECT staleness_days, count(*) FROM read_parquet('{f}') GROUP BY 1 ORDER BY 1").fetchall()}
        fin = ", ".join(f'round(avg(isfinite("{c}")::INT), 4)' for c in items)
        vals = con.execute(f"SELECT {fin} FROM read_parquet('{f}') WHERE accepted_utc >= TIMESTAMPTZ '2020-01-01'"
                           ).fetchone()
        counts["finite_fraction_2020_plus"] = dict(zip(items, vals))
        nulls = con.execute(f"SELECT {', '.join(f'sum((\"{c}\" IS NULL)::INT)' for c in items)} "
                            f"FROM read_parquet('{f}')").fetchone()
        if any(nulls):
            raise AssertionError("an item column carries nulls")
    finally:
        con.close()
    files = {}
    for name, rows in (("fundamental_events.parquet", n_ev), ("sic_events.parquet", n_sic)):
        p = out / name
        files[name] = {"bytes": p.stat().st_size, "sha256": common.sha256_file(p), "rows": rows}
    manifest = {
        "schema": SCHEMA, "status": "complete", "values_label": "modeled_unaccepted", "rehearsal_identity": False,
        "producer": "atx_db.alpha_panel.fund_export (stage F rule " + fu.RULE + ")",
        "seal": None, "emit_from": fu.EMIT_FROM.date().isoformat(),
        "files": files, "items": items, "item_units": {c: UNITS[c] for c in items},
        "descriptor_columns": ["zero_filled", *DESCRIPTORS],
        "code_sha256": common.code_identity("fund_export", *fu.MODULES),
        "inputs": {"fundamentals_manifest": {"path": str(stage_manifest), "bytes": stage_manifest.stat().st_size,
                                             "sha256": common.sha256_file(stage_manifest)},
                   "fundamentals_files": sm.get("files")},
        "parameters": {"staleness_rule": fu.STALENESS_RULE, "currency_rule": fu.CURRENCY_RULE,
                       "template_rule": fi.TEMPLATE_RULE, "structural_na": fu.structural_na(),
                       "item_rules": fu.ITEM_RULES, "rule_text": fu.RULE_TEXT,
                       "clock": "accepted_utc = FSDS SUB accepted_utc of the accession, else filed 00:00 UTC + 46 h "
                                "(clock_basis cf_fc1); a row is visible at session d iff accepted_utc < d 22:00 UTC"},
        "counts": counts, "caveats": CAVEATS,
        "created_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
        "seconds": round(time.perf_counter() - t0, 2),
    }
    common.write_json_atomic(man_path, manifest)
    print({"rows": n_ev, "sic_rows": n_sic, "manifest_sha256": common.sha256_file(man_path)})
    return manifest


def _load_consumer(path: Path):
    spec = importlib.util.spec_from_file_location("_consumer_prepare_research_fields", path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def verify(consumer: Path = CONSUMER) -> dict[str, Any]:
    """Load the export exactly as the consumer does (its own functions, imported read-only by path)."""
    import numpy as np
    import pyarrow.parquet as pq

    before = hashlib.sha256(consumer.read_bytes()).hexdigest()
    mod = _load_consumer(consumer)
    out = export_dir()
    sha = common.sha256_file(out / "manifest.json")
    budget = mod.Budget(max_rss_mib=700, max_seconds=1800.0)
    res: dict[str, Any] = {"consumer": str(consumer), "consumer_sha256": before, "manifest_sha256": sha}
    m, man_src = mod.pinned_manifest(out, sha, "fund-events", mod.EVENTS_ADAPTER["schema"])
    res["pinned_manifest"] = man_src
    ciks = np.unique(pq.read_table(out / "fundamental_events.parquet", columns=["cik"]).column("cik").to_numpy())
    consumer_items = [name for name, _u, _w in mod.FUND_ITEMS]
    missing = [c for c in consumer_items if c not in m["items"]]
    res["consumer_items_missing"] = missing
    ev, _src, st = mod.load_events(out, m, mod.EVENTS_ADAPTER, consumer_items, ciks, "fund-events", budget)
    res["load_events_consumer_items"] = st
    res["finite_share_consumer_items_rows_used"] = {
        c: round(float(np.isfinite(ev["values"][c]).mean()), 4) for c in consumer_items}
    del ev
    new_items = [c for c in fi.ITEM_COLUMNS_V2]
    ev, _src, st = mod.load_events(out, m, mod.EVENTS_ADAPTER, new_items, ciks, "fund-events", budget)
    res["load_events_v2_items"] = {"rows_used": st["rows_used"], "items": len(new_items)}
    del ev
    sic, _src, st = mod.load_events(out, m, mod.SIC_ADAPTER, [], ciks, "sic-events", budget)
    res["load_events_sic"] = st
    res["budget_peak_rss_mib"] = budget.peak >> 20
    after = hashlib.sha256(consumer.read_bytes()).hexdigest()
    res["consumer_unchanged"] = before == after
    common.write_json_atomic(out.parent / f"{EXPORT_NAME}.verify.json", res)
    print(json.dumps({k: v for k, v in res.items() if k != "finite_share_consumer_items_rows_used"}, default=str,
                     indent=1)[:4000])
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build")
    v = sub.add_parser("verify")
    v.add_argument("--consumer", type=Path, default=CONSUMER)
    args = ap.parse_args(argv)
    if args.cmd == "build":
        build()
    else:
        verify(args.consumer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
