"""Parity scorecard (task S0.3): render ``docs/PARITY_SCORECARD.md`` from ``parityscore/catalog.csv`` and the measured
coverage published in the stage lake. Every number is read verbatim from a file and printed with its basis and
source; a row with no published measurement shows ``not measured``. Nothing is estimated.

Measurements, per catalog row:

* ``metrics/coverage.parquet`` (S0.1 section 5 metrics), for every ``column`` that is a panel field: the finite
  share over member_equity cells in ``--year`` and in the TRAIN window 2020-01-01..2022-12-31; fundamentals rows
  use the linked-USD basis (``linked_usd``) and, where the item has a structural rule, ``excl_structural``;
* ``fundamentals/manifest.json`` ``coverage_by_year``: ``finite_ex_structural`` of each fundamentals column over all
  filing events with a clock in ``--year``;
* domain rows: the stage manifests' own coverage figures (``HEADLINES``).

    python -m atx_db.parityscore.scorecard [--root R] [--out docs/PARITY_SCORECARD.md] [--year 2025]
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import sys
from collections import OrderedDict
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .catalog import CSV_PATH, PACKAGE_ROOT, load

OUT = PACKAGE_ROOT / "docs" / "PARITY_SCORECARD.md"
TRAIN = "train_2020_2022"
GATE = 0.90
# domain number -> [(file under the root, JSON path with {year}, label)]
HEADLINES: dict[int, list[tuple[str, str, str]]] = {
    1: [("identity/audit.json", "per_year.{year}.member_equity_share.strict_or_name",
         "member_equity cells linked by a PIT tier (strict + name)"),
        ("identity/audit.json", "per_year.{year}.member_equity_share.any_tier",
         "member_equity cells linked by any tier")],
    10: [("thirteenf/manifest.json", "receipt.coverage_by_year.{year}.share_with_visible_inst_shares",
          "member_equity cells with visible 13F institutional shares")],
    11: [("insider/manifest.json", "coverage_per_year.{year}.issuers_with_p_or_s",
          "issuers with an open-market Form 4 purchase or sale (count)")],
    12: [("ftd/manifest.json", "receipt.coverage_by_year.{year}.share_with_mapped_ftd_row_last_30d",
          "member_equity cells with a mapped FTD row in the prior 30 days"),
         ("short_volume_ext/manifest.json", "receipt.coverage_by_year.{year}.share_with_row",
          "member_equity cells with a short-volume row")],
    13: [("earnings_calendar/manifest.json", "member_equity_coverage.{year}.ann364_share_of_all",
          "member_equity cells with a visible primary 8-K 2.02 within 364 days")],
}


@dataclass
class Measure:
    text: str
    value: float | None
    basis: str
    source: str


def _dig(obj: Any, path: str) -> Any:
    for part in path.split("."):
        if not isinstance(obj, dict) or part not in obj:
            return None
        obj = obj[part]
    return obj


def _fmt(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.4f}".rstrip("0").rstrip(".") if abs(v) < 10 else f"{v:,.0f}"
    if isinstance(v, int):
        return f"{v:,}"
    return str(v)


class Lake:
    """Read-only access to the measured files under the build root (each file read once)."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self._json: dict[str, Any] = {}
        self.read: OrderedDict[str, str] = OrderedDict()  # file -> sha256 (sources list)
        self.metrics: dict[tuple[str, str], dict[str, Any]] | None = None
        path = root / "metrics" / "coverage.parquet"
        if path.is_file():
            import pyarrow.parquet as pq

            self.metrics = {(str(r["window"]), str(r["field"])): r for r in pq.read_table(path).to_pylist()}
            self._note(path)

    def _note(self, path: Path) -> None:
        rel = path.relative_to(self.root).as_posix()
        if rel not in self.read:
            self.read[rel] = hashlib.sha256(path.read_bytes()).hexdigest()

    def json(self, rel: str) -> Any:
        if rel not in self._json:
            p = self.root / rel
            self._json[rel] = json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None
            if p.is_file():
                self._note(p)
        return self._json[rel]


def measure_row(row: Mapping[str, str], lake: Lake, year: str) -> list[Measure]:
    out: list[Measure] = []
    cols = [c for c in row["column"].split("|") if c]
    fund = "fundamentals" in row["stage"]
    if lake.metrics is not None:
        for c in cols:
            for window in (year, TRAIN):
                rec = lake.metrics.get((window, c))
                if not rec:
                    continue
                key = "linked_usd" if fund else "member_equity"
                if fund and rec.get("excl_structural") is not None:
                    key = "excl_structural"
                v = rec.get(key)
                if v is not None:
                    out.append(Measure(f"{c} {_fmt(float(v))} ({window}, {key})", float(v),
                                       f"member_equity cells, {key}, {window}", "metrics/coverage.parquet"))
    if fund:
        cov = _dig(lake.json("fundamentals/manifest.json"), f"coverage_by_year.years.{year}.items")
        for c in cols:
            rec = cov.get(c) if isinstance(cov, dict) else None
            if isinstance(rec, dict) and rec.get("finite_ex_structural") is not None:
                v = float(rec["finite_ex_structural"])
                out.append(Measure(f"{c} {_fmt(v)} (events {year}, ex-structural)", v,
                                   f"all filing events with clock in {year}, ex-structural",
                                   "fundamentals/manifest.json coverage_by_year"))
    if row["item"] == "(domain)":
        n = int(row["domain"][:2])
        for rel, path, label in HEADLINES.get(n, []):
            v = _dig(lake.json(rel), path.format(year=year))
            if v is not None:
                out.append(Measure(f"{_fmt(v)} = {label} ({year})", float(v) if isinstance(v, int | float) else None,
                                   label, f"{rel} {path.format(year=year)}"))
        if n in (9, 18):
            ref = lake.json("reference/manifest.json")
            comp = _dig(ref, "receipt.completeness_2010")
            if n == 18 and isinstance(comp, dict) and comp:
                miss = [int(v["business_days_missing_2010"]) for v in comp.values()
                        if isinstance(v, dict) and v.get("business_days_missing_2010") is not None]
                if miss:
                    out.append(Measure(f"{len(miss)} FRED series; business days missing since 2010: "
                                       f"{min(miss)}..{max(miss)}", None, "FRED series completeness",
                                       "reference/manifest.json receipt.completeness_2010"))
            cur = _dig(ref, "receipt.fx_daily.currencies")
            if n == 9 and isinstance(cur, list):
                out.append(Measure(f"{len(cur)} H.10 currencies in reference/fx_daily "
                                   f"({_fmt(_dig(ref, 'receipt.fx_daily.rows'))} rows)",
                                   None, "H.10 currencies", "reference/manifest.json receipt.fx_daily"))
        if n == 6:
            cov = _dig(lake.json("fundamentals/manifest.json"), f"coverage_by_year.years.{year}.items")
            if isinstance(cov, dict) and cov:
                vals = [float(r["finite_ex_structural"]) for r in cov.values()
                        if isinstance(r, dict) and r.get("finite_ex_structural") is not None]
                k = sum(v >= GATE for v in vals)
                out.append(Measure(f"{k} of {len(vals)} event columns >= {GATE:.2f} ex-structural (events {year})",
                                   None, "fundamentals event columns", "fundamentals/manifest.json coverage_by_year"))
    return out


def _cell(text: str) -> str:
    return text.replace("|", "\\|")


def render(rows: list[dict[str, str]], lake: Lake, year: str, today: str) -> str:
    measured = [({k: _cell(v or "") for k, v in r.items()}, measure_row(r, lake, year)) for r in rows]
    by_domain: OrderedDict[str, list[tuple[dict[str, str], list[Measure]]]] = OrderedDict()
    for r, ms in sorted(measured, key=lambda x: x[0]["domain"]):
        by_domain.setdefault(r["domain"], []).append((r, ms))
    fund_items = [(r, ms) for r, ms in measured if r["domain"].startswith("06") and r["item"] != "(domain)"]
    lines = [
        "# Parity scorecard",
        "",
        f"Generated {today} by `python -m atx_db.parityscore.scorecard` from `src/atx_db/parityscore/catalog.csv` "
        f"({len(rows)} rows: plan section 1.1 domains, design-spec items, seed items, CRSP and panel fields) and the "
        f"stage lake `{lake.root.as_posix()}`. Measurement year {year}; TRAIN window 2020-01-01..2022-12-31. Every "
        "number is read from the file named in its row; `not measured` means no published measurement exists. "
        "Nothing is estimated.",
        "",
        "Sources read (SHA-256 prefix):",
        "",
    ]
    if lake.read:
        lines += [f"- `{k}` {v[:16]}" for k, v in lake.read.items()]
    else:
        lines.append("- none")
    if lake.metrics is None:
        lines.append("- `metrics/coverage.parquet`: absent (S0.1 section 5 metrics not published yet)")
    lines += ["", "## Summary", "", "| domain | rows | with a lake column | measured | sprints |", "|---|---|---|---|---|"]
    for dom, items in by_domain.items():
        sprints = sorted({s.strip() for r, _ in items for s in r["sprint"].split(",")})
        lines.append(f"| {dom} | {len(items)} | {sum(bool(r['column']) for r, _ in items)} | "
                     f"{sum(bool(ms) for _, ms in items)} | {', '.join(sprints)} |")
    lines.append(f"| **total** | {len(rows)} | {sum(bool(r['column']) for r in rows)} | "
                 f"{sum(bool(ms) for _, ms in measured)} | |")
    best = []
    for _r, ms in fund_items:
        vals = [m.value for m in ms if m.value is not None and "events" in m.text]
        if vals:
            best.append(max(vals))
    lines += ["", "## Fundamentals gate (design spec: >= 110 items at >= 0.90, FY2015-2025)", "",
              f"- catalog fundamentals items: {len(fund_items)}; carried by a lake column: "
              f"{sum(bool(r['column']) for r, _ in fund_items)}; with a measured value: "
              f"{sum(bool(ms) for _, ms in fund_items)}",
              f"- items whose best measured column is >= {GATE:.2f} on the all-filing-events basis ({year}): "
              f"{sum(v >= GATE for v in best)} (the gate's top-3000 market-cap basis is not measured yet)", ""]
    lines += ["## Domains (plan section 1.1)", "", "| domain | tier-1 reference | lake stage | target | measured | sprint |",
              "|---|---|---|---|---|---|"]
    for r, ms in measured:
        if r["item"] == "(domain)":
            m = "<br>".join(_cell(x.text) for x in ms) if ms else "not measured"
            lines.append(f"| {r['domain']} | {r['reference']} | {r['stage'] or '-'} | {r['target']} | {m} | "
                         f"{r['sprint']} |")
    lines += ["", "## Items", ""]
    for dom, items in by_domain.items():
        rest = [(r, ms) for r, ms in items if r["item"] != "(domain)"]
        if not rest:
            continue
        lines += [f"### {dom}", "", "| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |",
                  "|---|---|---|---|---|---|---|---|"]
        for r, ms in rest:
            sc = f"{r['stage']}.{r['column']}" if r["stage"] and r["column"] else (r["stage"] or "-")
            m = "<br>".join(_cell(x.text) for x in ms) if ms else "not measured"
            lines.append(f"| {r['item']} | {r['compustat_field'] or '-'} | {r['crsp_field'] or '-'} | "
                         f"{r['factset_field'] or '-'} | {sc} | {r['target']} | {m} | {r['sprint']} |")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main(argv: list[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(prog="python -m atx_db.parityscore.scorecard", description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=None)
    ap.add_argument("--catalog", type=Path, default=CSV_PATH)
    ap.add_argument("--out", type=Path, default=OUT)
    ap.add_argument("--year", default="2025")
    args = ap.parse_args(argv)
    if args.root is None:
        from ..alpha_panel.common import build_root

        args.root = build_root()
    rows = load(args.catalog)
    text = render(rows, Lake(args.root), args.year, dt.date.today().isoformat())
    args.out.write_text(text, encoding="utf-8", newline="\n")
    print(f"{args.out}: {len(rows)} rows, {text.count('not measured')} not measured")
    return 0


if __name__ == "__main__":
    sys.exit(main())
