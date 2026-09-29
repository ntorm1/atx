"""Stage F input: stream the SEC Company Facts archive into fact batches (us-gaap, ifrs-full, dei).

The shared staging extract (``data/staging/companyfacts/<sha16>``) keeps only a 268-concept us-gaap/dei
allowlist, so it has no ``ifrs-full`` facts and misses the fallback concepts stage F needs (pre-tax income,
interest expense, cost of sales variants, payables, goodwill, ...). This module re-reads the pinned archive
``data/cache/companyfacts.zip`` (same snapshot, sha256 ``ee099c73...``) member by member and writes

* ``fundamentals/_work/cf/batch-NNNN.parquet``: one row per fact of a periodic form (10-K, 10-Q, 10-KT,
  10-QT, 20-F, 40-F and their ``/A``) whose unit is ``shares`` or an ISO-4217-like currency code
  (three capital letters), for (a) us-gaap concepts matching ``USGAAP_PATTERN`` or named in
  ``fund_items.CHAINS``, (b) every ``ifrs-full`` concept, (c) dei ``EntityCommonStockSharesOutstanding``.
  Columns: ``cik, taxonomy, concept, unit, period_start, period_end, filed_date, fiscal_year,
  fiscal_period, form, accession_number, value`` (value = the JSON number as float64);
* ``batch-NNNN.stats.parquet``: ``(taxonomy, concept, unit, n_facts, n_ciks)`` over every fact of a
  periodic form in the batch, all taxonomies (concept mining without a re-read);
* ``batch-NNNN.json``: receipt (archive identity, code version, members, rows, parquet sha256).

A batch whose receipt matches the archive identity and ``CODE_VERSION`` is skipped (resumable). One member
is parsed at a time (standard-library ``json``; the largest member is 9 MB).

Usage (guarded)::

    python -m atx_db.alpha_panel.fund_extract run [--only 0-9]
    python -m atx_db.alpha_panel.fund_extract stats      # print the concept census
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
import time
import zipfile
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from . import common
from . import fund_items as fi

CODE_VERSION = "fund-extract-v2"
ARCHIVE = common.PACKAGE_ROOT / "data" / "cache" / "companyfacts.zip"
TARGET_UNCOMPRESSED = 230_000_000
ROW_GROUP = 131_072
MEMBER_RE = re.compile(r"CIK([0-9]{10})\.json$")
CURRENCY_RE = re.compile(r"^[A-Z]{3}$")
BASE_FORMS = ("10-K", "10-Q", "10-KT", "10-QT", "20-F", "40-F")
FORMS = frozenset(BASE_FORMS + tuple(f + "/A" for f in BASE_FORMS))
# Generous us-gaap allowlist: every concept family stage F reads or may mine for fallbacks.
USGAAP_PATTERN = re.compile(
    r"Revenue|Sales|CostOf|CostsAndExpenses|GrossProfit|OperatingIncome|OperatingExpenses|"
    r"IncomeLoss|NetIncome|ProfitLoss|InterestExpense|InterestAndDebtExpense|InterestIncomeExpense|"
    r"InterestAndDividendIncome|Noninterest|InterestPaid|IncomeTax|PaymentsToAcquire|PaymentsForCapital|"
    r"PaymentsForConstruction|CapitalExpenditure|ResearchAndDevelopment|Depreciation|Amortization|Goodwill|"
    r"IntangibleAssets|AccountsPayable|DeferredRevenue|ContractWithCustomerLiability|PropertyPlantAndEquipment|"
    r"Dividend|MinorityInterest|NoncontrollingInterest|PreferredStock|StockholdersEquity|^Assets|^Liabilities|"
    r"Cash|Debt|Borrowing|NotesPayable|LoansPayable|CommercialPaper|SeniorNotes|LineOfCredit|FinanceLease|"
    r"Inventor|Receivable|SharesOutstanding|WeightedAverageNumber|NetCashProvidedByUsedIn|ShortTermInvestments|"
    r"MarketableSecurities|AvailableForSale|DeferredTax|DeferredIncomeTax|SellingGeneral|GeneralAndAdministrative|"
    r"SellingAndMarketing|SellingExpense|MarketingExpense|ProceedsFromIssuance|ProceedsFromStockOptions|"
    r"PaymentsForRepurchase|TreasuryStock|PaymentsOfDividends|ProvisionForLoan|PremiumsEarned|"
    r"NetInvestmentIncome|BenefitsLossesAndExpenses|PolicyholderBenefits|OperatingLease|RealEstate|"
    r"UtilityRevenue|RegulatedOperatingRevenue|OilAndGas|FeesAndCommissions|InvestmentIncome|StockRepurchaseProgram"
)
DEI_KEEP = frozenset({fi.DEI_SHARES})

SCHEMA = pa.schema([
    ("cik", pa.int64()), ("taxonomy", pa.string()), ("concept", pa.string()), ("unit", pa.string()),
    ("period_start", pa.date32()), ("period_end", pa.date32()), ("filed_date", pa.date32()),
    ("fiscal_year", pa.int32()), ("fiscal_period", pa.string()), ("form", pa.string()),
    ("accession_number", pa.string()), ("value", pa.float64()),
])
STATS_SCHEMA = pa.schema([("taxonomy", pa.string()), ("concept", pa.string()), ("unit", pa.string()),
                          ("n_facts", pa.int64()), ("n_ciks", pa.int64())])


def out_dir() -> Path:
    path = common.stage_dir("fundamentals") / "_work" / "cf"
    path.mkdir(parents=True, exist_ok=True)
    return path


def batch_files() -> list[Path]:
    return sorted(out_dir().glob("batch-[0-9][0-9][0-9][0-9].parquet"))


def archive_identity() -> dict[str, Any]:
    return common.file_identity(ARCHIVE)


def chain_concepts() -> frozenset[str]:
    chains = list(fi.CHAINS.values()) + list(fi.CHAINS_EXT.values())
    return frozenset(c for chain in chains for c in chain if ":" not in c) | frozenset(fi.MAIN_STATEMENT_CONCEPTS)


def keep_concept(taxonomy: str, concept: str, chain: frozenset[str]) -> bool:
    if taxonomy == "us-gaap":
        return concept in chain or USGAAP_PATTERN.search(concept) is not None
    if taxonomy == "ifrs-full":
        return True
    if taxonomy == "dei":
        return concept in DEI_KEEP
    return False


def keep_unit(unit: str) -> bool:
    return unit == "shares" or CURRENCY_RE.match(unit) is not None


def plan(z: zipfile.ZipFile) -> list[list[zipfile.ZipInfo]]:
    """Members in CIK order, cut into contiguous batches of about ``TARGET_UNCOMPRESSED`` bytes."""
    infos = sorted((i for i in z.infolist() if MEMBER_RE.search(i.filename)), key=lambda i: i.filename)
    batches: list[list[zipfile.ZipInfo]] = [[]]
    size = 0
    for info in infos:
        if batches[-1] and size + info.file_size > TARGET_UNCOMPRESSED:
            batches.append([])
            size = 0
        batches[-1].append(info)
        size += info.file_size
    return batches


class _Buffer:
    def __init__(self, writer: pq.ParquetWriter) -> None:
        self.w = writer
        self.cols: dict[str, list] = {n: [] for n in SCHEMA.names}
        self.rows = 0

    def add(self, cik: int, tax: str, concept: str, unit: str, f: dict[str, Any]) -> None:
        c = self.cols
        c["cik"].append(cik)
        c["taxonomy"].append(tax)
        c["concept"].append(concept)
        c["unit"].append(unit)
        c["period_start"].append(f.get("start"))
        c["period_end"].append(f.get("end"))
        c["filed_date"].append(f.get("filed"))
        fy = f.get("fy")
        c["fiscal_year"].append(fy if isinstance(fy, int) else None)
        c["fiscal_period"].append(f.get("fp"))
        c["form"].append(f.get("form"))
        c["accession_number"].append(f.get("accn"))
        v = f.get("val")
        c["value"].append(float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None)
        self.rows += 1
        if len(c["cik"]) >= ROW_GROUP:
            self.flush()

    def flush(self) -> None:
        c = self.cols
        if not c["cik"]:
            return
        arrays = []
        for name, typ in zip(SCHEMA.names, SCHEMA.types):
            if pa.types.is_date32(typ):
                arrays.append(pc.cast(pa.array(c[name], pa.string()), pa.date32()))
            else:
                arrays.append(pa.array(c[name], typ))
        self.w.write_table(pa.Table.from_arrays(arrays, schema=SCHEMA))
        self.cols = {n: [] for n in SCHEMA.names}


def _iter_facts(payload: dict[str, Any]) -> Iterator[tuple[str, str, str, list]]:
    facts = payload.get("facts")
    if not isinstance(facts, dict):
        return
    for tax, concepts in facts.items():
        if not isinstance(concepts, dict):
            continue
        for concept, body in concepts.items():
            units = body.get("units") if isinstance(body, dict) else None
            if not isinstance(units, dict):
                continue
            for unit, rows in units.items():
                if isinstance(rows, list):
                    yield tax, concept, unit, rows


def extract_batch(z: zipfile.ZipFile, bid: int, members: list[zipfile.ZipInfo]) -> dict[str, Any]:
    t0 = time.perf_counter()
    dest = out_dir() / f"batch-{bid:04d}.parquet"
    tmp = dest.with_name(dest.name + ".partial")
    chain = chain_concepts()
    stats: dict[tuple[str, str, str], list] = {}
    counts = {"facts_seen_periodic": 0, "rows": 0, "drop_non_numeric": 0, "members_error": 0}
    member_rows = []
    writer = pq.ParquetWriter(tmp, SCHEMA, compression="zstd")
    buf = _Buffer(writer)
    try:
        for info in members:
            cik = int(MEMBER_RE.search(info.filename).group(1))
            before = buf.rows
            try:
                payload = json.loads(z.read(info))
            except (ValueError, UnicodeDecodeError):
                counts["members_error"] += 1
                member_rows.append({"cik": cik, "rows": 0, "error": True})
                continue
            if not isinstance(payload, dict):
                member_rows.append({"cik": cik, "rows": 0})
                continue
            for tax, concept, unit, rows in _iter_facts(payload):
                periodic = [f for f in rows if isinstance(f, dict) and f.get("form") in FORMS]
                if not periodic:
                    continue
                key = (tax, concept, unit)
                s = stats.get(key)
                if s is None:
                    s = stats[key] = [0, 0]
                s[0] += len(periodic)
                s[1] += 1
                counts["facts_seen_periodic"] += len(periodic)
                if not keep_unit(unit) or not keep_concept(tax, concept, chain):
                    continue
                for f in periodic:
                    v = f.get("val")
                    if not isinstance(v, (int, float)) or isinstance(v, bool):
                        counts["drop_non_numeric"] += 1
                        continue
                    buf.add(cik, tax, concept, unit, f)
            member_rows.append({"cik": cik, "rows": buf.rows - before})
            del payload
        buf.flush()
    finally:
        writer.close()
    tmp.replace(dest)
    counts["rows"] = buf.rows
    st = pa.Table.from_pylist(
        [{"taxonomy": t, "concept": c, "unit": u, "n_facts": n, "n_ciks": k} for (t, c, u), (n, k) in stats.items()],
        schema=STATS_SCHEMA)
    spath = out_dir() / f"batch-{bid:04d}.stats.parquet"
    stmp = spath.with_name(spath.name + ".partial")
    pq.write_table(st, stmp, compression="zstd")
    stmp.replace(spath)
    receipt = {
        "code_version": CODE_VERSION, "archive": archive_identity(), "batch_id": bid, "file": dest.name,
        "rows": buf.rows, "parquet_sha256": common.sha256_file(dest), "parquet_bytes": dest.stat().st_size,
        "first_cik": member_rows[0]["cik"] if member_rows else None,
        "last_cik": member_rows[-1]["cik"] if member_rows else None,
        "members": len(members), "uncompressed_bytes": sum(i.file_size for i in members), "counts": counts,
        "member_rows": member_rows, "seconds": round(time.perf_counter() - t0, 2),
        "created_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    }
    common.write_json_atomic(out_dir() / f"batch-{bid:04d}.json", receipt)
    return receipt


def batch_done(bid: int) -> bool:
    rp = out_dir() / f"batch-{bid:04d}.json"
    if not rp.exists():
        return False
    r = common.read_json(rp)
    ident = archive_identity()
    return (r.get("code_version") == CODE_VERSION and r.get("archive", {}).get("bytes") == ident["bytes"]
            and r.get("archive", {}).get("mtime_ns") == ident["mtime_ns"]
            and (out_dir() / r["file"]).exists())


def run(only: tuple[int, int] | None = None) -> None:
    with zipfile.ZipFile(ARCHIVE) as z:
        batches = plan(z)
        common.write_json_atomic(out_dir() / "plan.json", {
            "code_version": CODE_VERSION, "archive": archive_identity(), "batches": len(batches),
            "target_uncompressed_bytes": TARGET_UNCOMPRESSED, "usgaap_pattern": USGAAP_PATTERN.pattern,
            "chain_concepts": sorted(chain_concepts()), "forms": sorted(FORMS)})
        for bid, members in enumerate(batches):
            if only and not (only[0] <= bid <= only[1]):
                continue
            if batch_done(bid):
                continue
            r = extract_batch(z, bid, members)
            print(f"extract {bid:04d}: members={r['members']} rows={r['rows']} {r['seconds']}s", flush=True)
    # drop outputs of batches beyond the plan (never expected: the archive is pinned)
    for p in batch_files():
        if int(p.stem.split("-")[1]) >= len(batches):
            raise AssertionError(f"stale extract batch {p.name}")


def complete() -> bool:
    with zipfile.ZipFile(ARCHIVE) as z:
        n = len(plan(z))
    return all(batch_done(b) for b in range(n))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--only")
    sub.add_parser("stats")
    args = ap.parse_args(argv)
    if args.cmd == "run":
        run(tuple(int(x) for x in args.only.split("-")) if args.only else None)  # type: ignore[arg-type]
    else:
        con = common.connect(memory="300MB", threads=2)
        try:
            g = (out_dir() / "batch-*.stats.parquet").as_posix()
            for row in con.execute(
                    f"SELECT taxonomy, sum(n_facts), sum(n_ciks) FROM read_parquet('{g}') GROUP BY 1 ORDER BY 2 DESC"
            ).fetchall():
                print(row)
        finally:
            con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
