"""Stage ``options``: option-implied fields per (session_date, security_id) -> ``options/year=YYYY/options.parquet``.

Tier1-v3 S8.2. The lake holds no per-security option-surface history beyond the vendor ATM term structure: atx-vol
was removed on 2026-09-19 (commit e4bdcf54) and kept no surface history (``C:/atx-data`` is empty), and the vendor
file (SpiderRock TickerHistory3) carries earnings-censored ATM IV only. So the stage has two parts:

* **free, built now** (``atm_source = 'tickerhistory3'``): ``iv_atm_{21,63,126,252}d`` from the prices stage (vendor
  ``atmCenI_*`` = ATM IV with the implied earnings move removed, NULL outside [0.02, 5.0]; tenors in trading days,
  21 d = 30 calendar days), ``term_slope_63_21`` and ``term_slope_252_21`` (= the panel characteristic
  ``iv_term_slope``, Vasquez 2017);
* **licensed, NULL until D3** (``skew_source``): ``iv_call_25d_30d``, ``iv_put_25d_30d``, ``skew_25d_30d``,
  ``call_volume``, ``put_volume``, ``call_oi``, ``put_oi`` from the ``licensed.options`` adapter stage
  (``--licensed <dir>`` holding its ``options_daily.parquet``), joined on (security_id, session_date).

Clock rule ``options-clock-v1``: ``atm_available_at`` = session_date 22:00 America/Chicago in UTC (03:00 / 04:00 UTC
next day), the vendor's documented US delivery of its end-of-day surface products (T+0; SurfaceFixedTermHist carries
the same ``atmCenI_*`` fields). With the panel cutoff (visible at d iff available_at < 22:00 UTC of d-1) a session's
values are usable from the second following session. ``lic_available_at`` is the adapter's clock (same rule);
``available_at`` = the later of the two when licensed values are present. Rows: every prices row with an ATM IV,
plus licensed rows. No return is read (D6 not touched).

Coverage (manifest ``coverage``), per year over the panel's ``member_equity`` cells: ``optionable`` = member cells
with a vendor ATM IV that session (the vendor publishes IV only for underliers with listed options); term-slope,
skew, volume and OI shares are over optionable cells, and over optionable member lines (distinct security_id).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C

STAGE = "options"
SCHEMA = "atx.alpha-panel.options/v1"
RULE = "options-clock-v1"
MODULES = ("options", "common")
DELIVERY_ZONE = "America/Chicago"
DELIVERY_TIME = "22:00:00"
LICENSED_COLUMNS = ("iv_call_25d_30d", "iv_put_25d_30d", "skew_25d_30d", "call_volume", "put_volume", "call_oi",
                    "put_oi")


def _years(root: Path) -> list[int]:
    return sorted(int(p.name.split("=")[1]) for p in (root / "prices").glob("year=*") if p.is_dir())


def build(licensed: Path | None = None, years: list[int] | None = None) -> dict[str, Any]:
    t0 = time.perf_counter()
    root = C.build_root()
    con = C.connect(memory="250MB", threads=2)
    out = C.stage_dir(STAGE)
    inputs: dict[str, str] = {}
    for m in ("prices/manifest.json", "panel/manifest.json"):
        if (root / m).exists():
            inputs[m] = C.sha256_file(root / m)
    lic_file = (licensed / "options_daily.parquet") if licensed else None
    if lic_file is not None:
        if not lic_file.exists():
            raise FileNotFoundError(lic_file)
        con.execute(f"""CREATE TEMP TABLE lic AS
            SELECT security_id, session_date, {', '.join(LICENSED_COLUMNS)}, available_at AS lic_available_at,
                   'licensed:' || coalesce(history_mode, '?') AS skew_source
            FROM read_parquet('{lic_file.as_posix()}') WHERE security_id IS NOT NULL""")
        if (licensed / "manifest.json").exists():
            inputs["licensed_options/manifest.json"] = C.sha256_file(licensed / "manifest.json")
    else:
        cols = ", ".join(f"CAST(NULL AS DOUBLE) AS {c}" for c in LICENSED_COLUMNS)
        con.execute(f"""CREATE TEMP TABLE lic AS SELECT CAST(NULL AS BIGINT) AS security_id,
            CAST(NULL AS DATE) AS session_date, {cols}, CAST(NULL AS TIMESTAMP) AS lic_available_at,
            CAST(NULL AS VARCHAR) AS skew_source LIMIT 0""")
    per_year: dict[str, Any] = {}
    for y in years or _years(root):
        prices = (root / "prices" / f"year={y}" / "prices.parquet").as_posix()
        dest = out / f"year={y}" / "options.parquet"
        sql = f"""
            WITH p AS (
                SELECT session_date, security_id, ticker, iv_atm_21d, iv_atm_63d, iv_atm_126d, iv_atm_252d
                FROM read_parquet('{prices}')
                WHERE iv_atm_21d IS NOT NULL OR iv_atm_63d IS NOT NULL OR iv_atm_126d IS NOT NULL
                   OR iv_atm_252d IS NOT NULL),
            l AS (SELECT * FROM lic WHERE year(session_date) = {y})
            SELECT coalesce(p.session_date, l.session_date) AS session_date,
                   coalesce(p.security_id, l.security_id) AS security_id, p.ticker,
                   p.iv_atm_21d, p.iv_atm_63d, p.iv_atm_126d, p.iv_atm_252d,
                   p.iv_atm_63d - p.iv_atm_21d AS term_slope_63_21, p.iv_atm_252d - p.iv_atm_21d AS term_slope_252_21,
                   {', '.join('l.' + c for c in LICENSED_COLUMNS)},
                   CASE WHEN p.security_id IS NOT NULL THEN 'tickerhistory3' END AS atm_source,
                   coalesce(l.skew_source, 'none') AS skew_source,
                   CAST(timezone('{DELIVERY_ZONE}', coalesce(p.session_date, l.session_date) + TIME '{DELIVERY_TIME}')
                        AS TIMESTAMP) AS atm_available_at,
                   l.lic_available_at,
                   greatest(CAST(timezone('{DELIVERY_ZONE}', coalesce(p.session_date, l.session_date)
                                 + TIME '{DELIVERY_TIME}') AS TIMESTAMP), l.lic_available_at) AS available_at
            FROM p FULL OUTER JOIN l ON l.security_id = p.security_id AND l.session_date = p.session_date
            ORDER BY 1, 2"""
        n = C.copy_to_parquet(con, sql, dest, row_group_size=32768)
        per_year[str(y)] = {"rows": n, **coverage(con, root, y, dest)}
        print(y, per_year[str(y)], flush=True)
    lic_code = Path(__file__).resolve().parents[1] / "licensed" / "options.py"
    payload = {
        "rule": RULE,
        "rule_text": ("atm_available_at = session_date 22:00 America/Chicago (vendor US EOD delivery, T+0); "
                      "available_at = greatest(atm_available_at, lic_available_at); visible at session d iff "
                      "available_at < 22:00 UTC of d-1"),
        "atm_source": "prices stage iv_atm_* = TickerHistory3 atmCenI_* (earnings-censored ATM IV), [0.02, 5.0]",
        "licensed_source": str(lic_file) if lic_file else None,
        "license_need": "docs/LICENSED_ADAPTERS.md section Options (SpiderRock OptionEODFeaturesHist + SurfaceFixedGridHist)",
        "input_manifests_sha256": inputs,
        "licensed_adapter_sha256": hashlib.sha256(lic_code.read_bytes()).hexdigest() if lic_code.exists() else None,
        "coverage": per_year,
        "seconds": round(time.perf_counter() - t0, 1),
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return payload


def coverage(con, root: Path, y: int, dest: Path) -> dict[str, Any]:
    """Member-cell and member-line coverage of the stage for year ``y`` (see module docstring)."""
    panel = (root / "panel" / f"year={y}" / "*.parquet").as_posix()
    if not list((root / "panel" / f"year={y}").glob("*.parquet")):
        return {"member_cells": None}
    lic = " OR ".join(f"o.{c} IS NOT NULL" for c in ("skew_25d_30d", "call_volume", "call_oi"))
    r = con.execute(f"""
        WITH m AS (SELECT session_date, security_id FROM read_parquet('{panel}', hive_partitioning = false)
                   WHERE member_equity),
        j AS (SELECT m.security_id, o.iv_atm_21d IS NOT NULL AS opt, o.term_slope_252_21 IS NOT NULL AS slope,
                     o.skew_25d_30d IS NOT NULL AS skew, o.call_volume IS NOT NULL AND o.put_volume IS NOT NULL AS vol,
                     o.call_oi IS NOT NULL AND o.put_oi IS NOT NULL AS oi, ({lic}) AS anylic
              FROM m LEFT JOIN read_parquet('{dest.as_posix()}') o USING (session_date, security_id)),
        lines AS (SELECT security_id, bool_or(opt) AS opt, bool_or(opt AND slope) AS slope, bool_or(opt AND skew) AS skew,
                         bool_or(opt AND vol) AS vol, bool_or(opt AND oi) AS oi FROM j GROUP BY 1)
        SELECT (SELECT count(*) FROM j), (SELECT count(*) FILTER (WHERE opt) FROM j),
               (SELECT count(*) FILTER (WHERE opt AND slope) FROM j), (SELECT count(*) FILTER (WHERE opt AND skew) FROM j),
               (SELECT count(*) FILTER (WHERE opt AND vol) FROM j), (SELECT count(*) FILTER (WHERE opt AND oi) FROM j),
               (SELECT count(*) FILTER (WHERE anylic AND NOT opt) FROM j),
               (SELECT count(*) FROM lines), (SELECT count(*) FILTER (WHERE opt) FROM lines),
               (SELECT count(*) FILTER (WHERE slope) FROM lines), (SELECT count(*) FILTER (WHERE skew) FROM lines),
               (SELECT count(*) FILTER (WHERE vol) FROM lines), (SELECT count(*) FILTER (WHERE oi) FROM lines)
    """).fetchone()
    cells, opt, slope, skew, vol, oi, lic_only, lines, lopt, lslope, lskew, lvol, loi = (int(v) for v in r)

    def share(a: int, b: int) -> float | None:
        return round(a / b, 4) if b else None
    return {"member_cells": cells, "optionable_cells": opt, "optionable_share_of_member": share(opt, cells),
            "term_slope_share": share(slope, opt), "skew_share": share(skew, opt), "volume_share": share(vol, opt),
            "oi_share": share(oi, opt), "licensed_cells_without_vendor_iv": lic_only,
            "member_lines": lines, "optionable_lines": lopt, "term_slope_line_share": share(lslope, lopt),
            "skew_line_share": share(lskew, lopt), "volume_line_share": share(lvol, lopt),
            "oi_line_share": share(loi, lopt)}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--licensed", type=Path, default=None,
                    help="directory with the licensed.options adapter's options_daily.parquet (S8.3)")
    ap.add_argument("--years", default=None, help="e.g. 2018-2026")
    args = ap.parse_args(argv)
    years = None
    if args.years:
        a, _, b = args.years.partition("-")
        years = list(range(int(a), int(b or a) + 1))
    payload = build(args.licensed, years)
    print(json.dumps({k: payload[k] for k in ("rule", "coverage", "seconds")}, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
