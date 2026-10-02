"""Form 144 notices of proposed sale (lane OWN, S5.4) -> stage ``insider_ext``. See docs/ALPHA_PANEL_OWNERSHIP.md.

Form 144 has been filed electronically in EDGAR's structured XML since 2023-04-13 (before that almost all notices
were paper; EDGAR holds ~200 a year). The SEC publishes no Form 144 data set, so each notice's ``primary_doc.xml`` is
fetched once (:mod:`sec_docs`, FetchLedgerStore under ``data/raw/sec_144/``), newest first, within the lane's SEC
request budget (``fetch --max-new``); ``notices`` covers every 144 / 144/A accession in ``sec_filings`` whether its
document was landed or not.

Outputs (``insider_ext/``):
* ``form144_notices.parquet``: one row per accession 2019+: filing date, ``available_at``, issuer CIK (from the XML;
  for notices not landed, the one CIK of the accession that EDGAR lists with a SIC code, ``issuer_basis``), filer
  CIKs, ``doc_status``; parsed fields where landed: seller name, relationship (Officer / Director / 10% owner /
  Other), class, broker, units to be sold, aggregate market value, units outstanding, approximate sale date,
  exchange, nature of acquisition of the lots to be sold (vesting / option exercise / open market ...), whether
  sales in the past 3 months are reported, notice date, and the Rule 10b5-1 plan adoption date when given;
* ``form144_monthly.parquet``: per (issuer CIK, month of ``available_at``): notices, distinct sellers, and for the
  parsed notices the units and aggregate market value proposed for sale; ``available_at`` = the month's latest
  notice clock.

Clock: ``available_at`` = the accession's resolved EDGAR acceptance from ``sec_filings`` (earliest over its CIK rows);
amendments (144/A) are separate rows.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
import time
from typing import Any

from . import common as C
from . import sec_docs as D

STAGE = "insider_ext"
SCHEMA = "atx.alpha-panel.insider-ext/v1"
SOURCE = "sec_144"
FORMS = ("144", "144/A")
XML_FROM = dt.date(2023, 1, 1)
NOTICE_FROM = dt.date(2019, 1, 1)
MODULES = ("form144", "insider_measures", "sec_docs", "common")


def fetch_urls() -> list[str]:
    out = []
    for acc, cik, _f, _fd, pdoc, _av, _ciks in D.select_filings(FORMS, XML_FROM):
        if pdoc and pdoc.lower().startswith("xsl"):
            out.append(D.doc_url(cik, acc, D.structured_document(pdoc)))
    return out


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _first(el, name: str) -> str | None:
    for e in el.iter():
        if _local(e.tag) == name and e.text and e.text.strip():
            return e.text.strip()
    return None


def _num(s: str | None) -> float | None:
    if s is None:
        return None
    try:
        return float(re.sub(r"[,\s$]", "", s))
    except ValueError:
        return None


def _mdy(s: str | None) -> dt.date | None:
    if not s:
        return None
    for fmt in ("%m/%d/%Y", "%Y-%m-%d"):
        try:
            return dt.datetime.strptime(s.strip(), fmt).date()
        except ValueError:
            continue
    return None


def parse_144(body: bytes) -> dict[str, Any]:
    """Form 144 ``primary_doc.xml`` -> notice fields (several securities blocks: units and value summed, class and
    exchange of the first)."""
    import xml.etree.ElementTree as ET

    root = ET.fromstring(body)
    info = [e for e in root.iter() if _local(e.tag) == "securitiesInformation"]
    lots = [e for e in root.iter() if _local(e.tag) == "securitiesToBeSold"]
    sold3m = [e for e in root.iter() if _local(e.tag) == "securitiesSoldInPast3Months"]
    rels = sorted({e.text.strip() for e in root.iter() if _local(e.tag) == "relationshipToIssuer" and e.text})
    units = [_num(_first(e, "noOfUnitsSold")) for e in info]
    value = [_num(_first(e, "aggregateMarketValue")) for e in info]
    natures = sorted({(_first(e, "natureOfAcquisitionTransaction") or "").strip() for e in lots} - {""})
    plan = [e.text.strip() for e in root.iter() if "PlanAdoption" in _local(e.tag) or "planAdoption" in _local(e.tag)
            if e.text and e.text.strip()]
    return {"issuer_cik": int(re.sub(r"\D", "", _first(root, "issuerCik") or "") or 0) or None,
            "issuer_name": _first(root, "issuerName"),
            "seller_name": _first(root, "nameOfPersonForWhoseAccountTheSecuritiesAreToBeSold"),
            "relationship": ",".join(rels) or None,
            "security_class": _first(info[0], "securitiesClassTitle") if info else None,
            "broker": _first(info[0], "name") if info else None,
            "units_to_sell": sum(u for u in units if u is not None) if any(u is not None for u in units) else None,
            "aggregate_market_value": sum(v for v in value if v is not None) if any(v is not None for v in value) else None,
            "units_outstanding": _num(_first(info[0], "noOfUnitsOutstanding")) if info else None,
            "approx_sale_date": _mdy(_first(info[0], "approxSaleDate")) if info else None,
            "exchange": _first(info[0], "securitiesExchangeName") if info else None,
            "n_blocks": len(info), "n_lots": len(lots), "acquisition_nature": "|".join(natures)[:500] or None,
            "sold_past_3m_reported": bool(sold3m) or None,
            "nothing_sold_past_3m": (_first(root, "nothingToReportFlagOnSecuritiesSoldInPast3Months") or "").upper() == "Y",
            "notice_date": _mdy(_first(root, "noticeDate")), "plan_10b5_1_adoption": _mdy(plan[0]) if plan else None}


FIELDS = ("accession", "form", "is_amendment", "filing_date", "available_at", "issuer_cik", "issuer_basis", "issuer_name",
          "filer_ciks", "doc_status", "seller_name", "relationship", "security_class", "broker", "units_to_sell",
          "aggregate_market_value", "units_outstanding", "pct_outstanding", "approx_sale_date", "exchange", "n_blocks",
          "n_lots", "acquisition_nature", "sold_past_3m_reported", "nothing_sold_past_3m", "notice_date",
          "plan_10b5_1_adoption", "doc_url", "doc_sha256")


def _schema():
    import pyarrow as pa

    t = {"is_amendment": pa.bool_(), "filing_date": pa.date32(), "available_at": pa.timestamp("us", tz="UTC"),
         "issuer_cik": pa.int64(), "units_to_sell": pa.float64(), "aggregate_market_value": pa.float64(),
         "units_outstanding": pa.float64(), "pct_outstanding": pa.float64(), "approx_sale_date": pa.date32(),
         "n_blocks": pa.int32(), "n_lots": pa.int32(), "sold_past_3m_reported": pa.bool_(),
         "nothing_sold_past_3m": pa.bool_(), "notice_date": pa.date32(), "plan_10b5_1_adoption": pa.date32()}
    return pa.schema([(k, t.get(k, pa.string())) for k in FIELDS])


def _sic_ciks():
    return D.sic_ciks()


def build_notices() -> dict[str, Any]:
    """Write ``form144_notices.parquet`` (pyarrow + stdlib; no DuckDB)."""
    import os

    import pyarrow as pa
    import pyarrow.parquet as pq

    store = D.store(SOURCE)
    sic = set(_sic_ciks().to_pylist())
    out = C.stage_dir(STAGE) / "form144_notices.parquet"
    tmp = out.with_name(out.name + ".partial")
    w = pq.ParquetWriter(tmp, _schema(), compression="zstd")
    stats = {"notices": 0, "landed": 0, "parsed": 0, "parse_error": 0}
    buf: list[dict[str, Any]] = []
    for acc, cik, form, fd, pdoc, av, ciks in D.select_filings(FORMS, NOTICE_FROM):
        stats["notices"] += 1
        subj = [c for c in ciks if c in sic]
        row: dict[str, Any] = {"accession": acc, "form": form, "is_amendment": form.endswith("/A"), "filing_date": fd,
                               "available_at": av, "issuer_cik": subj[0] if len(subj) == 1 else None,
                               "issuer_basis": "single_cik_with_sic" if len(subj) == 1 else ("ambiguous" if subj else "none"),
                               "filer_ciks": ",".join(str(c) for c in ciks if c not in subj) or None,
                               "doc_status": "not_landed"}
        url = D.doc_url(cik, acc, D.structured_document(pdoc)) if pdoc and pdoc.lower().startswith("xsl") else None
        rec = store.lookup(url) if url else None
        if rec is not None and rec.ok:
            stats["landed"] += 1
            row.update(doc_url=url, doc_sha256=rec.sha256)
            try:
                p = parse_144(store.read(rec))
                row.update({k: v for k, v in p.items() if k in FIELDS})
                if p.get("issuer_cik"):
                    row["issuer_cik"], row["issuer_basis"] = p["issuer_cik"], "xml"
                    row["filer_ciks"] = ",".join(str(c) for c in ciks if c != p["issuer_cik"]) or None
                if row.get("units_to_sell") and row.get("units_outstanding"):
                    row["pct_outstanding"] = 100.0 * row["units_to_sell"] / row["units_outstanding"]
                row["doc_status"] = "parsed"
                stats["parsed"] += 1
            except Exception:
                row["doc_status"] = "parse_error"
                stats["parse_error"] += 1
        buf.append(row)
        if len(buf) >= 20000:
            w.write_table(pa.Table.from_pylist(buf, schema=_schema()))
            buf = []
    if buf:
        w.write_table(pa.Table.from_pylist(buf, schema=_schema()))
    w.close()
    os.replace(tmp, out)
    return stats


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--max-new", type=int, required=True)
    f.add_argument("--threads", type=int, default=2)
    sub.add_parser("notices")
    args = ap.parse_args(argv)
    D.start_memory_trace()
    t0 = time.perf_counter()
    if args.cmd == "fetch":
        urls = fetch_urls()
        print(f"form144: {len(urls)} structured notices", flush=True)
        res = D.fetch_urls(SOURCE, urls, max_new=args.max_new, threads=args.threads)
    else:
        res = build_notices()
    print(json.dumps({**res, "elapsed_s": round(time.perf_counter() - t0, 1)}), flush=True)
    print(json.dumps({"peak_memory_gb": D.peak_memory_gb(), "run": "unguarded per C-1 (network / pyarrow, no DuckDB)"}),
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
