#!/usr/bin/env python
"""C9: filing-sourced historical identity/listing evidence pilot (L2 H2), scratch DB only.

Never opens the warehouse. Every table lives in a SCRATCH DuckDB (``--db``)
bootstrapped at migration head (0327, test-template style: a copy of a built
template, or the production bootstrap path), opened with memory_limit <= 384MB
and 2 threads. Network: only the ``fetch`` stage, through
:class:`atx_db.historical_identity_sources.SecDocumentFetcher` (approved user
agent, <= 4 req/s, RX5 budget of 500 requests across runs, every response
cached and ledgered under ``--cache-dir``).

Stages (``--stages``, in order)::

    bootstrap  copy --template (or run the production bootstrap) into --db; assert head >= 0327
    scan       stream submissions.zip -> c9_sec_entities / c9_sec_former_names / c9_sec_filings
    lines      TickerHistory3 vendor lines and symbol spans -> c9_vendor_lines / c9_vendor_symbols
    plan       stratified fetch plan (form x era x exchange) -> c9_fetch_plan
    fetch      fetch the planned primary documents (budgeted, cached)
    evidence   parse cached documents + bulk facts -> security_identity_evidence (scratch)
    measure    coverage / precision probes / audits / full-acquisition volume -> --out JSON
    sheet      hand-verification excerpts for --sheet-size random parsed documents -> --out text
    linksheet  hand-verification rows for --sheet-size random accepted links on ceased lines -> --out JSONL

Example::

    python scripts/qualify_historical_identity.py --db <scratch>/C9-evidence.duckdb \
        --template .pytest_cache/db_schema_templates/<fp>/warehouse_template.duckdb --stages bootstrap,scan
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import sys
import time
from pathlib import Path

import duckdb

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

from atx_db import historical_identity_sources as his  # noqa: E402

DEFAULT_SUBMISSIONS = ROOT / "data" / "cache" / "submissions.zip"
DEFAULT_PARQUET = ROOT / "data" / "staging" / "broad-bars" / "2026-09-20-updated" / "TickerHistory3.parquet"
DEFAULT_CACHE_DIR = ROOT / "data" / "cache" / "C9-sec-identity-documents"
WAREHOUSE = (ROOT / "data" / "warehouse.duckdb").resolve()
MEMORY_LIMIT = "384MB"
THREADS = 2
STAGES = ("bootstrap", "scan", "lines", "plan", "fetch", "evidence", "measure", "sheet", "linksheet")


def log(message: str) -> None:
    print(f"[{dt.datetime.now(dt.UTC):%H:%M:%S}] {message}", flush=True)


def connect(db: Path) -> duckdb.DuckDBPyConnection:
    if db.resolve() == WAREHOUSE:
        raise SystemExit("refusing to open the warehouse: C9 writes a scratch DB only")
    return duckdb.connect(str(db), config={"memory_limit": MEMORY_LIMIT, "threads": THREADS})


def stage_bootstrap(args: argparse.Namespace) -> None:
    db = Path(args.db)
    if db.exists():
        log(f"bootstrap: {db} exists; checking head")
    elif args.template:
        shutil.copyfile(args.template, db)
        log(f"bootstrap: copied template {args.template}")
    else:
        from atx_db.connection import DuckDBStore

        store = DuckDBStore(db)
        store.connection = duckdb.connect(str(db), config={"memory_limit": "1GB", "threads": 1})
        store._configure_session(store.connection)
        store.initialize()
        store.connection.close()
        log("bootstrap: production bootstrap path completed")
    con = connect(db)
    try:
        head = con.execute("SELECT max(CAST(version AS INTEGER)) FROM schema_migrations").fetchone()[0]
        present = con.execute(
            "SELECT count(*) FROM duckdb_tables() WHERE table_name = ?", [his.EVIDENCE_TABLE]
        ).fetchone()[0]
    finally:
        con.close()
    if head is None or head < 327 or not present:
        raise SystemExit(f"scratch DB is not at migration head 0327 (head={head}, evidence table={present})")
    log(f"bootstrap: head={head:04d}, {his.EVIDENCE_TABLE} present")


def stage_scan(args: argparse.Namespace) -> None:
    con = connect(Path(args.db))
    try:
        for table in ("c9_sec_entities", "c9_sec_former_names", "c9_sec_filings"):
            con.execute(f"DROP TABLE IF EXISTS {table}")

        def progress(stats: his.ScanStats) -> None:
            log(
                f"scan: members={stats.main_members:,} history={stats.history_members_read:,} "
                f"filings={stats.filings_seen:,} identity={stats.identity_filings:,} {stats.seconds:,.0f}s"
            )

        stats = his.scan_submissions_identity(
            args.submissions, con, limit=args.limit, progress=progress, batch_entities=args.batch
        )
        con.execute("CREATE OR REPLACE TABLE c9_scan_receipt AS SELECT ? AS receipt_json", [
            json.dumps({**stats.__dict__, "archive_path": str(args.submissions)}, default=str)
        ])
    finally:
        con.close()
    log(f"scan done: {json.dumps(stats.__dict__, default=str)[:600]}")


def stage_lines(args: argparse.Namespace) -> None:
    con = connect(Path(args.db))
    try:
        summary = his.stage_vendor_lines(con, args.parquet)
    finally:
        con.close()
    log(f"lines: {summary}")


def stage_plan(args: argparse.Namespace) -> None:
    con = connect(Path(args.db))
    try:
        counts = his.build_fetch_plan(con, issuers=args.issuers, era_documents=args.era_documents)
        planned = con.execute("SELECT count(*) FROM c9_fetch_plan").fetchone()[0]
    finally:
        con.close()
    log(f"plan: {planned} documents {counts}")


def _fetcher(args: argparse.Namespace) -> his.SecDocumentFetcher:
    return his.SecDocumentFetcher(args.cache_dir, budget=his.PILOT_FETCH_BUDGET)


def stage_fetch(args: argparse.Namespace) -> None:
    fetcher = _fetcher(args)
    log(f"fetch: budget {fetcher.budget}, used {fetcher.requests_used}, left {fetcher.requests_left}")
    con = connect(Path(args.db))

    def progress(priority: int, total: int, response: his.CachedResponse) -> None:
        if not response.from_cache:
            log(f"fetch {priority}/{total} {response.status} {response.size:,}B {response.url[-70:]}")

    try:
        summary = his.fetch_planned_documents(
            con, fetcher, progress=progress, max_attempts_per_url=args.max_attempts
        )
    finally:
        con.close()
    log(f"fetch: {summary}")


def _submissions_observed_at(path: str) -> dt.datetime:
    stat = Path(path).stat()
    return dt.datetime.fromtimestamp(stat.st_mtime, dt.UTC).replace(tzinfo=None)


def stage_evidence(args: argparse.Namespace) -> None:
    fetcher = _fetcher(args)
    con = connect(Path(args.db))
    run_id = f"C9-{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%S}"
    loaded = dt.datetime.now(dt.UTC).replace(tzinfo=None)
    try:
        receipt = json.loads(con.execute("SELECT receipt_json FROM c9_scan_receipt").fetchone()[0])
        archive_sha = receipt["archive_sha256"]
        bulk_observed = _submissions_observed_at(receipt["archive_path"])
        con.execute(f"DELETE FROM {his.EVIDENCE_TABLE} WHERE evidence_id LIKE 'C9-%'")
        con.execute(
            """CREATE OR REPLACE TABLE c9_parsed_documents (
                priority BIGINT, cik VARCHAR, accession VARCHAR, form VARCHAR, filing_date DATE, url VARCHAR,
                status VARCHAR, issuer_name VARCHAR, exchange_name VARCHAR, mic VARCHAR, class_titles VARCHAR,
                security_types VARCHAR, symbols VARCHAR, rule_provision VARCHAR, notes VARCHAR)"""
        )
        plan = con.execute(
            """SELECT p.priority, p.cik, p.accession, p.form, p.filing_date, p.acceptance_raw, p.primary_document
               FROM c9_fetch_plan p ORDER BY p.priority"""
        ).fetchall()
        document_rows: list[dict[str, object]] = []
        for priority, cik, accession, form, filed, raw, document in plan:
            url = his.archive_document_url(cik, accession, his.raw_document_name(document))
            response = fetcher.cached(url)
            if response is None:
                continue  # never fetched (budget/outage): reported as unfetched, no row
            ref = his.FilingRef(cik, accession, form, filed, raw, document, url, response.sha256, response.fetched_at)
            if response.status != 200:
                parsed = his.ParsedDocument(status=f"http_{response.status}", form=form)
            else:
                parsed = his.parse_document(response.read(), form, his.raw_document_name(document))
            document_rows.extend(his.document_evidence(ref, parsed, run_id=run_id))
            con.execute(
                "INSERT INTO c9_parsed_documents VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    priority, cik, accession, form, filed, url, parsed.status, parsed.issuer_name,
                    parsed.exchange_name, ",".join(sorted({c.mic for c in parsed.classes if c.mic})),
                    json.dumps([c.title for c in parsed.classes]),
                    json.dumps([his.classify_class_title(c.title)[0] for c in parsed.classes]),
                    json.dumps(parsed.symbols), parsed.rule_provision, json.dumps(parsed.notes),
                ],
            )
        for row in document_rows:
            row["source_loaded_at"] = loaded
        written = his.insert_evidence(con, document_rows)
        log(f"evidence: {written} document rows")
        cert_rows = his.bulk_cert_symbol_evidence(con, artifact_sha256=archive_sha, observed_at=bulk_observed, run_id=run_id)
        log(f"evidence: {his.insert_evidence(con, cert_rows)} certification-name symbol rows")
        scope = [
            row[0]
            for row in con.execute(
                """SELECT DISTINCT cik FROM c9_sec_filings
                   WHERE filing_date >= ? AND (form IN ('25', '25-NSE', '8-A12B', '8-A12G') OR form LIKE 'CERT%')""",
                [his.PRICE_WINDOW_START],
            ).fetchall()
        ]
        name_rows = his.former_name_evidence(con, scope, artifact_sha256=archive_sha, observed_at=bulk_observed, run_id=run_id)
        log(f"evidence: {his.insert_evidence(con, name_rows)} issuer-name window rows ({len(scope)} scoped CIKs)")
        line_rows = his.line_link_evidence(con, observed_at=loaded, artifact_sha256=args.parquet_sha256, run_id=run_id)
        log(f"evidence: {his.insert_evidence(con, line_rows)} vendor-line issuer_link rows")
    finally:
        con.close()


def _rows(con: duckdb.DuckDBPyConnection, sql: str, params: list[object] | None = None) -> list[dict[str, object]]:
    cursor = con.execute(sql, params or [])
    names = [column[0] for column in cursor.description]
    return [dict(zip(names, row, strict=True)) for row in cursor.fetchall()]


def stage_measure(args: argparse.Namespace) -> None:
    from atx_db.historical_identity import ACCEPTANCE_SQL_B, audit_identity_evidence

    con = connect(Path(args.db))
    fetcher = _fetcher(args)
    out: dict[str, object] = {"measured_at": dt.datetime.now(dt.UTC).isoformat(), "db": str(args.db)}
    try:
        out["scan_receipt"] = json.loads(con.execute("SELECT receipt_json FROM c9_scan_receipt").fetchone()[0])
        start = his.PRICE_WINDOW_START
        # 1. Bulk location of identity filings -----------------------------------------------------
        out["identity_filings_by_group"] = _rows(con, """
            SELECT CASE WHEN form IN ('8-A12B','8-A12B/A') THEN '8-A12B' WHEN form IN ('8-A12G','8-A12G/A') THEN '8-A12G'
                        WHEN form LIKE 'CERT%' THEN 'CERT*' WHEN form IN ('25-NSE','25-NSE/A') THEN '25-NSE'
                        WHEN form IN ('25','25/A') THEN '25' ELSE '15*' END AS grp,
                   count(*) AS filing_rows, count(DISTINCT accession) AS accessions,
                   count(*) FILTER (WHERE form LIKE '25-NSE%' AND substr(accession,1,10) = cik) AS exchange_filer_copies,
                   count(DISTINCT cik) FILTER (WHERE NOT (form LIKE '25-NSE%' AND substr(accession,1,10) = cik)) AS ciks,
                   count(DISTINCT accession) FILTER (WHERE filing_date >= ?) AS accessions_in_price_window,
                   count(*) FILTER (WHERE primary_document LIKE '%.paper' OR primary_document IS NULL) AS paper_or_no_doc,
                   min(filing_date) AS first, max(filing_date) AS last, sum(size) AS bytes
            FROM c9_sec_filings GROUP BY 1 ORDER BY 1""", [start])
        out["identity_filings_by_year"] = _rows(con, """
            SELECT year(filing_date) AS year,
                   count(DISTINCT accession) FILTER (WHERE form IN ('8-A12B','8-A12G')) AS registrations,
                   count(DISTINCT accession) FILTER (WHERE form LIKE 'CERT%') AS certifications,
                   count(DISTINCT accession) FILTER (WHERE form = '25-NSE') AS exchange_removals,
                   count(DISTINCT accession) FILTER (WHERE form = '25') AS issuer_removals,
                   count(DISTINCT accession) FILTER (WHERE form IN ('25','25-NSE') AND (primary_document LIKE '%.paper' OR primary_document IS NULL)) AS removals_paper
            FROM c9_sec_filings WHERE filing_date >= DATE '1994-01-01' GROUP BY 1 ORDER BY 1""")
        out["removal_filers_in_window"] = _rows(con, """
            SELECT substr(f.accession,1,10) AS filer_cik, e.name AS filer_name, count(*) AS notices
            FROM c9_sec_filings f LEFT JOIN c9_sec_entities e ON e.cik = substr(f.accession,1,10)
            WHERE f.form = '25-NSE' AND f.filing_date >= ? GROUP BY ALL ORDER BY notices DESC LIMIT 15""", [start])
        out["issuers"] = _rows(con, """
            WITH r AS (SELECT cik, max(filing_date) AS last_removal FROM c9_sec_filings
                       WHERE form IN ('25','25-NSE') AND filing_date >= ?
                         AND NOT (form = '25-NSE' AND substr(accession,1,10) = cik) GROUP BY cik)
            SELECT count(*) AS issuers_with_removal_in_window,
                   count(*) FILTER (WHERE e.tickers_json = '[]') AS without_current_ticker,
                   count(*) FILTER (WHERE e.tickers_json = '[]' AND e.last_periodic <= r.last_removal + 400) AS stopped_reporting_within_13m
            FROM r JOIN c9_sec_entities e USING (cik)""", [start])[0]
        out["cert_name_symbols"] = _rows(con, """
            SELECT count(*) AS rows, count(DISTINCT cik) AS ciks, min(valid_from) AS first, max(valid_from) AS last
            FROM security_identity_evidence WHERE method = 'c9_cert_document_name_v1'""")[0]
        # 2. Fetch ledger ------------------------------------------------------------------------------
        ledger = fetcher.ledger()
        out["fetch"] = {
            "budget": fetcher.budget,
            "requests_used": len(ledger),
            "by_status": {str(s): sum(1 for r in ledger if r["status"] == s) for s in sorted({r["status"] for r in ledger})},
            "bytes_200": sum(r["bytes"] for r in ledger if r["status"] == 200),
            "first": ledger[0]["fetched_at"] if ledger else None,
            "last": ledger[-1]["fetched_at"] if ledger else None,
            "user_agent": his.USER_AGENT,
            "planned": con.execute("SELECT count(*) FROM c9_fetch_plan").fetchone()[0],
            "plan_by_stratum_form": _rows(con, "SELECT stratum, form, count(*) AS n FROM c9_fetch_plan GROUP BY ALL ORDER BY ALL"),
        }
        # 3. Parse outcomes --------------------------------------------------------------------------
        out["parse_outcomes"] = _rows(con, """
            SELECT form, status, count(*) AS docs FROM c9_parsed_documents GROUP BY ALL ORDER BY ALL""")
        out["class_types_parsed"] = _rows(con, """
            SELECT form, t.security_type, count(*) AS classes
            FROM c9_parsed_documents, unnest(CAST(json(security_types) AS VARCHAR[])) AS t(security_type)
            WHERE status = 'parsed' GROUP BY ALL ORDER BY ALL""")
        out["symbol_stated"] = _rows(con, """
            SELECT form, CASE WHEN filing_date < DATE '2006-08-01' THEN 'pre-2006-08' WHEN filing_date < DATE '2012-03-26'
                              THEN '2006-2012' WHEN filing_date < DATE '2019-01-01' THEN '2012-2018' ELSE '2019+' END AS era,
                   count(*) AS parsed_docs, count(*) FILTER (WHERE symbols <> '[]') AS with_symbol
            FROM c9_parsed_documents WHERE status = 'parsed' GROUP BY ALL ORDER BY ALL""")
        # 4. Evidence rows, audits, PIT --------------------------------------------------------------
        out["evidence_counts"] = _rows(con, """
            SELECT method, fact_kind, evidence_status, availability_status, count(*) AS rows, count(DISTINCT cik) AS ciks
            FROM security_identity_evidence GROUP BY ALL ORDER BY ALL""")
        out["audit_identity_evidence"] = audit_identity_evidence(con)
        out["acceptance_sql_b"] = [con.execute(sql).fetchone()[0] for sql in ACCEPTANCE_SQL_B]
        out["pit_checks"] = _rows(con, """
            SELECT
              count(*) FILTER (WHERE method = 'c9_filing_document_v1' AND evidence_status NOT IN ('unknown','conflicting')
                               AND available_at < CAST(CAST(json_extract_string(value_json,'$.filing_date') AS DATE) AS TIMESTAMP) + INTERVAL 46 HOUR)
                AS document_rows_before_fc1_floor,
              count(*) FILTER (WHERE available_at < source_published_at) AS available_before_published,
              count(*) FILTER (WHERE evidence_status IN ('unknown','conflicting') AND available_at IS NOT NULL) AS rejected_rows_linkable,
              count(*) FILTER (WHERE method = 'c9_filing_symbol_line_join_v1' AND evidence_status = 'reconstructed'
                               AND available_at IS NULL) AS accepted_line_links_without_clock,
              count(*) FILTER (WHERE method = 'c9_filing_symbol_line_join_v1' AND evidence_status = 'reconstructed'
                               AND available_at > CAST(valid_from AS TIMESTAMP)) AS line_links_known_after_span_start,
              count(*) FILTER (WHERE availability_status = 'verified') AS verified_availability_rows,
              count(*) FILTER (WHERE availability_status = 'modeled') AS modeled_availability_rows
            FROM security_identity_evidence""")[0]
        # 5. Line coverage -----------------------------------------------------------------------------
        con.execute("""
            CREATE OR REPLACE TEMP TABLE c9_line_status AS
            SELECT l.vendor_id, l.ceased, year(l.last_date) AS last_year, l.first_date, l.last_date,
                   count(e.evidence_id) FILTER (WHERE e.evidence_status = 'reconstructed') AS links,
                   count(e.evidence_id) FILTER (WHERE e.evidence_status = 'conflicting') AS conflicts,
                   bool_or(json_extract_string(e.value_json,'$.anchor_best_status') = 'verified_dated'
                           AND e.evidence_status = 'reconstructed') AS verified_anchor,
                   bool_or(json_extract_string(e.value_json,'$.terminal_notice_date') IS NOT NULL
                           AND e.evidence_status = 'reconstructed') AS terminal_aligned,
                   count(DISTINCT e.cik) FILTER (WHERE e.evidence_status = 'reconstructed') AS ciks
            FROM c9_vendor_lines l LEFT JOIN security_identity_evidence e
              ON e.method = 'c9_filing_symbol_line_join_v1' AND e.native_key = CAST(l.vendor_id AS VARCHAR)
            GROUP BY ALL""")
        out["line_coverage"] = _rows(con, """
            SELECT ceased, count(*) AS lines, count(*) FILTER (WHERE links > 0) AS linked,
                   count(*) FILTER (WHERE verified_anchor) AS linked_verified_anchor,
                   count(*) FILTER (WHERE terminal_aligned) AS linked_terminal_aligned,
                   count(*) FILTER (WHERE conflicts > 0 AND links = 0) AS conflict_only,
                   count(*) FILTER (WHERE ciks > 1) AS lines_with_multiple_issuers_over_time
            FROM c9_line_status GROUP BY ALL ORDER BY ALL""")
        out["line_link_rules"] = _rows(con, """
            SELECT evidence_status, coalesce(r.rule, '(none)') AS rule, count(*) AS links
            FROM security_identity_evidence e
            LEFT JOIN LATERAL (SELECT unnest(CAST(json_extract(e.value_json, '$.resolution_rules') AS VARCHAR[])) AS rule) r ON true
            WHERE e.method = 'c9_filing_symbol_line_join_v1' GROUP BY ALL ORDER BY ALL""")
        out["linked_ciks"] = _rows(con, """
            SELECT l.ceased, count(DISTINCT e.cik) AS ciks
            FROM security_identity_evidence e JOIN c9_vendor_lines l ON e.native_key = CAST(l.vendor_id AS VARCHAR)
            WHERE e.method = 'c9_filing_symbol_line_join_v1' AND e.evidence_status = 'reconstructed' GROUP BY ALL ORDER BY ALL""")
        out["uncovered_by_cessation_year"] = _rows(con, """
            SELECT last_year AS year, count(*) AS ceased_lines, count(*) FILTER (WHERE links > 0) AS linked,
                   count(*) FILTER (WHERE verified_anchor) AS verified_anchor, count(*) FILTER (WHERE terminal_aligned) AS terminal_aligned,
                   count(*) FILTER (WHERE links = 0) AS uncovered,
                   round(100.0 * count(*) FILTER (WHERE links = 0) / count(*), 1) AS uncovered_pct
            FROM c9_line_status WHERE ceased GROUP BY 1 ORDER BY 1""")
        # Sampled delisted issuers: what the documents alone deliver
        out["sample_issuers"] = _rows(con, """
            WITH s AS (SELECT DISTINCT cik FROM c9_fetch_plan WHERE stratum = 'issuer'),
            d AS (
              SELECT p.cik,
                     bool_or(p.form IN ('25','25-NSE') AND p.status = 'parsed') AS removal_parsed,
                     bool_or(p.form IN ('25','25-NSE') AND p.status = 'parsed' AND p.security_types LIKE '%common%') AS common_removal,
                     bool_or(p.form LIKE '8-A%' AND p.status = 'parsed') AS registration_parsed,
                     bool_or(p.symbols <> '[]') AS symbol_stated
              FROM c9_parsed_documents p JOIN s USING (cik) GROUP BY p.cik),
            k AS (SELECT DISTINCT cik FROM security_identity_evidence
                  WHERE method = 'c9_filing_symbol_line_join_v1' AND evidence_status = 'reconstructed'
                    AND json_extract_string(value_json,'$.anchor_best_status') = 'verified_dated')
            SELECT count(*) AS sampled, count(*) FILTER (WHERE d.removal_parsed) AS removal_parsed,
                   count(*) FILTER (WHERE d.common_removal) AS common_removal,
                   count(*) FILTER (WHERE d.registration_parsed) AS registration_parsed,
                   count(*) FILTER (WHERE d.symbol_stated) AS symbol_stated,
                   count(*) FILTER (WHERE k.cik IS NOT NULL) AS line_linked_verified_anchor
            FROM s LEFT JOIN d USING (cik) LEFT JOIN k USING (cik)""")[0]
        # 6. Precision probes (signals the join never reads) -------------------------------------
        # Active lines: does the linked issuer's CURRENT SEC ticker list (never read by the join) hold the
        # line's current symbol? Comparable = issuer has current tickers; disagreement = it has, but not this.
        out["probe_active_lines_current_ticker"] = _rows(con, """
            WITH links AS (
              SELECT e.cik, json_extract_string(e.value_json,'$.anchor_best_status') AS anchor,
                     json_extract_string(e.value_json,'$.anchors[0].method') AS anchor_method,
                     l.last_symbol,
                     list_transform(CAST(json(x.tickers_json) AS VARCHAR[]),
                                    t -> regexp_replace(upper(t), '[./\\s]+', '-', 'g')) AS sec_tickers
              FROM security_identity_evidence e JOIN c9_vendor_lines l ON e.native_key = CAST(l.vendor_id AS VARCHAR)
              JOIN c9_sec_entities x ON x.cik = e.cik
              WHERE e.method = 'c9_filing_symbol_line_join_v1' AND e.evidence_status = 'reconstructed'
                AND NOT l.ceased AND e.valid_to > DATE '2026-09-01')
            SELECT anchor, anchor_method, count(*) AS links,
                   count(*) FILTER (WHERE len(sec_tickers) > 0) AS comparable,
                   count(*) FILTER (WHERE list_contains(sec_tickers, last_symbol)) AS agree,
                   count(*) FILTER (WHERE len(sec_tickers) > 0 AND NOT list_contains(sec_tickers, last_symbol)) AS disagree
            FROM links GROUP BY ALL ORDER BY ALL""")
        out["probe_active_disagreements_sample"] = _rows(con, """
            SELECT e.cik, x.name, x.tickers_json, l.last_symbol, e.valid_from, e.valid_to,
                   json_extract_string(e.value_json,'$.anchors[0].method') AS anchor_method
            FROM security_identity_evidence e JOIN c9_vendor_lines l ON e.native_key = CAST(l.vendor_id AS VARCHAR)
            JOIN c9_sec_entities x ON x.cik = e.cik
            WHERE e.method = 'c9_filing_symbol_line_join_v1' AND e.evidence_status = 'reconstructed'
              AND NOT l.ceased AND e.valid_to > DATE '2026-09-01' AND x.tickers_json <> '[]'
              AND NOT list_contains(list_transform(CAST(json(x.tickers_json) AS VARCHAR[]),
                                    t -> regexp_replace(upper(t), '[./\\s]+', '-', 'g')), l.last_symbol)
            ORDER BY hash(e.evidence_id) LIMIT 25""")
        out["probe_terminal_alignment_vs_null"] = _rows(con, """
            WITH links AS (
              SELECT e.cik, l.vendor_id, l.last_date FROM security_identity_evidence e
              JOIN c9_vendor_lines l ON e.native_key = CAST(l.vendor_id AS VARCHAR)
              WHERE e.method = 'c9_filing_symbol_line_join_v1' AND e.evidence_status = 'reconstructed' AND l.ceased
                AND e.valid_to >= l.last_date),
            shuffled AS (
              SELECT a.cik, b.last_date FROM (SELECT *, row_number() OVER (ORDER BY hash(vendor_id)) AS i FROM links) a
              JOIN (SELECT *, row_number() OVER (ORDER BY hash(vendor_id * 7919 + 13)) AS i FROM links) b USING (i))
            SELECT 'observed' AS pairing, count(*) AS pairs,
                   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM c9_sec_filings f WHERE f.cik = links.cik AND f.form IN ('25','25-NSE','15-12B','15-12G','15-15D')
                                                  AND f.filing_date BETWEEN links.last_date - 7 AND links.last_date + 90)) AS removal_or_dereg_near_last_trade
            FROM links
            UNION ALL
            SELECT 'shuffled_null', count(*),
                   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM c9_sec_filings f WHERE f.cik = shuffled.cik AND f.form IN ('25','25-NSE','15-12B','15-12G','15-15D')
                                                  AND f.filing_date BETWEEN shuffled.last_date - 7 AND shuffled.last_date + 90))
            FROM shuffled""")
        # 7. Full acquisition volume -------------------------------------------------------------------
        out["removal_notices_only_in_exchange_listing"] = _rows(con, """
            WITH copies AS (SELECT accession, filing_date FROM c9_sec_filings
                            WHERE form LIKE '25-NSE%' AND substr(accession,1,10) = cik),
            issuer_side AS (SELECT DISTINCT accession FROM c9_sec_filings
                            WHERE form LIKE '25%' AND NOT (form LIKE '25-NSE%' AND substr(accession,1,10) = cik))
            SELECT count(*) AS exchange_filer_copies,
                   count(*) FILTER (WHERE accession NOT IN (SELECT accession FROM issuer_side)) AS only_in_exchange_listing,
                   count(*) FILTER (WHERE accession NOT IN (SELECT accession FROM issuer_side) AND filing_date >= ?) AS only_in_exchange_listing_in_window
            FROM copies""", [start])[0]
        out["full_acquisition"] = _rows(con, """
            WITH delisted AS (SELECT DISTINCT cik FROM c9_sec_filings WHERE form IN ('25','25-NSE') AND filing_date >= ?
                                AND NOT (form = '25-NSE' AND substr(accession,1,10) = cik)),
            docs AS (
              SELECT * FROM c9_sec_filings f
              WHERE f.primary_document IS NOT NULL AND f.primary_document NOT LIKE '%.paper'
                AND ((f.form IN ('25','25-NSE','25/A','25-NSE/A') AND f.filing_date >= ?)
                     OR (f.form IN ('8-A12B','8-A12G','8-A12B/A','8-A12G/A') AND f.cik IN (SELECT cik FROM delisted)))
              QUALIFY row_number() OVER (PARTITION BY f.accession ORDER BY substr(f.accession,1,10) = f.cik, f.cik) = 1)
            SELECT CASE WHEN form LIKE '25%' THEN 'removal notices in price window' ELSE '8-A of issuers with a removal in window' END AS scope,
                   count(*) AS documents, count(DISTINCT cik) AS ciks, sum(size) AS submission_bytes,
                   round(count(*) / 4.0 / 3600, 2) AS hours_at_4_req_s
            FROM docs GROUP BY 1 ORDER BY 1""", [start, start])
    finally:
        con.close()
    text = json.dumps(out, indent=1, default=str)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    log(f"measure: {len(text):,} chars -> {args.out or 'stdout'}")
    if not args.out:
        print(text)


def stage_sheet(args: argparse.Namespace) -> None:
    """Hand-verification sheet: parsed fields next to the raw cover excerpt, for a random sample."""
    fetcher = _fetcher(args)
    con = connect(Path(args.db))
    try:
        # Sample A (seed 7) was used to find parser errors; a later sample excludes it so its
        # precision is measured on documents the fixes were not designed against.
        sample = con.execute(
            """SELECT priority, form, url, status, issuer_name, exchange_name, mic, class_titles, security_types, symbols,
                      rule_provision FROM c9_parsed_documents
               WHERE status <> 'http_404' AND (? = 7 OR priority NOT IN (
                   SELECT priority FROM c9_parsed_documents ORDER BY hash(priority * 31 + 7) LIMIT 60))
                 AND priority NOT IN (SELECT unnest(?::BIGINT[]))
               ORDER BY hash(priority * 31 + ?) LIMIT ?""",
            [args.sheet_seed, [int(p) for p in args.sheet_exclude.split(",") if p], args.sheet_seed, args.sheet_size],
        ).fetchall()
    finally:
        con.close()
    lines: list[str] = []
    for priority, form, url, status, issuer, exchange, mic, titles, types, symbols, rule in sample:
        response = fetcher.cached(url)
        raw = his.document_text(response.read()) if response and response.status == 200 else f"<http {response.status if response else 'n/a'}>"
        flat = " ".join(raw.split())
        anchor = next((m for m in ("Title of each class", "Section 12(g)", "descriptionClassSecurity", "Description of class",
                                   "Exact name") if m.lower() in flat.lower()), None)
        at = flat.lower().find(anchor.lower()) if anchor else 0
        excerpt = flat[max(at - 700, 0) : at + 900]
        lines.append(f"### p{priority} {form} {status} {url}\nPARSED issuer={issuer!r} exchange={exchange!r} mic={mic} "
                     f"classes={titles} types={types} symbols={symbols} rule={rule}\nRAW {excerpt}\n")
    Path(args.out).write_text("\n".join(lines), encoding="utf-8")
    log(f"sheet: {len(sample)} documents -> {args.out}")


def stage_linksheet(args: argparse.Namespace) -> None:
    """Hand-verification sheet for accepted links on ceased lines: issuer names vs symbols vs dates."""
    con = connect(Path(args.db))
    try:
        rows = _rows(con, """
            SELECT e.native_key AS vendor_id, json_extract(e.value_json,'$.symbols') AS symbols, e.valid_from, e.valid_to,
                   l.first_date AS line_first, l.last_date AS line_last, e.cik, x.name AS current_name,
                   (SELECT string_agg(n.name || ' [' || coalesce(n.from_raw[:10],'?') || '..' || coalesce(n.to_raw[:10],'?') || ']', '; ')
                    FROM c9_sec_former_names n WHERE n.cik = e.cik) AS former_names,
                   json_extract_string(e.value_json,'$.anchors[0].method') AS anchor_method,
                   json_extract_string(e.value_json,'$.anchors[0].date') AS anchor_date,
                   json_extract_string(e.value_json,'$.terminal_notice_date') AS terminal_notice,
                   x.last_periodic, json_extract(e.value_json,'$.resolution_rules') AS rules
            FROM security_identity_evidence e JOIN c9_vendor_lines l ON e.native_key = CAST(l.vendor_id AS VARCHAR)
            JOIN c9_sec_entities x ON x.cik = e.cik
            WHERE e.method = 'c9_filing_symbol_line_join_v1' AND e.evidence_status = 'reconstructed' AND l.ceased
            ORDER BY hash(e.evidence_id) LIMIT ?""", [args.sheet_size])
    finally:
        con.close()
    Path(args.out).write_text("\n".join(json.dumps(row, default=str) for row in rows), encoding="utf-8")
    log(f"linksheet: {len(rows)} links -> {args.out}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", required=True, help="scratch DuckDB path (never the warehouse)")
    parser.add_argument("--stages", default=",".join(STAGES))
    parser.add_argument("--template", help="built schema template to copy at bootstrap")
    parser.add_argument("--submissions", default=str(DEFAULT_SUBMISSIONS))
    parser.add_argument("--parquet", default=str(DEFAULT_PARQUET))
    parser.add_argument("--cache-dir", default=str(DEFAULT_CACHE_DIR))
    parser.add_argument("--limit", type=int, default=None, help="scan: first N main members only")
    parser.add_argument("--batch", type=int, default=20_000)
    parser.add_argument("--out", help="measure: JSON output path")
    parser.add_argument("--sheet-size", type=int, default=60, help="sheet: documents in the verification sample")
    parser.add_argument("--max-attempts", type=int, default=2, help="fetch: transient failures before a URL is skipped")
    parser.add_argument("--sheet-seed", type=int, default=7, help="sheet: sample seed (7 = design sample A)")
    parser.add_argument("--sheet-exclude", default="", help="sheet: comma-separated priorities already verified")
    parser.add_argument("--issuers", type=int, default=110, help="plan: delisted issuers sampled")
    parser.add_argument("--era-documents", type=int, default=50, help="plan: pre-window documents sampled")
    parser.add_argument(
        "--parquet-sha256",
        default="0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae",
        help="retained TickerHistory3 pin (L2 §2)",
    )
    args = parser.parse_args(argv)
    handlers = {name: globals().get(f"stage_{name}") for name in STAGES}
    for name in [stage.strip() for stage in args.stages.split(",") if stage.strip()]:
        handler = handlers.get(name)
        if handler is None:
            raise SystemExit(f"unknown or unimplemented stage {name!r}")
        started = time.monotonic()
        handler(args)
        log(f"stage {name} finished in {time.monotonic() - started:,.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
