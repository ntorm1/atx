"""Explicit accession-level source admission independent of feature coverage."""
from __future__ import annotations

import datetime as dt
import argparse
import json
import re
from collections.abc import Iterable
from typing import Any
from pathlib import Path

from . import common as C

STAGE = "thirteenf_quality"
SCHEMA = "atx.alpha-panel.thirteenf-quality/v1"

FORMS = {"13F-HR", "13F-HR/A", "13F-NT", "13F-NT/A"}


def effective_chain(filings:list[dict[str,Any]],cutoff:dt.datetime) -> dict[str,Any]:
    """One filer/report cohort's actual-public-clock base/additive amendment chain.

    Notices are accounted for but cannot become known-zero holdings. Unknown
    amendment types or inconsistent report dates remain keyed exceptions.
    """
    if cutoff.tzinfo is None:
        raise ValueError("filing cutoff requires timezone")
    visible=[]
    exceptions=[]
    for filing in filings:
        accession=accession_key(filing["accession"])
        value=filing.get("available_at")
        if not value:
            exceptions.append({"accession":accession,"reason":"public_clock_missing"})
            continue
        clock=dt.datetime.fromisoformat(value.replace("Z","+00:00"))
        if clock.tzinfo is None:
            raise ValueError("filing public clock requires timezone")
        if clock<cutoff:
            visible.append((clock,accession,filing))
    visible.sort(key=lambda row:(row[0],row[1]))
    report_dates={row[2].get("report_date") for row in visible}
    filers={row[2].get("filer_cik") for row in visible}
    if len(report_dates)>1 or None in report_dates or len(filers)>1 or None in filers:
        exceptions.append({"accession":None,"reason":"filer_or_report_cohort_inconsistent"})
    base=None
    additions=[]
    notices=[]
    for clock,accession,filing in visible:
        form=filing["form"]
        amendment=(filing.get("amendment_type") or "").upper().strip()
        if form in {"13F-NT","13F-NT/A"}:
            notices.append(accession)
        elif form=="13F-HR" or (form=="13F-HR/A" and amendment=="RESTATEMENT"):
            base=accession
            additions=[]
        elif form=="13F-HR/A" and amendment=="NEW HOLDINGS":
            if base is None:
                exceptions.append({"accession":accession,"reason":"additive_amendment_without_base"})
            else:
                additions.append(accession)
        else:
            exceptions.append({"accession":accession,"reason":"form_or_amendment_type_unresolved"})
    return {"effective_accessions":([base]+additions) if base else [],"notices":notices,
        "visible_accessions":[row[1] for row in visible],"exceptions":exceptions,
        "chain_status":"FAIL" if exceptions else "CANDIDATE",
        "holdings_zero_known":False}


def accession_key(value: str) -> str:
    key = value.replace("-", "")
    if not re.fullmatch(r"\d{18}", key):
        raise ValueError(f"invalid accession: {value}")
    return key


def master_inventory(lines: Iterable[str], *, inventory_url: str,
                     inventory_sha256: str) -> Iterable[dict[str, Any]]:
    """Read SEC master.idx without guessing report quarters or acceptance clocks.

    A master inventory establishes filing-date membership only. Cover/submission
    evidence must separately supply holdings report quarter and acceptance.
    """
    for line in lines:
        fields = line.rstrip().split("|")
        if len(fields) != 5 or fields[2] not in FORMS:
            continue
        cik, name, form, filed, filename = fields
        accession = accession_key(filename.rsplit("/", 1)[-1].removesuffix(".txt"))
        date = dt.date.fromisoformat(filed)
        yield {"accession": accession, "cik": int(cik), "name": name, "form": form,
               "filing_date": date.isoformat(), "filing_quarter": f"{date.year}q{(date.month-1)//3+1}",
               "source_url": "https://www.sec.gov/Archives/" + filename,
               "inventory_url": inventory_url, "inventory_sha256": inventory_sha256,
               "report_quarter": None, "available_at": None}


def submissions_inventory(document: dict[str, Any], *, cik: int, member: str,
                          archive_sha256: str, first: dt.date) -> Iterable[dict[str, Any]]:
    """Recent and historical SEC submission arrays, with strict shape validation.

    An inventory slice is not a complete cohort certificate. Main document
    filings.files references must also be reconciled to retained history members.
    Missing reportDate/acceptance remain explicit; no guessed ownership quarter.
    """
    fields = document.get("filings", {}).get("recent", document)
    forms = fields.get("form", [])
    for name in ("accessionNumber", "filingDate"):
        if len(fields.get(name, [])) != len(forms):
            raise ValueError("submission arrays have unequal required lengths")
    for name in ("reportDate", "acceptanceDateTime"):
        if name in fields and len(fields[name]) != len(forms):
            raise ValueError("submission arrays have unequal optional lengths")
    for index, form in enumerate(forms):
        if form not in FORMS:
            continue
        filed = dt.date.fromisoformat(fields["filingDate"][index])
        if filed < first:
            continue
        accession = accession_key(fields["accessionNumber"][index])
        report_date = fields.get("reportDate", [None] * len(forms))[index] or None
        report = dt.date.fromisoformat(report_date) if report_date else None
        accepted = fields.get("acceptanceDateTime", [None] * len(forms))[index] or None
        yield {"accession": accession, "cik": cik, "form": form,
            "filing_date": filed.isoformat(), "filing_quarter": f"{filed.year}q{(filed.month-1)//3+1}",
            "report_date": report_date,
            "report_quarter": f"{report.year}q{(report.month-1)//3+1}" if report else None,
            "available_at": accepted,"clock_basis": "submission_acceptance" if accepted else "clock_missing",
            "source_url": f"https://www.sec.gov/Archives/edgar/data/{cik}/{accession}/",
            "inventory_url": "https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip",
            "inventory_sha256": archive_sha256,"inventory_member": member}


def inventory_submissions(archive: Path, output: Path, *, after: str | None,
                         through: str, max_main: int, first: dt.date,
                         max_member_bytes: int = 4 * 1024 * 1024) -> dict[str, Any]:
    """Bounded read-only archive partition; never rebuild another producer's cache.

    Full partition reader coverage and missing history chunk keys are retained.
    Complete inventory admission requires all partitions across the full CIK
    axis, consistent archive identity and every referenced public history chunk.
    """
    from atx_db._submissions_archive import SubmissionsArchive, _one_info

    class ReadOnlyArchive(SubmissionsArchive):
        def _build_index(self) -> None:
            raise ValueError("authenticated submissions directory required; no cache mutation allowed")

    if not 1 <= max_main <= 10000 or not 1 <= max_member_bytes <= 4*1024*1024:
        raise ValueError("positive inventory bounds required")
    if output.exists():
        raise ValueError("inventory output exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_suffix(output.suffix + ".partial")
    counts = {"main_members": 0,"history_members": 0,"filings": 0}
    exceptions = []
    last = after
    with ReadOnlyArchive(archive) as source, partial.open("x",encoding="utf-8") as sink:
        def read_member(name: str) -> dict[str, Any]:
            indexed = source._connection().execute("SELECT record FROM members WHERE name=?",(name,)).fetchone()
            if indexed is None:
                raise KeyError(name)
            if _one_info(indexed[0]).file_size > max_member_bytes:
                raise ValueError("submission member exceeds explicit byte bound")
            return json.loads(source.read(name))

        def emit(member: str,doc: dict[str,Any],cik: int) -> None:
            for row in submissions_inventory(doc,cik=cik,member=member,archive_sha256=source.sha256,first=first):
                sink.write(json.dumps(row) + "\n")
                counts["filings"] += 1

        bounded = False
        for name in source.main_members(after=after,through=through):
            if counts["main_members"] >= max_main:
                bounded = True
                break
            last = name
            cik = int(re.fullmatch(r"CIK(\d{10})\.json",name).group(1))
            try:
                document = read_member(name)
                history_references = document.get("filings",{}).get("files",[])
                emit(name,document,cik)
                # Keep only reference metadata; do not retain all historical JSON
                # documents for one manager in memory.
                del document
                for history in history_references:
                    # Retain even old references; the complete history contract is
                    # independent of a current recent-array observation window.
                    history_name = history["name"]
                    if history_name not in source:
                        exceptions.append({"cik":cik,"member":history_name,"reason":"history_member_missing"})
                        continue
                    try:
                        emit(history_name,read_member(history_name),cik)
                        counts["history_members"] += 1
                    except (KeyError,ValueError) as exc:
                        exceptions.append({"cik":cik,"member":history_name,"reason":str(exc)})
            except (KeyError,ValueError) as exc:
                exceptions.append({"cik":cik,"member":name,"reason":str(exc)})
            counts["main_members"] += 1
        source.assert_unchanged()
        archive_sha = source.sha256
    partial.replace(output)
    receipt = {"schema":"atx.thirteenf-submissions-inventory-partition/v1", "counts":counts,
        "after":after,"through":through,"last_main_member":last,"bound_reached":bounded,
        "first_filing_date":first.isoformat(),"exceptions":exceptions,"archive_sha256":archive_sha,
        "source_completeness":"UNQUALIFIED_PARTITION", "output_sha256":C.sha256_file(output),
        "archive_source_url":"https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip",
        "archive_retrieval_clock":None,
        "code_sha256": {"thirteenf_quality":C.sha256_file(Path(__file__)),
            "submissions_archive":C.sha256_file(Path(__import__('atx_db._submissions_archive',fromlist=['__file__']).__file__))}}
    C.write_json_atomic(output.with_suffix(output.suffix + ".receipt.json"),receipt)
    return receipt


def reconcile(expected: dict[str, Any], retained: dict[str, Any] | None,
              parsed: dict[str, Any] | None) -> dict[str, Any]:
    """FAIL every unexplained gap; exemptions need original public evidence.

    Notices still require their source and cover parse. Confidentiality cannot
    exempt the public filing itself. No coverage-based source imputation.
    """
    accession = accession_key(expected["accession"])
    reasons: list[str] = []
    if not expected.get("inventory_sha256"):
        reasons.append("inventory_provenance_missing")
    if not retained or not all(retained.get(k) for k in ("path", "sha256", "url", "fetched_at", "verified_original")):
        reasons.append("retained_original_missing")
    if not parsed or parsed.get("parse_ok") is not True:
        reasons.append("parsed_accession_missing_or_failed")
    for label, row in (("retained", retained), ("parsed", parsed)):
        if row and row.get("accession") and accession_key(row["accession"]) != accession:
            reasons.append(label + "_accession_mismatch")
    if parsed:
        if not parsed.get("available_at"):
            reasons.append("acceptance_or_conservative_clock_missing")
        else:
            try:
                clock = dt.datetime.fromisoformat(parsed["available_at"].replace("Z", "+00:00"))
                if clock.tzinfo is None:
                    reasons.append("clock_timezone_missing")
            except (ValueError, TypeError):
                reasons.append("clock_invalid")
        if not parsed.get("report_quarter"):
            reasons.append("report_quarter_missing")
        if expected["form"] in {"13F-HR", "13F-HR/A"}:
            if parsed.get("value_unit") not in {"USD", "USD_THOUSANDS"}:
                reasons.append("value_unit_unknown")
            if parsed.get("cover_reconciled") is not True:
                reasons.append("cover_totals_unreconciled")
            if parsed.get("manager_chain_resolved") is not True:
                reasons.append("shared_manager_or_amendment_chain_unresolved")
            if parsed.get("holdings_rows", 0) == 0 and not parsed.get("zero_holdings_source_evidence"):
                reasons.append("zero_holdings_unexplained")
    return {"accession": accession, "filing_quarter": expected.get("filing_quarter"),
            "report_quarter": (parsed or {}).get("report_quarter"),
            "gate_status": "FAIL" if reasons else "PASS", "exceptions": reasons}


def _load_jsonl(con, table: str, path: Path) -> int:
    """Disk-backed keyed normalized adapter input; duplicate accession fails closed."""
    con.execute(f"CREATE TABLE {table} (accession VARCHAR PRIMARY KEY, payload VARCHAR)")
    count = 0
    with path.open(encoding="utf-8") as stream:
        batch = []
        for line in stream:
            if not line.strip():
                continue
            row = json.loads(line)
            batch.append((accession_key(row["accession"]), json.dumps(row)))
            count += 1
            if len(batch) == 512:
                con.executemany(f"INSERT INTO {table} VALUES (?, ?)", batch)
                batch.clear()
        if batch:
            con.executemany(f"INSERT INTO {table} VALUES (?, ?)", batch)
    return count


def parsed_adapter_sql(filings: str, holdings: str, sec_filings: str) -> str:
    """Reconcile a complete explicitly selected parsed bulk part, never receipts.

    Acceptance evidence overrides the historical guessed 46h clock. Unknown
    manager/amendment chains stay unresolved; this adapter cannot admit them.
    Cover counts/value totals are independent of security mapping coverage.
    """
    paths = [p.replace("'", "''") for p in (filings, holdings, sec_filings)]
    f, h, s = paths
    return f"""WITH clocks AS (
        SELECT replace(accession,'-','') AS accession,
               max(acceptance_utc) FILTER (WHERE acceptance_utc IS NOT NULL) AS acceptance_utc,
               string_agg(DISTINCT vintage_risk, ';') AS vintage_risk
        FROM read_parquet('{s}',hive_partitioning=false)
        WHERE form IN ('13F-HR','13F-HR/A','13F-NT','13F-NT/A') GROUP BY 1),
    totals AS (
        SELECT replace(accession,'-','') AS accession, count(*) AS holdings_rows,
               sum(value_raw) AS value_raw_total,
               count(*) FILTER(WHERE value_raw IS NULL) AS missing_values,
               count(DISTINCT value_unit) AS unit_regimes,
               min(value_unit) AS value_unit,
               count(*) FILTER(WHERE sshprnamt_type NOT IN ('SH','PRN') OR sshprnamt_type IS NULL) AS bad_share_classes,
               count(*) FILTER(WHERE put_call IS NOT NULL AND put_call NOT IN ('PUT','CALL')) AS bad_options,
               count(*) FILTER(WHERE other_manager IS NOT NULL) AS shared_rows
        FROM read_parquet('{h}',hive_partitioning=false) GROUP BY 1)
    SELECT replace(f.accession,'-','') AS accession,
        f.submission_type AS form, f.filer_cik, f.period_of_report,
        strftime(f.period_q,'%Y') || 'q' || CAST(quarter(f.period_q) AS VARCHAR) AS report_quarter,
        CASE WHEN c.acceptance_utc IS NOT NULL THEN c.acceptance_utc AT TIME ZONE 'UTC'
             ELSE f.available_at END AS available_at,
        CASE WHEN c.acceptance_utc IS NOT NULL AND c.vintage_risk IS NOT NULL THEN 'conservative_acceptance_risk'
             WHEN c.acceptance_utc IS NOT NULL THEN 'public_acceptance'
             ELSE 'documented_filed_plus_46h_floor' END AS clock_basis,
        f.submission_type IN ('13F-HR','13F-HR/A','13F-NT','13F-NT/A') AS parse_ok,
        coalesce(t.holdings_rows,0) AS holdings_rows,
        CASE WHEN t.unit_regimes=1 AND t.value_unit='dollars' THEN 'USD'
             WHEN t.unit_regimes=1 AND t.value_unit='thousands' THEN 'USD_THOUSANDS' END AS value_unit,
        (t.holdings_rows=f.table_entry_total AND t.missing_values=0
          AND abs(t.value_raw_total-f.table_value_total_raw)<=greatest(1.0,abs(f.table_value_total_raw)*0.000001)
          AND t.bad_share_classes=0 AND t.bad_options=0) AS cover_reconciled,
        (f.other_included_managers_count=0 AND coalesce(t.shared_rows,0)=0
          AND f.submission_type='13F-HR'
          AND upper(coalesce(f.is_amendment,'')) NOT IN ('Y','TRUE','1')) AS manager_chain_resolved,
        false AS zero_holdings_source_evidence,
        f.amendment_type, f.report_type, f.is_confidential_omitted,
        f.table_entry_total, f.table_value_total_raw, t.value_raw_total,
        t.bad_share_classes, t.bad_options, t.shared_rows
    FROM read_parquet('{f}',hive_partitioning=false) f
    LEFT JOIN clocks c ON replace(f.accession,'-','')=c.accession
    LEFT JOIN totals t ON replace(f.accession,'-','')=t.accession"""


def adapt_parsed(filings: Path, holdings: Path, sec_filings: Path, output: Path) -> dict[str, Any]:
    if output.exists():
        raise ValueError("adapter output exists; use a fresh evidence path")
    paths = {"filings": filings, "holdings": holdings, "sec_filings": sec_filings}
    identities = {k: C.file_identity(p) for k, p in paths.items()}
    hashes = {k: C.sha256_file(p) for k, p in paths.items()}
    con = C.connect(memory="128MB", threads=1, db_file="thirteenf_adapter.duckdb")
    temporary = output.with_suffix(output.suffix + ".partial")
    try:
        cursor = con.execute(parsed_adapter_sql(*(p.as_posix() for p in paths.values())))
        columns = [v[0] for v in cursor.description]
        output.parent.mkdir(parents=True, exist_ok=True)
        count = 0
        with temporary.open("x", encoding="utf-8") as stream:
            while rows := cursor.fetchmany(512):
                for row in rows:
                    stream.write(json.dumps(dict(zip(columns, row)), default=str) + "\n")
                    count += 1
        if any(C.file_identity(p) != identities[k] or C.sha256_file(p) != hashes[k] for k,p in paths.items()):
            raise RuntimeError("parsed adapter inputs changed")
        temporary.replace(output)
        receipt = {"rows": count, "input_evidence_sha256": hashes, "input_evidence_files": identities,
                   "gate_status": "UNQUALIFIED", "output_sha256": C.sha256_file(output)}
        C.write_json_atomic(output.with_suffix(output.suffix + ".receipt.json"), receipt)
        return receipt
    finally:
        con.close()


def build(expected: Path, retained: Path, parsed: Path) -> dict[str, Any]:
    """Publish all expected accession gates and explicit unexpected parsed keys.

    Inputs are normalized evidence adapters, never self-certified by this build.
    Stage publication is diagnostic even if every accession predicate passes:
    inventory scope and independent original/member verification are required.
    """
    paths = {"expected": expected, "retained": retained, "parsed": parsed}
    before = {name: C.file_identity(path) for name, path in paths.items()}
    hashes = {name: C.sha256_file(path) for name, path in paths.items()}
    out = C.stage_dir(STAGE)
    con = C.connect(memory="128MB", threads=1, db_file="thirteenf_quality.duckdb")
    try:
        counts = {name: _load_jsonl(con, name, path) for name, path in paths.items()}
        con.execute("""CREATE TABLE gates (accession VARCHAR PRIMARY KEY, filing_quarter VARCHAR,
                       report_quarter VARCHAR, gate_status VARCHAR, exceptions VARCHAR[], available_at TIMESTAMPTZ)""")
        cursor = con.cursor()
        cursor.execute("""SELECT e.payload, r.payload, p.payload FROM expected e
                       LEFT JOIN retained r USING(accession) LEFT JOIN parsed p USING(accession)
                       ORDER BY e.accession""")
        generated = dt.datetime.now(dt.UTC)
        while batch := cursor.fetchmany(512):
            result = []
            for e, r, p in batch:
                gate = reconcile(json.loads(e), json.loads(r) if r else None, json.loads(p) if p else None)
                result.append((gate["accession"], gate["filing_quarter"], gate["report_quarter"],
                               gate["gate_status"], gate["exceptions"], generated))
            con.executemany("INSERT INTO gates VALUES (?, ?, ?, ?, ?, ?)", result)
        cursor.close()
        if any(C.file_identity(path) != before[name] or C.sha256_file(path) != hashes[name]
               for name, path in paths.items()):
            raise RuntimeError("source evidence changed during quality build")
        C.copy_to_parquet(con, "SELECT * FROM gates ORDER BY accession", out / "accessions.parquet")
        # Parsed keys outside an authoritative inventory are never silently dropped.
        extra = C.copy_to_parquet(con, """SELECT p.accession, 'unexpected_parsed_accession' AS reason,
            current_timestamp AS available_at FROM parsed p ANTI JOIN expected e USING(accession)
            ORDER BY p.accession""", out / "unexpected_parsed.parquet")
        failures = con.execute("SELECT count(*) FROM gates WHERE gate_status='FAIL'").fetchone()[0]
        receipt = {"input_counts": counts, "failed_accessions": failures, "unexpected_parsed": extra,
                   "gate_status": "FAIL" if failures or extra or not counts["expected"] else "UNQUALIFIED",
                   "source_completeness": "UNQUALIFIED_NORMALIZED_ADAPTERS",
                   "rows": counts["expected"]}
        C.write_stage_manifest(STAGE, SCHEMA, ("thirteenf_quality", "common"), {
            "receipt": receipt, "gate_status": receipt["gate_status"],
            "input_evidence_sha256": hashes, "input_evidence_files": before,
            "input_manifests_sha256": {},
            "limits": "Normalized evidence claims require independent source verification; no complete-source admission"})
        return receipt
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("build")
    for name in ("expected", "retained", "parsed"):
        command.add_argument("--" + name, type=Path, required=True)
    adapter = sub.add_parser("adapt-parsed")
    for name in ("filings", "holdings", "sec-filings", "output"):
        adapter.add_argument("--" + name, type=Path, required=True)
    inventory = sub.add_parser("inventory-submissions")
    inventory.add_argument("--archive",type=Path,required=True)
    inventory.add_argument("--output",type=Path,required=True)
    inventory.add_argument("--after")
    inventory.add_argument("--through",required=True)
    inventory.add_argument("--max-main",type=int,default=1000)
    inventory.add_argument("--first",type=dt.date.fromisoformat,default=dt.date(2019,1,1))
    args = parser.parse_args(argv)
    if args.command == "inventory-submissions":
        receipt = inventory_submissions(args.archive,args.output,after=args.after,through=args.through,
                                        max_main=args.max_main,first=args.first)
        print(json.dumps(receipt,sort_keys=True))
        return 2 if receipt["exceptions"] or receipt["bound_reached"] else 0
    receipt = (build(args.expected, args.retained, args.parsed) if args.command == "build" else
               adapt_parsed(args.filings, args.holdings, args.sec_filings, args.output))
    print(json.dumps(receipt, sort_keys=True))
    return 2 if receipt["gate_status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
