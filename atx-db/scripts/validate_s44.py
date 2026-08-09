#!/usr/bin/env python
"""S44: rebuild the derived fundamental surfaces after the companyfacts re-fetch, then validate.

Order matters — the S10 ratio families (liquidity / leverage / margin / activity / health)
read canonical metrics from ``fundamental_xbrl_metric`` via the ratio engine's balx/flowx
pivots, so the metric extractor must run before the ratio materializer:

  1. FundamentalXbrlMetricDataset  -> rebuild fundamental_xbrl_metric (now also sourcing the
                                      broad companyfacts feed via the S44 candidate path)
  2. FundamentalRatiosDataset      -> rebuild fundamental_ratios
  3. coverage report               -> row/security/code counts + per-category security coverage
  4. refresh_warehouse_watermarks
  5. run_warehouse_quality_checks(record=False) -> summarize non-passed checks

Pure derivation over already-loaded data — no network. Run after refetch_companyfacts_s44.py.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_xbrl_metrics import FundamentalXbrlMetricDataset, FundamentalXbrlMetricOptions
from atx_db.fundamental_ratios import FundamentalRatiosDataset, FundamentalRatiosOptions
from atx_db.watermarks import refresh_warehouse_watermarks
from atx_db.quality import run_warehouse_quality_checks


def _scalar(store, sql: str):
    return store.con.execute(sql).fetchone()[0]


def main() -> int:
    db_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DB_PATH
    out: dict = {"step": "validate_s44", "db_path": str(db_path)}
    with DuckDBStore(db_path) as store:
        # 1. rebuild consolidated XBRL canonical metrics (inline + companyfacts candidate path)
        m = FundamentalXbrlMetricDataset().run(store, FundamentalXbrlMetricOptions())
        out["xbrl_metric_rows_loaded"] = m.rows_loaded

        # 2. rebuild derived ratios
        r = FundamentalRatiosDataset().run(store, FundamentalRatiosOptions())
        out["ratio_rows_loaded"] = r.rows_loaded

        # 3. coverage report
        out["fundamental_xbrl_metric"] = {
            "rows": _scalar(store, "SELECT count(*) FROM fundamental_xbrl_metric"),
            "securities": _scalar(store, "SELECT count(DISTINCT security_id) FROM fundamental_xbrl_metric"),
            "metrics": _scalar(store, "SELECT count(DISTINCT canonical_metric) FROM fundamental_xbrl_metric"),
        }
        out["fundamental_ratios"] = {
            "rows": _scalar(store, "SELECT count(*) FROM fundamental_ratios"),
            "securities": _scalar(store, "SELECT count(DISTINCT security_id) FROM fundamental_ratios"),
            "ratio_codes": _scalar(store, "SELECT count(DISTINCT ratio_code) FROM fundamental_ratios"),
        }
        # per-category distinct-security coverage (the S10 families read fundamental_xbrl_metric)
        cat_rows = store.con.execute(
            "SELECT ratio_category, count(DISTINCT security_id) AS secs, count(*) AS rows "
            "FROM fundamental_ratios GROUP BY ratio_category ORDER BY ratio_category"
        ).fetchall()
        out["ratio_category_coverage"] = {c: {"securities": s, "rows": n} for c, s, n in cat_rows}

        # 4. watermarks
        wm = refresh_warehouse_watermarks(store)
        out["watermarks_refreshed"] = len(wm.watermarks)

        # 5. quality checks (non-recording)
        results = run_warehouse_quality_checks(store, record=False)
        non_passed = [
            {"check": r.check_name, "table": r.table_name, "status": r.status, "observed": r.observed_value}
            for r in results
            if r.status != "passed"
        ]
        out["quality_total_checks"] = len(results)
        out["quality_non_passed"] = non_passed

    print(json.dumps(out, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
