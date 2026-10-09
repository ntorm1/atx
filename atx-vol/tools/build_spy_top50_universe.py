#!/usr/bin/env python3
"""Build the fixed SPY top-50 dispersion universe from an SEC N-PORT XML filing.

Source of truth (authoritative, auditable, deterministic)
---------------------------------------------------------
The universe is the top-50 S&P 500 constituents *by index weight* as of the
nearest documented year-end as-of to 2026-01-01, taken directly from the SPDR
S&P 500 ETF Trust (SPY) N-PORT-P holdings filing on SEC EDGAR:

  Registrant : SPDR S&P 500 ETF TRUST
  Filing     : N-PORT-P, accession 0001410368-26-020131
  As-of      : 2025-12-31 (holdings report period end == index weights at year end)
  Retrieved  : SEC EDGAR (public, primary source). Local copy checked into the
               data tree at data/spy-dispersion/universe-source/spy-nport-2025-12-31.xml
               (the raw filing; its sha256 is echoed on every run for audit).

SPY holds the S&P 500 in float-adjusted market-cap proportion, so its N-PORT
equity holdings ranked by `pctVal` ARE the S&P 500 constituents ranked by index
weight — a primary-source, reproducible substitute for a paid index-membership
feed. The exact CUSIP->ticker resolution is a small committed map
(examples/spy_top50_symbol_map.tsv), so the run is fully deterministic.

With --index-symbol SPY the ETF itself is prepended as the dispersion *index
leg* (raw_weight 100 == the whole index; source tagged INDEX_ETF_SPDR_SPY), so
the emitted fixture is "SPY + top-50 single names" — the D1 dispersion universe.
Without the flag the output is exactly the 50 constituents (the TDD contract).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import pathlib
import xml.etree.ElementTree as ET

NS = {"n": "http://www.sec.gov/edgar/nport"}
EXPECTED_REGISTRANT = "SPDR S&P 500 ETF TRUST"
EXPECTED_REPORT_DATE = "2025-12-31"
SOURCE_ID = "SEC_NPORT_0001410368-26-020131"
# Provenance tag stamped on the prepended index-ETF leg (distinguishes the index
# leg from the constituent holdings, which carry SOURCE_ID).
INDEX_LEG_SOURCE = "INDEX_ETF_SPDR_SPY"
# The index leg is the whole index; a raw_weight of 100 also sorts it first so a
# top-N-by-weight budget degrade (D2 §3 guardrail) never drops the index leg.
INDEX_LEG_WEIGHT = 100.0


def required_text(root: ET.Element, path: str) -> str:
    node = root.find(path, NS)
    if node is None or node.text is None or not node.text.strip():
        raise ValueError(f"missing required XML field: {path}")
    return node.text.strip()


def load_map(path: pathlib.Path) -> dict[tuple[str, str], str]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    if not rows or set(rows[0]) != {"cusip", "name", "ticker"}:
        raise ValueError("symbol map header must be: cusip, name, ticker")
    result: dict[tuple[str, str], str] = {}
    for row in rows:
        key = (row["cusip"].strip(), row["name"].strip())
        ticker = row["ticker"].strip()
        if not all(key) or not ticker or key in result:
            raise ValueError(f"invalid or duplicate symbol mapping: {key}")
        result[key] = ticker
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml", type=pathlib.Path, required=True)
    parser.add_argument("--symbol-map", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--effective-date", default="2026-01-02")
    parser.add_argument(
        "--index-symbol",
        default="",
        help="prepend this ETF as the dispersion index leg (e.g. SPY); "
        "off by default so the raw output is exactly the 50 constituents",
    )
    args = parser.parse_args()

    xml_bytes = args.xml.read_bytes()
    root = ET.fromstring(xml_bytes)
    registrant = required_text(root, ".//n:regName")
    report_date = required_text(root, ".//n:repPdDate")
    if registrant != EXPECTED_REGISTRANT or report_date != EXPECTED_REPORT_DATE:
        raise ValueError(
            f"unexpected filing identity: registrant={registrant!r} report_date={report_date!r}"
        )
    if args.effective_date <= report_date:
        raise ValueError("effective date must follow the filing report date")

    holdings: list[tuple[float, str, str]] = []
    for security in root.findall(".//n:invstOrSec", NS):
        if required_text(security, "n:assetCat") != "EC":
            continue
        name = required_text(security, "n:name")
        cusip = required_text(security, "n:cusip")
        weight = float(required_text(security, "n:pctVal"))
        if weight <= 0.0:
            raise ValueError(f"non-positive equity weight for {name}")
        holdings.append((weight, cusip, name))
    holdings.sort(key=lambda row: (-row[0], row[1], row[2]))
    selected = holdings[:50]
    if len(selected) != 50:
        raise ValueError(f"expected at least 50 equity holdings, found {len(holdings)}")

    symbol_map = load_map(args.symbol_map)
    missing = [(cusip, name) for _, cusip, name in selected if (cusip, name) not in symbol_map]
    if missing:
        raise ValueError(f"top-50 holdings missing exact symbol mappings: {missing}")
    tickers = [symbol_map[(cusip, name)] for _, cusip, name in selected]
    if len(set(tickers)) != 50:
        raise ValueError("top-50 symbol mappings are not unique")

    index_symbol = args.index_symbol.strip().upper()
    if index_symbol and index_symbol in set(tickers):
        raise ValueError(
            f"--index-symbol {index_symbol} collides with a top-50 constituent ticker"
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="ascii") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["effective_date", "symbol", "raw_weight", "source", "as_of"])
        if index_symbol:
            writer.writerow(
                [args.effective_date, index_symbol, f"{INDEX_LEG_WEIGHT:.12f}",
                 INDEX_LEG_SOURCE, report_date]
            )
        for weight, cusip, name in selected:
            writer.writerow(
                [args.effective_date, symbol_map[(cusip, name)], f"{weight:.12f}", SOURCE_ID, report_date]
            )

    n_written = len(selected) + (1 if index_symbol else 0)
    print(
        f"wrote {n_written} rows ({len(selected)} constituents"
        f"{f' + index leg {index_symbol}' if index_symbol else ''}) to {args.out}; "
        f"source_sha256={hashlib.sha256(xml_bytes).hexdigest()} "
        f"selected_weight={sum(row[0] for row in selected):.12f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
