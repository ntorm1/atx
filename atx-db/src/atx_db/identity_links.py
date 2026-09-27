"""Permanent line identities and clocked company links (0331, part 1).

The retained-input rehearsal is independent of the production warehouse. It
keeps the original archive receipt clocks and labels reconstruction explicitly.
Positive vendor ids are the permanent security ids in allocation version 1;
the natural line key is always TBLTICKERHISTORY-<vendor id>, never a ticker.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import time
from collections import defaultdict
from collections.abc import Mapping, Sequence
from pathlib import Path

import duckdb

_LOADED_CODE_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
PREPARATION_VERSION = "identity_retained_inputs_v1"
ALLOCATION_VERSION = "positive_vendor_id_v1_cik_company_v1"
STAGE_NAME = "identity_links_rebuild"
SCHEMA_VERSION = "identity_links_v1"
TABLE_COLUMNS = {
    "security_permanent_ids": "perm_security_id BIGINT NOT NULL, security_id VARCHAR NOT NULL, first_trade_date DATE, last_trade_date DATE, created_at TIMESTAMP NOT NULL",
    "company_permanent_ids": "perm_company_id BIGINT NOT NULL, cik VARCHAR, basis VARCHAR NOT NULL, created_at TIMESTAMP NOT NULL",
    "security_company_links": "perm_security_id BIGINT NOT NULL, perm_company_id BIGINT NOT NULL, cik VARCHAR, link_start DATE NOT NULL, link_end DATE, link_basis VARCHAR NOT NULL, link_primary VARCHAR NOT NULL, tier VARCHAR, available_at TIMESTAMP NOT NULL, evidence_ids VARCHAR, created_at TIMESTAMP NOT NULL, valid_until TIMESTAMP, class_status VARCHAR NOT NULL",
    "identity_unresolved_memberships": "membership_id VARCHAR NOT NULL, perm_security_id BIGINT, perm_company_id BIGINT NOT NULL, cik VARCHAR NOT NULL, link_start DATE NOT NULL, link_end DATE, available_at TIMESTAMP NOT NULL, valid_until TIMESTAMP, evidence_ids VARCHAR NOT NULL, reason VARCHAR NOT NULL",
    "security_names_history": "perm_security_id BIGINT NOT NULL, ticker VARCHAR, issuer_name VARCHAR, venue VARCHAR, valid_from DATE NOT NULL, valid_to DATE, source VARCHAR NOT NULL, available_at TIMESTAMP NOT NULL",
    "identity_link_evidence": "evidence_id VARCHAR NOT NULL, perm_security_id BIGINT, cik VARCHAR, source_file VARCHAR NOT NULL, source_sha256 VARCHAR NOT NULL, assertion VARCHAR NOT NULL, valid_from DATE, valid_to DATE, available_at TIMESTAMP NOT NULL, observed_at TIMESTAMP, evidence_kind VARCHAR NOT NULL, superseded_id VARCHAR, availability_basis VARCHAR NOT NULL",
}
TABLE_KEYS = {
    "security_permanent_ids": ("perm_security_id",),
    "company_permanent_ids": ("perm_company_id",),
    "security_company_links": ("perm_security_id", "perm_company_id", "link_start", "available_at"),
    "identity_unresolved_memberships": ("membership_id",),
    "security_names_history": ("perm_security_id", "valid_from", "source", "available_at", "ticker", "issuer_name"),
    "identity_link_evidence": ("evidence_id",),
}


def create_identity_tables(con: duckdb.DuckDBPyConnection, *, suffix: str = "") -> None:
    if suffix not in ("", "__next"):
        raise ValueError("unexpected identity table suffix")
    for table, columns in TABLE_COLUMNS.items():
        con.execute(f"CREATE TABLE IF NOT EXISTS {table}{suffix} ({columns})")


def security_master_asof_sql(cutoff_sql: str, *, line_predicate: str = "TRUE") -> str:
    """Public snapshot at one trusted SQL clock; no current-name backcasting.

    The publication adapter supplies a quoted TIMESTAMP literal. The historical
    view uses a correlated event clock and permanent line predicate. NULL fields
    are deliberate where no dated public evidence exists (including LEI/FIGI).
    """
    date_sql = f"CAST({cutoff_sql} AS DATE)"
    return f"""WITH spine AS (
        SELECT * FROM security_permanent_ids p WHERE {line_predicate}
          AND CAST(p.first_trade_date AS TIMESTAMP) + INTERVAL 22 HOUR <= {cutoff_sql}
    ), all_links AS (
        SELECT l.* FROM security_company_links l JOIN spine USING (perm_security_id)
        WHERE l.available_at <= {cutoff_sql} AND (l.valid_until IS NULL OR {cutoff_sql} < l.valid_until)
          AND l.link_start <= {date_sql} AND (l.link_end IS NULL OR l.link_end >= {date_sql})
    ), links AS (
        SELECT * FROM all_links WHERE perm_security_id IN (
            SELECT perm_security_id FROM all_links GROUP BY perm_security_id
            HAVING count(DISTINCT perm_company_id) = 1)
          AND (tier IN ('high','medium') OR link_basis IN ('strict_dated','current_ticker_verified'))
    ), owner AS (
        SELECT perm_security_id,
          CASE WHEN count(DISTINCT perm_company_id) = 1 THEN max(perm_company_id) END AS perm_company_id,
          CASE WHEN count(DISTINCT cik) = 1 THEN max(cik) END AS cik,
          CASE WHEN count(*) = 1 THEN max(link_basis) ELSE 'ambiguous' END AS link_basis,
          CASE WHEN count(*) = 1 THEN max(tier) END AS tier,
          max(available_at) AS available_at FROM links GROUP BY perm_security_id
    ), names AS (
        SELECT n.* FROM security_names_history n JOIN spine USING (perm_security_id)
        WHERE n.available_at <= {cutoff_sql} AND n.valid_from <= {date_sql}
          AND (n.valid_to IS NULL OR n.valid_to >= {date_sql})
    ), ticker AS (
        SELECT * FROM names WHERE ticker IS NOT NULL QUALIFY dense_rank() OVER
          (PARTITION BY perm_security_id ORDER BY available_at DESC) = 1
    ), issuer AS (
        SELECT * FROM names WHERE issuer_name IS NOT NULL QUALIFY dense_rank() OVER
          (PARTITION BY perm_security_id ORDER BY available_at DESC) = 1
    ), venue AS (
        SELECT * FROM names WHERE venue IS NOT NULL QUALIFY dense_rank() OVER
          (PARTITION BY perm_security_id ORDER BY available_at DESC) = 1
    ) SELECT p.security_id, p.perm_security_id, o.perm_company_id,
        CASE WHEN o.cik IS NOT NULL THEN 'CIK-' || o.cik END AS entity_id,
        CASE WHEN o.cik IS NOT NULL THEN 'CIK-' || o.cik END AS issuer_id,
        (SELECT CASE WHEN count(DISTINCT ticker) = 1 THEN max(ticker) END FROM ticker t
          WHERE t.perm_security_id = p.perm_security_id) AS primary_symbol,
        (SELECT CASE WHEN count(DISTINCT issuer_name) = 1 THEN max(issuer_name) END FROM issuer n
          WHERE n.perm_security_id = p.perm_security_id) AS name,
        (SELECT CASE WHEN count(DISTINCT venue) = 1 THEN max(venue) END FROM venue v
          WHERE v.perm_security_id = p.perm_security_id) AS venue,
        CAST(NULL AS VARCHAR) AS asset_class, CAST(NULL AS VARCHAR) AS country,
        CAST(NULL AS VARCHAR) AS currency,
        p.first_trade_date <= {date_sql} AND p.last_trade_date >= {date_sql} AS active,
        o.cik, CAST(NULL AS VARCHAR) AS lei, CAST(NULL AS VARCHAR) AS figi,
        o.link_basis, o.tier, {date_sql} AS as_of_date, {cutoff_sql} AS available_at,
        'identity_history_reconstructed_v1' AS source, CAST(NULL AS VARCHAR) AS run_id,
        p.created_at AS source_loaded_at
      FROM spine p LEFT JOIN owner o USING (perm_security_id)"""


def create_security_master_public_view(con):
    """Event-keyed history; set joins avoid correlated DuckDB binder failures."""
    con.execute("""CREATE OR REPLACE VIEW v_security_master_public AS
    WITH events AS (
        SELECT perm_security_id, CAST(first_trade_date AS TIMESTAMP) + INTERVAL 22 HOUR AS event_at FROM security_permanent_ids
        UNION SELECT perm_security_id, CAST(last_trade_date AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 22 HOUR FROM security_permanent_ids
        UNION SELECT perm_security_id, available_at FROM security_company_links
        UNION SELECT perm_security_id, valid_until FROM security_company_links WHERE valid_until IS NOT NULL
        UNION SELECT perm_security_id, greatest(available_at,CAST(link_start AS TIMESTAMP) + INTERVAL 22 HOUR) FROM security_company_links
        UNION SELECT perm_security_id, greatest(available_at,CAST(link_end AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 22 HOUR) FROM security_company_links WHERE link_end IS NOT NULL
        UNION SELECT perm_security_id, available_at FROM security_names_history
        UNION SELECT perm_security_id, greatest(available_at,CAST(valid_from AS TIMESTAMP) + INTERVAL 22 HOUR) FROM security_names_history
        UNION SELECT perm_security_id, greatest(available_at,CAST(valid_to AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 22 HOUR) FROM security_names_history WHERE valid_to IS NOT NULL
    ), spine AS (
        SELECT p.*,e.event_at FROM events e JOIN security_permanent_ids p USING(perm_security_id)
        WHERE CAST(p.first_trade_date AS TIMESTAMP) + INTERVAL 22 HOUR <= e.event_at
    ), all_links AS (
        SELECT e.perm_security_id,e.event_at,l.perm_company_id,l.cik,l.link_basis,l.tier,
            (l.tier IN ('high','medium') OR l.link_basis IN ('strict_dated','current_ticker_verified')) AS eligible
        FROM spine e JOIN security_company_links l ON l.perm_security_id=e.perm_security_id
         AND l.available_at<=e.event_at AND (l.valid_until IS NULL OR e.event_at<l.valid_until)
         AND l.link_start<=CAST(e.event_at AS DATE) AND (l.link_end IS NULL OR l.link_end>=CAST(e.event_at AS DATE))
    ), owners AS (
        SELECT perm_security_id,event_at,
            CASE WHEN count(DISTINCT perm_company_id)=1 THEN max(perm_company_id) FILTER(WHERE eligible) END AS perm_company_id,
            CASE WHEN count(DISTINCT perm_company_id)=1 THEN max(cik) FILTER(WHERE eligible) END AS cik,
            CASE WHEN count(DISTINCT perm_company_id)>1 THEN 'ambiguous' ELSE max(link_basis) FILTER(WHERE eligible) END AS link_basis,
            CASE WHEN count(DISTINCT perm_company_id)=1 THEN max(tier) FILTER(WHERE eligible) END AS tier
        FROM all_links GROUP BY perm_security_id,event_at
    ), names AS (
        SELECT e.perm_security_id,e.event_at,n.ticker,n.issuer_name,n.venue,n.available_at
        FROM spine e JOIN security_names_history n ON n.perm_security_id=e.perm_security_id
         AND n.available_at<=e.event_at AND n.valid_from<=CAST(e.event_at AS DATE)
         AND (n.valid_to IS NULL OR n.valid_to>=CAST(e.event_at AS DATE))
    ), tickers AS (
        SELECT * FROM names WHERE ticker IS NOT NULL QUALIFY dense_rank() OVER
            (PARTITION BY perm_security_id,event_at ORDER BY available_at DESC)=1
    ), issuers AS (
        SELECT * FROM names WHERE issuer_name IS NOT NULL QUALIFY dense_rank() OVER
            (PARTITION BY perm_security_id,event_at ORDER BY available_at DESC)=1
    ), venues AS (
        SELECT * FROM names WHERE venue IS NOT NULL QUALIFY dense_rank() OVER
            (PARTITION BY perm_security_id,event_at ORDER BY available_at DESC)=1
    ), ticker AS (
        SELECT perm_security_id,event_at,CASE WHEN count(DISTINCT ticker)=1 THEN max(ticker) END AS primary_symbol
        FROM tickers GROUP BY perm_security_id,event_at
    ), issuer AS (
        SELECT perm_security_id,event_at,CASE WHEN count(DISTINCT issuer_name)=1 THEN max(issuer_name) END AS name
        FROM issuers GROUP BY perm_security_id,event_at
    ), venue AS (
        SELECT perm_security_id,event_at,CASE WHEN count(DISTINCT venue)=1 THEN max(venue) END AS venue
        FROM venues GROUP BY perm_security_id,event_at
    ) SELECT p.security_id,p.perm_security_id,o.perm_company_id,
        CASE WHEN o.cik IS NOT NULL THEN 'CIK-' || o.cik END AS entity_id,
        CASE WHEN o.cik IS NOT NULL THEN 'CIK-' || o.cik END AS issuer_id,
        t.primary_symbol,n.name,v.venue,CAST(NULL AS VARCHAR) AS asset_class,
        CAST(NULL AS VARCHAR) AS country,CAST(NULL AS VARCHAR) AS currency,
        p.first_trade_date<=CAST(p.event_at AS DATE) AND p.last_trade_date>=CAST(p.event_at AS DATE) AS active,
        o.cik,CAST(NULL AS VARCHAR) AS lei,CAST(NULL AS VARCHAR) AS figi,o.link_basis,o.tier,
        CAST(p.event_at AS DATE) AS as_of_date,p.event_at AS available_at,
        'identity_history_reconstructed_v1' AS source,CAST(NULL AS VARCHAR) AS run_id,p.created_at AS source_loaded_at
    FROM spine p LEFT JOIN owners o USING(perm_security_id,event_at)
    LEFT JOIN ticker t USING(perm_security_id,event_at)
    LEFT JOIN issuer n USING(perm_security_id,event_at)
    LEFT JOIN venue v USING(perm_security_id,event_at)""")


def _clock(value: dt.datetime) -> dt.datetime:
    if not isinstance(value, dt.datetime):
        raise TypeError("cutoff must be datetime")
    return value.astimezone(dt.UTC).replace(tzinfo=None) if value.tzinfo else value


def links_asof_sql(cutoff: dt.datetime, tiers: Sequence[str] = ("high", "medium")) -> str:
    """Business-valid links at cutoff, with the tier/primary then in force.

    Evidence versions are half-open; business end dates are inclusive. Filtering
    tiers occurs after selecting the applicable evidence version, so a later low
    tier cannot resurrect an older high tier. P/J are already clocked snapshots.
    """
    if tuple(tiers) not in (("high", "medium"), ("high",)):
        raise ValueError("tiers must be ('high', 'medium') or ('high',)")
    tier_sql = ",".join(_literal(tier) for tier in tiers)
    visible = (f"SELECT * EXCLUDE(company_count) FROM (SELECT *, count(DISTINCT perm_company_id) "
               f"OVER (PARTITION BY perm_security_id) AS company_count FROM ({_visible_links_sql(cutoff)})) "
               f"WHERE company_count = 1 AND (tier IN ({tier_sql}) OR link_basis IN ('strict_dated','current_ticker_verified'))")
    return (f"WITH visible AS ({visible}) SELECT * EXCLUDE (link_primary), CASE WHEN link_primary = 'N' THEN 'N' "
            "WHEN perm_security_id = min(perm_security_id) FILTER (WHERE link_primary IN ('P','J')) "
            "OVER (PARTITION BY perm_company_id) THEN 'P' ELSE 'J' END AS link_primary FROM visible")


def _visible_links_sql(cutoff: dt.datetime, table: str = "security_company_links") -> str:
    instant = "TIMESTAMP " + _literal(_clock(cutoff).isoformat(sep=" "))
    return (f"SELECT * FROM {table} WHERE available_at <= {instant} "
            f"AND (valid_until IS NULL OR {instant} < valid_until) "
            f"AND link_start <= CAST({instant} AS DATE) "
            f"AND (link_end IS NULL OR link_end >= CAST({instant} AS DATE))")


def company_me_sql(cutoff: dt.datetime, *, line_me_relation: str = "line_me",
                   tiers: Sequence[str] = ("high", "medium")) -> str:
    """Company ME only when all then-known possible common members are covered.

    The caller's bounded relation has perm_security_id, me_line, available_at.
    It must contain one row per line for the pinned formation session. Membership
    is selected BEFORE tier eligibility. Low/unknown/conflicting members cannot
    disappear from the denominator when a high-only sensitivity is requested.
    Positive non-common proof is the only basis for excluding a known member.
    """
    if not line_me_relation.replace("_", "").isalnum():
        raise ValueError("line_me_relation must be a SQL identifier")
    instant = "TIMESTAMP " + _literal(_clock(cutoff).isoformat(sep=" "))
    return f"""WITH eligible AS ({links_asof_sql(cutoff, tiers)}), members AS (
        {_visible_links_sql(cutoff)}
    ), unresolved AS (
        SELECT perm_company_id, cik, count(*) AS unresolved_memberships,
               max(available_at) AS available_at
        FROM ({_visible_links_sql(cutoff, 'identity_unresolved_memberships')})
        GROUP BY perm_company_id, cik
    ), prices AS (
        SELECT perm_security_id, CASE WHEN count(*) = 1 THEN max(me_line) END AS me_line,
               max(available_at) AS available_at FROM {line_me_relation} GROUP BY perm_security_id
    ), totals AS (
        SELECT l.perm_company_id, l.cik,
            count(*) FILTER (WHERE l.class_status = 'common') AS expected_common_lines,
            count(*) FILTER (WHERE l.class_status = 'unknown') AS unknown_class_lines,
            count(e.perm_security_id) FILTER (WHERE l.class_status = 'common') AS eligible_common_lines,
            count(m.me_line) FILTER (WHERE l.class_status = 'common' AND e.perm_security_id IS NOT NULL
                AND m.me_line > 0 AND isfinite(m.me_line) AND m.available_at <= {instant}) AS priced_common_lines,
            sum(m.me_line) FILTER (WHERE l.class_status = 'common') AS total_me,
            greatest(max(l.available_at), max(m.available_at) FILTER (WHERE m.available_at <= {instant})) AS available_at
        FROM members l LEFT JOIN eligible e USING (perm_security_id, perm_company_id)
        LEFT JOIN prices m USING (perm_security_id)
        GROUP BY l.perm_company_id, l.cik
    ), counts AS (
        SELECT coalesce(t.perm_company_id,u.perm_company_id) AS perm_company_id,
            coalesce(t.cik,u.cik) AS cik, coalesce(expected_common_lines,0) AS expected_common_lines,
            coalesce(unknown_class_lines,0) AS unknown_class_lines,
            coalesce(eligible_common_lines,0) AS eligible_common_lines,
            coalesce(priced_common_lines,0) AS priced_common_lines,
            coalesce(unresolved_memberships,0) AS unresolved_memberships, total_me,
            greatest(t.available_at,u.available_at) AS available_at
        FROM totals t FULL JOIN unresolved u USING (perm_company_id,cik)
    ) SELECT * EXCLUDE(total_me), CASE WHEN unknown_class_lines = 0 AND unresolved_memberships = 0
            AND expected_common_lines > 0 AND eligible_common_lines = expected_common_lines
            AND priced_common_lines = expected_common_lines THEN total_me END AS me_company,
        CASE WHEN unknown_class_lines > 0 OR unresolved_memberships > 0 THEN 'uncertain_class_membership'
             WHEN expected_common_lines = 0 THEN 'no_common_class'
             WHEN eligible_common_lines < expected_common_lines THEN 'ineligible_common_class'
             WHEN priced_common_lines < expected_common_lines THEN 'incomplete_line_me'
             ELSE 'complete' END AS me_status FROM counts"""


def _merge_candidate_evidence(left: Mapping[str, object], right: Mapping[str, object]) -> dict[str, object]:
    """Union provenance only when every candidate semantic field is identical."""
    if ({key: value for key, value in left.items() if key != "evidence_ids"} !=
            {key: value for key, value in right.items() if key != "evidence_ids"}):
        raise ValueError(f"conflicting identity assertion {right['assertion_id']} at {right['available_at']}")
    return {**right, "evidence_ids": json.dumps(sorted(
        set(json.loads(left["evidence_ids"])) | set(json.loads(right["evidence_ids"]))))}


def _company_versions(candidates: Sequence[Mapping[str, object]], created_at: dt.datetime) -> list[tuple]:
    """Sweep evidence clocks and business intervals without future primary choices.

    Candidates are one company's assertion/tier events. A version is a complete
    state for this company between two event clocks. The lowest eligible common
    permanent id is P; the rest are J; non-common/unknown classes remain N.
    """
    if not candidates:
        return []
    if len({row["cik"] for row in candidates}) != 1:
        raise ValueError("a company slice must have exactly one CIK")
    clocks = sorted({row["available_at"] for row in candidates})
    output = []
    for clock_index, at in enumerate(clocks):
        until = clocks[clock_index + 1] if clock_index + 1 < len(clocks) else None
        latest = {}
        for row in candidates:
            if row["available_at"] <= at:
                key = row["assertion_id"]
                if key not in latest or latest[key]["available_at"] < row["available_at"]:
                    latest[key] = row
                elif latest[key]["available_at"] == row["available_at"] and latest[key] != row:
                    latest[key] = _merge_candidate_evidence(latest[key], row)
        active = list(latest.values())
        boundaries = sorted({row["link_start"] for row in active} | {
            row["link_end"] + dt.timedelta(days=1) for row in active if row["link_end"] is not None})
        for index, start in enumerate(boundaries):
            end = boundaries[index + 1] - dt.timedelta(days=1) if index + 1 < len(boundaries) else None
            spanning = [row for row in active if row["link_start"] <= start and (row["link_end"] is None or row["link_end"] >= start)]
            by_line = defaultdict(list)
            for row in spanning:
                by_line[row["perm_security_id"]].append(row)
            chosen = []
            for rows in by_line.values():
                # Independent corroborating assertions agree on CIK; select a
                # deterministic supported identity basis. Class evidence follows
                # its own newest clock; an old strong identity must not overrule
                # a later observed class change or class uncertainty.
                priority = {"strict_dated": 4, "reconstructed_high": 3,
                            "reconstructed_document_symbol_continuity": 2.5, "reconstructed_medium": 2,
                            "reconstructed_low": 1, "current_ticker_verified": 0}
                identity = max(rows, key=lambda row: (priority[row["link_basis"]], row["available_at"], row["evidence_ids"]))
                # A later share-count inference describes issuer common shares,
                # not necessarily this traded class (e.g. an explicitly proven
                # ADR or fund). Prefer direct class observations in the bounded
                # business segment; their newest unknown still fails closed.
                observed_classes = [row for row in rows if row["link_basis"] in
                    ("reconstructed_document_symbol_continuity", "current_ticker_verified")]
                class_sources = observed_classes or rows
                class_clock = max(row["available_at"] for row in class_sources)
                class_rows = [row for row in class_sources if row["available_at"] == class_clock]
                statuses = {row["class_status"] for row in class_rows}
                status = next(iter(statuses)) if len(statuses) == 1 else "unknown"
                evidence = {eid for row in (identity, *class_rows) for eid in json.loads(row["evidence_ids"])}
                chosen.append({**identity, "class_status": status, "is_common": status == "common",
                               "evidence_ids": json.dumps(sorted(evidence))})
            common = [row["perm_security_id"] for row in chosen if row["is_common"] and row["tier"] in (None, "high", "medium")]
            primary = min(common) if common else None
            for row in chosen:
                role = "N" if row["perm_security_id"] not in common else ("P" if row["perm_security_id"] == primary else "J")
                output.append((row["perm_security_id"], int(row["cik"]), row["cik"], start, end,
                    row["link_basis"], role, row["tier"], at, row["evidence_ids"], created_at, until,
                    row["class_status"]))
    return output


def _insert_rows(con, table: str, rows: Sequence[Sequence[object]], batch_size: int = 1000) -> None:
    if not rows:
        return
    placeholders = ",".join("?" for _ in rows[0])
    for start in range(0, len(rows), batch_size):
        con.execute("BEGIN")
        try:
            con.executemany(f"INSERT INTO {table} VALUES ({placeholders})", rows[start:start + batch_size])
            con.execute("COMMIT")
        except BaseException:
            try:
                con.execute("ROLLBACK")
            except duckdb.Error:
                pass  # A failed post-commit checkpoint may already be durable.
            raise


def _row_dicts(con, sql: str) -> list[dict[str, object]]:
    cursor = con.execute(sql)
    names = [column[0] for column in cursor.description]
    return [dict(zip(names, row, strict=True)) for row in cursor.fetchall()]


def _evidence_rows(con, files):
    """Each retained file contains at most 250 lines; never retain all JSON rows."""
    for path in files:
        yield from _row_dicts(con, f"SELECT * FROM read_parquet({_literal(path.as_posix())}) ORDER BY evidence_id")


def _event_id(*values: object) -> str:
    return hashlib.sha256(json.dumps(values, default=str, separators=(",", ":")).encode()).hexdigest()


def _document_review_pin(path: Path | None) -> dict[str, object] | None:
    """Pin the human-reviewed assertion set and actual primary bytes, including unknowns."""
    if path is None:
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("version") != "identity_dated_document_review_v1":
        raise ValueError("unsupported dated document review version")
    files = {}
    for row in payload["assertions"]:
        source = Path(row["document_path"]).resolve()
        sha = row["document_sha256"]
        if str(source) in files and files[str(source)] != sha:
            raise ValueError("conflicting primary document pins")
        if str(source) not in files and file_sha256(source) != sha:
            raise ValueError(f"reviewed primary document changed: {source}")
        files[str(source)] = sha
    return {"path": str(path.resolve()), "sha256": file_sha256(path), "documents": files}


def _reviewed_document_events(path: Path, vendor, snapshot_clock: dt.datetime):
    """C110: a dated point supports reconstructed forward continuity in one island.

    Unknown/joint/peer reviews remain evidence only. A confirmed own-symbol point
    with explicitly unknown class creates an unknown class event; it cannot reuse
    a previous common classification or resolve raw-share uncertainty.
    """
    from .historical_identity_sources import filing_clock

    _document_review_pin(path)  # Validate bytes immediately before consumption.
    payload = json.loads(path.read_text(encoding="utf-8"))
    lines = {line.vendor_id: line for line in vendor}
    seen = {}
    for review in payload["assertions"]:
        vid, cik = int(review["vendor_id"]), str(review["cik"])
        if vid not in lines or len(cik) != 10 or not cik.isdigit():
            raise ValueError("document review must identify a retained line and normalized CIK")
        filed = dt.date.fromisoformat(review["filing_date"])
        symbol = str(review["symbol"]).strip().upper()
        clock = filing_clock(review.get("accepted_raw"), filed)
        observed = _clock(dt.datetime.fromisoformat(review["fetched_at"].replace("Z", "+00:00")))
        status = review.get("class_status") or "unknown"
        if status not in ("common", "non_common", "unknown"):
            raise ValueError("invalid reviewed document class")
        matches = [(ticker, first, last) for ticker, first, last in lines[vid].symbols
                   if ticker == symbol and first <= filed <= last]
        confirmed = review.get("review_status") == "confirmed" and review.get("scope") == "own_issuer_exact_symbol_class"
        if confirmed and (len(matches) != 1 or not review.get("review_basis") or
                          (status != "unknown" and not review.get("class_title"))):
            raise ValueError("confirmed document needs one exact dated vendor symbol island and reviewed class proof")
        island = matches[0] if len(matches) == 1 else None
        at = max(clock.available_at, dt.datetime.combine(island[1], dt.time(22))) if island and clock.available_at else observed
        if at > snapshot_clock:
            # The source snapshot bounds the historical rehearsal. Retain the
            # review in the pinned manifest, but never inject later knowledge.
            continue
        first, last = (filed, island[2]) if island else (filed, filed)
        event_id = _event_id("reviewed_dated_document", review, at)
        assertion = {**review, "class_status": status, "filing_published_at": clock.published_at,
            "filing_available_at": clock.available_at, "availability_status": clock.availability_status,
            "stamp_status": clock.stamp_status, "vendor_symbol_island": island,
            "point_only_verified": confirmed and status != "unknown",
            "continuity": "reconstructed_from_point_within_exact_vendor_symbol_island"}
        evidence = (event_id, vid, cik, str(Path(review["document_path"]).resolve()), review["document_sha256"],
            json.dumps(assertion, sort_keys=True, default=str), first, last, at, observed,
            "reviewed_dated_document", None, "filing_clock_and_dated_vendor_island" if confirmed else "unresolved_document_review")
        signature = json.dumps(assertion, sort_keys=True, default=str)
        if event_id in seen:
            if seen[event_id] != signature:
                raise ValueError("conflicting dated document event")
            continue
        seen[event_id] = signature
        candidate = document = None
        if confirmed and island and clock.available_at is not None and first <= last:
            # A later dated point does not retract the earlier supported business
            # segment. Separate starts preserve that segment at later knowledge
            # clocks while the newest class observation governs their overlap.
            assertion_id = _event_id("document_continuity", vid, cik, symbol, island[1], island[2], first)
            candidate = {"assertion_id": assertion_id, "perm_security_id": vid, "cik": cik,
                "link_start": first, "link_end": last, "link_basis": "reconstructed_document_symbol_continuity",
                "tier": "medium", "available_at": at, "evidence_ids": json.dumps([event_id]),
                "is_common": status == "common", "class_status": status}
            document = {"document_id": event_id, "perm_security_id": vid, "cik": cik,
                "link_start": first, "link_end": last, "available_at": at, "class_status": status,
                "source_file": evidence[3], "source_sha256": evidence[4], "observed_at": observed}
        yield evidence, candidate, document


def _resolve_raw_membership(row, documents):
    """Version a raw-share rectangle; positive dated evidence closes only its island.

    Preserve every earlier knowledge version and every outside-island business
    segment. A newer unknown or same-clock conflicting document restores the
    unresolved state; it never silently falls back to older common evidence.
    """
    if not documents:
        return [row], []
    raw_id, vid, company, cik, first, last, at, until, raw_evidence, reason = row
    ceiling = last or dt.date.max
    relevant = [d for d in documents if d["link_start"] <= ceiling and d["link_end"] >= first
                and (until is None or d["available_at"] < until)]
    if not relevant:
        return [row], []
    clocks = sorted({at} | {d["available_at"] for d in relevant if d["available_at"] > at})
    output, evidence = [], []
    for index, clock in enumerate(clocks):
        end_clock = clocks[index + 1] if index + 1 < len(clocks) else until
        known = [d for d in relevant if d["available_at"] <= clock]
        boundaries = {first}
        if last is not None and last < dt.date.max:
            boundaries.add(last + dt.timedelta(days=1))
        for document in known:
            boundaries.add(max(first, document["link_start"]))
            if document["link_end"] < ceiling:
                boundaries.add(document["link_end"] + dt.timedelta(days=1))
        starts = sorted(boundaries)
        for position, start in enumerate(starts):
            if start > ceiling:
                continue
            end = min(ceiling, starts[position + 1] - dt.timedelta(days=1)) if position + 1 < len(starts) else last
            spanning = [d for d in known if d["link_start"] <= start <= d["link_end"]]
            newest = max((d["available_at"] for d in spanning), default=None)
            latest = [d for d in spanning if d["available_at"] == newest]
            states = {(d["cik"], d["class_status"]) for d in latest}
            positive = len(states) == 1 and next(iter(states))[1] in ("common", "non_common")
            identifier = _event_id(raw_id, "document_resolution_version", clock, start, end)
            ids = sorted(set(json.loads(raw_evidence)) | {d["document_id"] for d in latest})
            if not positive:
                output.append((identifier, vid, company, cik, start, end, clock, end_clock,
                               json.dumps(ids), reason))
                continue
            proof = min(latest, key=lambda d: d["document_id"])
            why = ("dated_non_common_class" if proof["class_status"] == "non_common" else
                   "dated_issuer_contradiction" if proof["cik"] != cik else "dated_supported_same_issuer")
            assertion = {"raw_membership_id": raw_id, "raw_evidence_ids": json.loads(raw_evidence),
                "document_evidence_ids": [d["document_id"] for d in latest], "raw_candidate_cik": cik,
                "document_cik": proof["cik"], "class_status": proof["class_status"], "reason": why,
                "resolution_available_at": clock, "resolution_valid_until": end_clock,
                "scope": "reconstructed_document_continuity_only_within_exact_symbol_island"}
            evidence.append((identifier, vid, cik, proof["source_file"], proof["source_sha256"],
                json.dumps(assertion, sort_keys=True, default=str), start, end, clock, proof["observed_at"],
                "raw_share_membership_resolution", raw_id, "filing_clock_and_dated_vendor_island"))
    return output, evidence


def _class_proof_at(line, payload: Mapping[str, object], at: dt.datetime, symbol_holders) -> dict[str, object]:
    """Class evidence at an event clock; never use a final bridge classification.

    A dated vendor symbol is modeled available at that session's 22:00 cutoff.
    Only retained share facts known by the event can support common status.
    Current directories and symbols first seen later are absent from this proof.
    """
    from . import identity_reconstruction as ir
    from . import market_owner_bridge as bridge

    known = [item for item in payload.get("items", ())
             if _clock(dt.datetime.fromisoformat(item["known_at"])) <= at]
    symbols = [(symbol, first, min(last, at.date())) for symbol, first, last in line.symbols
               if dt.datetime.combine(first, dt.time(22)) <= at]
    keys = {key for symbol, _, _ in symbols if (key := bridge.normalize_symbol(symbol))}

    def trades_apart(root: str) -> bool:
        return any(other != line.vendor_id and dt.datetime.combine(first, dt.time(22)) <= at
                   and any(first <= own_last and own_first <= min(last, at.date())
                           for _, own_first, own_last in symbols)
                   for other, first, last in symbol_holders.get(root, ()))

    non_common = bridge._line_symbol_class(keys, set(), trades_apart)
    common_fact = any(item["kind"] == ir.EV_SHARES and
                      item.get("concept", "").split(":")[-1] in {concept for _, concept in ir.SHARE_CONCEPTS}
                      for item in known)
    # A suffix heuristic is sufficient to withhold common admission, but is not
    # positive evidence that a possible sibling may be dropped from company ME.
    status = "common" if common_fact and symbols and not non_common else "unknown"
    return {"class_status": status, "security_class": non_common or ("reconstructed_common" if status == "common" else "unknown"),
            "class_basis": "then-known common-share fingerprint and dated vendor symbols; reconstructed, unverified",
            "known_items": known, "symbol_witnesses": symbols}


def _dated_vendor_symbols(con, source: Path, vendor, *, chunks: int = 64, metrics=None):
    """Observed symbol islands; reject ambiguous days and never bridge missing sessions.

    todayTicker is deliberately absent: it is a current descriptor. Multiple
    symbols on one vendor/day withhold that day's descriptor and break islands.
    """
    from dataclasses import replace
    from .calendar import xnys_sessions

    first = min(line.first_trade_date for line in vendor)
    last = max(line.last_trade_date for line in vendor)
    con.execute("CREATE TEMP TABLE identity_sessions(d DATE, ordinal INTEGER)")
    con.executemany("INSERT INTO identity_sessions VALUES (?,?)",
                    [(day, index) for index, day in enumerate(xnys_sessions(first, last))])
    con.execute("CREATE TABLE identity_vendor_symbols(vendor_id BIGINT, symbol VARCHAR, first_date DATE, last_date DATE)")
    ambiguous = 0
    for part in range(chunks):
        began = time.perf_counter()
        con.execute("""CREATE OR REPLACE TEMP TABLE identity_symbol_days AS
            SELECT securityID::BIGINT AS vendor_id, tradingDate::DATE AS d,
                CASE WHEN count(DISTINCT upper(trim(ticker_tk))) = 1
                     THEN max(upper(trim(ticker_tk))) END AS symbol
            FROM read_parquet(?) WHERE securityID > 0 AND close > 0
              AND tradingDate IS NOT NULL AND securityID % ? = ?
            GROUP BY 1,2""", [str(source), chunks, part])
        ambiguous += con.execute("SELECT count(*) FROM identity_symbol_days WHERE symbol IS NULL").fetchone()[0]
        con.execute("""INSERT INTO identity_vendor_symbols
            WITH marked AS (
                SELECT *, CASE WHEN lag(symbol) OVER w IS DISTINCT FROM symbol
                    OR lag(ordinal) OVER w IS DISTINCT FROM ordinal - 1
                    THEN 1 ELSE 0 END AS boundary
                FROM identity_symbol_days LEFT JOIN identity_sessions USING(d)
                WINDOW w AS (PARTITION BY vendor_id ORDER BY d)
            ), islands AS (
                SELECT *, sum(boundary) OVER (PARTITION BY vendor_id ORDER BY d) AS island FROM marked
            ) SELECT vendor_id,symbol,min(d),max(d) FROM islands
              WHERE nullif(symbol,'') IS NOT NULL AND ordinal IS NOT NULL
              GROUP BY vendor_id,symbol,island""")
        if metrics is not None:
            metrics["max_symbol_slice_seconds"] = max(metrics.get("max_symbol_slice_seconds", 0), time.perf_counter() - began)
    symbols = defaultdict(list)
    for vid, symbol, low, high in con.execute("SELECT * FROM identity_vendor_symbols ORDER BY vendor_id,first_date,symbol").fetchall():
        symbols[vid].append((symbol, low, high))
    con.execute("DROP TABLE identity_symbol_days")
    return [replace(line, symbols=tuple(symbols[line.vendor_id])) for line in vendor], ambiguous


def _export_table(con, table: str, out: Path, *, public_name: str | None = None,
                  keys: Sequence[str] | None = None) -> list[dict[str, object]]:
    """No output partition exceeds 2M rows; stable numeric keys define the slices."""
    count = con.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
    slices = max(1, (count + 999_999) // 1_000_000)
    keys = keys or TABLE_KEYS[table]
    key = "perm_company_id" if table == "security_company_links" else keys[0]
    result = []
    for part in range(slices):
        where = f"hash({key}) % {slices} = {part}"
        rows = con.execute(f"SELECT count(*) FROM {table} WHERE {where}").fetchone()[0]
        if rows > 2_000_000:
            raise ValueError(f"{table} partition exceeds 2M rows; increase slices")
        path = out / f"{public_name or table}.part-{part:04d}.parquet"
        temporary = path.with_suffix(".parquet.tmp")
        order = ",".join(keys)
        con.execute(f"COPY (SELECT * FROM {table} WHERE {where} ORDER BY {order}) TO {_literal(temporary.as_posix())} "
                    "(FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 65536)")
        os.replace(temporary, path)
        result.append({"dataset": public_name or table, "table": table, "file": path.name, "rows": rows,
                       "sha256": file_sha256(path), "bytes": path.stat().st_size,
                       "schema": [list(row) for row in con.execute(f"DESCRIBE {table}").fetchall()]})
    return result


def audit_identity(con) -> dict[str, object]:
    """Bounded pre-publication relational and evidence-clock gates."""
    counts = {}
    for table, keys in TABLE_KEYS.items():
        counts[table] = con.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
        if con.execute(f"SELECT 1 FROM {table} GROUP BY {','.join(keys)} HAVING count(*) > 1 LIMIT 1").fetchone():
            raise ValueError(f"duplicate identity keys: {table}")
    if con.execute("""SELECT 1 FROM security_permanent_ids
        WHERE perm_security_id <= 0 OR security_id <> 'TBLTICKERHISTORY-' || perm_security_id
          OR first_trade_date > last_trade_date LIMIT 1""").fetchone():
        raise ValueError("permanent security allocation/date violation")
    if con.execute("""SELECT 1 FROM company_permanent_ids
        WHERE perm_company_id <> try_cast(cik AS BIGINT) OR NOT regexp_full_match(cik,'[0-9]{10}') LIMIT 1""").fetchone():
        raise ValueError("permanent company allocation violation")
    for table in ("security_company_links", "identity_unresolved_memberships"):
        bad = con.execute(f"""WITH referenced AS (
                SELECT l.available_at, unnest(from_json(l.evidence_ids, '["VARCHAR"]')) AS evidence_id
                FROM {table} l
            ) SELECT count(*) FROM referenced r LEFT JOIN identity_link_evidence e USING(evidence_id)
            WHERE e.evidence_id IS NULL OR e.available_at > r.available_at""").fetchone()[0]
        if bad:
            raise ValueError(f"{table}: {bad} missing/future evidence references")
    monthly = []
    for year in range(2013, 2024):
        for month in range(1, 13):
            next_month = dt.date(year + (month == 12), month % 12 + 1, 1)
            at = dt.datetime.combine(next_month - dt.timedelta(days=1), dt.time(22))
            rows = con.execute(f"""SELECT link_basis,class_status,count(*) AS lines,
                count(DISTINCT perm_company_id) AS companies FROM ({links_asof_sql(at)})
                GROUP BY 1,2 ORDER BY 1,2""").fetchall()
            monthly.append({"cutoff": str(at), "eligible": [dict(zip(
                ("basis", "class_status", "lines", "companies"), row, strict=True)) for row in rows]})
    unnamed = con.execute("""SELECT perm_security_id FROM security_permanent_ids p WHERE NOT EXISTS
        (SELECT 1 FROM security_names_history n WHERE n.perm_security_id=p.perm_security_id)
        ORDER BY perm_security_id""").fetchall()
    return {"rows": counts, "duplicate_keys": 0, "allocation_violations": 0,
            "names_covered_lines": counts["security_permanent_ids"] - len(unnamed),
            "lines_without_dated_names": [row[0] for row in unnamed],
            "missing_or_future_evidence": 0, "monthly_coverage": monthly,
            "company_membership_scope": "all known possible members; no closed-world completeness assertion"}


REHEARSAL_PHASES = ("symbols", "candidates", "uncertainty", "links", "names", "export")
CANDIDATE_COLUMNS = ("assertion_id", "perm_security_id", "cik", "link_start", "link_end",
                     "link_basis", "tier", "available_at", "evidence_ids", "is_common", "class_status")
CANDIDATE_DDL = ("assertion_id VARCHAR,perm_security_id BIGINT,cik VARCHAR,link_start DATE,link_end DATE,"
                 "link_basis VARCHAR,tier VARCHAR,available_at TIMESTAMP,evidence_ids VARCHAR,"
                 "is_common BOOLEAN,class_status VARCHAR")


def _snapshot_class_status(item) -> str:
    """Only positive observed directory/class evidence settles a class."""
    from . import market_owner_bridge as bridge

    if item is None:
        return "unknown"
    if item.security_class in bridge._common_equity_types() | {bridge._CLASS_SHARE}:
        return "common"
    if item.basis == bridge.CLASS_BASIS_DIRECTORY and item.security_class not in (
            "unknown", "unclassified", "common_unverified"):
        return "non_common"
    return "unknown"


def _insert_candidates(con, rows):
    _insert_rows(con, "identity_candidates", [tuple(row[key] for key in CANDIDATE_COLUMNS) for row in rows])


def _load_dated_vendor(con):
    from dataclasses import replace
    from .identity_reconstruction import read_vendor_lines

    symbols = defaultdict(list)
    for vid, symbol, first, last in con.execute(
            "SELECT * FROM identity_vendor_symbols ORDER BY vendor_id,first_date,symbol").fetchall():
        symbols[vid].append((symbol, first, last))
    return [replace(line, symbols=tuple(symbols[line.vendor_id])) for line in read_vendor_lines(con)]


def build_rehearsal(work: Path, ops_manifest: Path, out: Path, *, phase: str,
                    document_review: Path | None = None) -> dict[str, object]:
    """Run exactly one immutable phase; launch each phase in a fresh guarded process.

    Completed phase files are read-only inputs. Failed attempts are retained in
    separate directories and cannot be mistaken for a completion receipt.
    No preparation writes, production access, or unaccepted batch-runner imports.
    """
    from types import SimpleNamespace
    from . import identity_reconstruction as ir
    from . import market_owner_bridge as bridge
    from ._submissions_archive import SubmissionsArchive

    if phase not in REHEARSAL_PHASES:
        raise ValueError("choose an explicit rehearsal phase")
    prepared = json.loads((work / "preparation.json").read_text(encoding="utf-8"))
    if "reconstruct" not in prepared["steps"]:
        raise ValueError("retained reconstruction is not complete")
    inputs = prepared["pin"]["inputs"]
    ops = json.loads(ops_manifest.read_text(encoding="utf-8"))
    snapshot = dt.date(2026, 9, 20)
    snapshot_clock = dt.datetime.combine(snapshot, dt.time(22))
    out.mkdir(parents=True, exist_ok=True)
    code_files = ("identity_links.py", "identity_reconstruction.py", "market_owner_bridge.py",
                  "calendar.py", "_submissions_archive.py", "universe_us_listed.py",
                  "historical_identity.py", "historical_identity_sources.py", "_fundamental_clock.py", "ticker_history.py",
                  "market_daily.py", "warehouse.py")
    pin = {"version": "identity_rehearsal_phases_v1", "allocation_version": ALLOCATION_VERSION,
           "preparation_manifest_sha256": file_sha256(work / "preparation.json"),
           "retained_database_sha256": file_sha256(work / "retained.duckdb"),
           "ops_manifest_sha256": file_sha256(ops_manifest),
           "code_files": {name: file_sha256(Path(__file__).parent / name) for name in code_files},
           "inputs": inputs, "document_review": _document_review_pin(document_review)}
    if pin["code_files"]["identity_links.py"] != _LOADED_CODE_SHA256:
        raise ValueError("identity source changed after process import")
    build_path = out / "build.json"
    if build_path.exists():
        build = json.loads(build_path.read_text(encoding="utf-8"))
        if build["pin"] != pin:
            raise ValueError("build source/input pins changed; use a new build directory")
    else:
        if phase != "symbols":
            raise ValueError("symbols must initialize the build")
        build = {"pin": pin, "created_at": str(dt.datetime.now(dt.UTC).replace(tzinfo=None))}
        code_root = out / "code_snapshot"
        code_root.mkdir(exist_ok=True)
        for name, expected in pin["code_files"].items():
            shutil.copyfile(Path(__file__).parent / name, code_root / name)
            if file_sha256(code_root / name) != expected:
                raise ValueError(f"source changed during snapshot: {name}")
        _write_json(build_path, build)
    for name, expected in pin["code_files"].items():
        if file_sha256(out / "code_snapshot" / name) != expected:
            raise ValueError(f"retained source snapshot changed: {name}")
    created = dt.datetime.fromisoformat(build["created_at"])
    phase_root = out / "phases"
    phase_root.mkdir(exist_ok=True)
    receipts = {}
    for name in REHEARSAL_PHASES[:REHEARSAL_PHASES.index(phase) + 1]:
        receipt_path = phase_root / f"{name}.json"
        if not receipt_path.exists():
            if name != phase:
                raise ValueError(f"complete phase {name} first")
            break
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if receipt["build_sha256"] != file_sha256(build_path):
            raise ValueError(f"phase {name} build pin mismatch")
        for item in receipt["files"]:
            if file_sha256(out / item["file"]) != item["sha256"]:
                raise ValueError(f"sealed phase {name} output changed: {item['file']}")
        receipts[name] = receipt
    if phase in receipts:
        if phase == "export" and not (out / "manifest.json").exists():
            _seal_rehearsal_manifest(out, pin, created, receipts[phase])
        return {**receipts[phase], "reused": True}
    attempt = 1
    while (phase_root / f"{phase}-{attempt:03d}").exists():
        attempt += 1
    attempt_root = phase_root / f"{phase}-{attempt:03d}"
    attempt_root.mkdir()
    launch = {"phase": phase, "build_sha256": file_sha256(build_path), "pin": pin,
              "parents": {name: file_sha256(phase_root / f"{name}.json") for name in receipts}}
    _write_json(attempt_root / "launch.json", launch)
    con = _connect(attempt_root / "identity.duckdb")
    details = {}
    output_tables = []
    table_orders = {}
    evidence_rows, unresolved_rows, candidate_rows = [], [], []
    try:
        con.execute(f"ATTACH {_literal((work / 'retained.duckdb').as_posix())} AS preparation (READ_ONLY)")
        for table in ("ri_vendor_lines", "ri_vendor_symbols", "ri_share_runs", "ri_share_facts", "ri_operating_lines"):
            con.execute(f"CREATE TEMP VIEW {table} AS SELECT * FROM preparation.{table}")
        def load_table(name, phases):
            paths = [str((out / item["file"]).resolve()) for parent in phases
                     for item in receipts[parent]["files"] if item["table"] == name]
            if not paths:
                raise ValueError(f"missing upstream table {name}")
            values = ",".join(_literal(path) for path in paths)
            con.execute(f"CREATE TEMP VIEW {name} AS SELECT * FROM read_parquet([{values}])")
        if phase == "symbols":
            source = Path(inputs["ticker_history"]["path"])
            if file_sha256(source) != inputs["ticker_history"]["sha256"]:
                raise ValueError("retained ticker source changed")
            vendor, rejected = _dated_vendor_symbols(con, source, ir.read_vendor_lines(con), metrics=details)
            con.execute("CREATE TABLE security_permanent_ids (" + TABLE_COLUMNS["security_permanent_ids"] + ")")
            for begin in range(0, len(vendor), 1000):
                _insert_rows(con, "security_permanent_ids", [(line.vendor_id, line.price_security_id,
                    line.first_trade_date, line.last_trade_date, created) for line in vendor[begin:begin+1000]])
            details.update(vendor_lines=len(vendor), ambiguous_or_missing_symbol_days=rejected)
            output_tables = ["identity_vendor_symbols", "security_permanent_ids"]
            table_orders["identity_vendor_symbols"] = ("vendor_id", "first_date", "symbol")
        elif phase == "candidates":
            load_table("identity_vendor_symbols", ("symbols",))
            vendor = _load_dated_vendor(con)
            line_by_id = {line.price_security_id: line for line in vendor}
            for export in ops["exports"]:
                if not export.get("file"):
                    continue
                path = ops_manifest.parent / export["file"]
                if file_sha256(path) != export["sha256"]:
                    raise ValueError(f"OPS export hash mismatch: {path}")
                table = export["table"]
                if table in ("securities", "exchange_listings", "sec_company_tickers",
                             "nasdaq_symbol_directory", "security_identifier_history"):
                    # Existing bridge adapters discover persistent tables, not
                    # TEMP VIEWs. These five exports are individually < 53k rows.
                    con.execute(f"CREATE TABLE {table} AS SELECT * FROM read_parquet({_literal(path.as_posix())})")
            evidence_files = []
            for entry in prepared["steps"]["reconstruct"]["files"]:
                path = work / entry["file"]
                if file_sha256(path) != entry["sha256"]:
                    raise ValueError(f"reconstruction output hash mismatch: {path}")
                evidence_files.append(path)
            reconstructed = bridge.reconstructed_link_evidence(_evidence_rows(con, evidence_files))
            lines = [bridge.PriceLine(line.price_security_id, line.last_symbol, line.first_trade_date,
                                      line.last_trade_date, line.bar_rows, vendor_id=line.vendor_id) for line in vendor]
            symbols = [bridge.LineSymbol(line.price_security_id, symbol, first, last)
                       for line in vendor for symbol, first, last in line.symbols]
            ticker_observed = dt.datetime.fromisoformat(inputs["company_tickers"]["received_at"])
            ticker_source = Path(inputs["company_tickers"]["path"])
            if file_sha256(ticker_source) != inputs["company_tickers"]["sha256"]:
                raise ValueError("retained company ticker source changed")
            tickers = [(cik, ticker, ticker_observed) for cik, ticker in
                       ir.read_sec_ticker_snapshot(ticker_source)]
            directory = bridge._read_directory(SimpleNamespace(con=con))
            if not directory.types:
                raise ValueError("nonempty pinned directory was not read by bridge adapter")
            current_classes = bridge._class_index(bridge.classify_sec_tickers(tickers, directory.types))
            directory_clock = con.execute("SELECT max(source_loaded_at) FROM nasdaq_symbol_directory").fetchone()[0] or created
            retrospective, _, _, bridge_stats = bridge.classify_reconstructed_with_history(
                lines, tickers, {}, directory.types, symbols, directory.adr_ratios, reconstructed=reconstructed)
            symbol_holders = defaultdict(list)
            for line in vendor:
                for symbol, first, last in line.symbols:
                    if key := bridge.normalize_symbol(symbol):
                        symbol_holders[key].append((line.vendor_id, first, last))
            operating = {int(row[0]) for row in con.execute("SELECT vendor_id FROM ri_operating_lines").fetchall()}
            retrospective_linked = {line_by_id[row.price_security_id].vendor_id for row in retrospective if row.linked}
            details = {"operating_proxy_lines": len(operating),
                       "retrospective_operating_linked": len(operating & retrospective_linked), "bridge_stats": bridge_stats}
            create_identity_tables(con)
            con.execute("CREATE TABLE identity_candidates (" + CANDIDATE_DDL + ")")
            del reconstructed, lines, symbols
            # Every evidence event names only the inputs needed at that clock. The
            # parent RI row can include later evidence; it is retained as provenance,
            # never silently treated as all known at the earliest tier.
            for ordinal, row in enumerate(_evidence_rows(con, evidence_files)):
                if ordinal % 100 == 0:
                    _insert_rows(con, "identity_link_evidence", evidence_rows)
                    evidence_rows.clear()
                    _insert_rows(con, "identity_unresolved_memberships", unresolved_rows)
                    unresolved_rows.clear()
                    _insert_candidates(con, candidate_rows)
                    candidate_rows.clear()
                payload = json.loads(row["value_json"])
                vendor_id = int(row["native_key"])
                line = line_by_id.get(f"TBLTICKERHISTORY-{vendor_id}")
                if line is None:
                    raise ValueError(f"evidence names unknown vendor {vendor_id}")
                if row["evidence_status"] != "reconstructed":
                    # Rejected RI rows carry no historical knowledge clock. Keep
                    # uncertainty at observation only; never fabricate an early one.
                    at = _clock(row["observed_at"])
                    if at <= snapshot_clock:
                        event_id = _event_id(row["evidence_id"], "unresolved", at)
                        first = row["valid_from"] or line.first_trade_date
                        last = row["valid_to"] - dt.timedelta(days=1) if row["valid_to"] else line.last_trade_date
                        evidence_rows.append((event_id, vendor_id, row["cik"], inputs["companyfacts"]["path"],
                            inputs["companyfacts"]["sha256"], json.dumps(payload, sort_keys=True), first, last, at, at,
                            "unresolved_company_candidate", None, "observed_reconstruction_without_historical_clock"))
                        unresolved_rows.append((event_id, vendor_id, int(row["cik"]), row["cik"], first, last,
                                                at, None, json.dumps([event_id]), payload["rejection_reason"]))
                    continue
                history = [(tier, _clock(dt.datetime.fromisoformat(stamp))) for tier, stamp in payload["tier_history"]]
                clocks = {at for _, at in history}
                clocks.update(dt.datetime.combine(first, dt.time(22)) for _, first, _ in line.symbols)
                previous = None
                for at in sorted(clocks):
                    if at > snapshot_clock:
                        continue
                    known_tiers = [(tier, since) for tier, since in history if since <= at]
                    if not known_tiers:
                        continue
                    tier = max(known_tiers, key=lambda item: item[1])[0]
                    proof = _class_proof_at(line, payload, at, symbol_holders)
                    state = (tier, proof["class_status"], proof["security_class"])
                    if state == previous:
                        continue
                    previous = state
                    event_id = _event_id(row["evidence_id"], tier, at)
                    assertion = {"parent_evidence_id": row["evidence_id"], "tier": tier,
                                 **proof, "is_common": proof["class_status"] == "common",
                                 "method": prepared["pin"]["reconstruction_method"]}
                    evidence_rows.append((event_id, vendor_id, row["cik"], inputs["companyfacts"]["path"],
                        inputs["companyfacts"]["sha256"], json.dumps(assertion, sort_keys=True, default=str), row["valid_from"],
                        row["valid_to"] - dt.timedelta(days=1) if row["valid_to"] else None,
                        at, row["observed_at"], "reconstructed_tier_and_class", None, "modeled_reconstruction"))
                    candidate_rows.append({"assertion_id": row["evidence_id"], "perm_security_id": vendor_id,
                        "cik": row["cik"], "link_start": row["valid_from"],
                        "link_end": row["valid_to"] - dt.timedelta(days=1) if row["valid_to"] else None,
                        "link_basis": "reconstructed_" + tier, "tier": tier, "available_at": at,
                        "evidence_ids": json.dumps([event_id]), "is_common": proof["class_status"] == "common",
                        "class_status": proof["class_status"]})
            snapshot_events = {}
            for row in retrospective:
                if not row.linked or row.evidence_id or not row.cik:
                    continue
                line = line_by_id[row.price_security_id]
                # A current snapshot cannot identify a long-ended holder of a reused ticker.
                if (snapshot - line.last_trade_date).days > bridge.STALE_LINK_DAYS:
                    continue
                at = row.evidence_observed_at or ticker_observed
                if at > snapshot_clock:
                    continue
                event_id = _event_id("current_sec_ticker", line.vendor_id, row.cik, at)
                item = current_classes.get((row.cik, bridge.normalize_symbol(row.share_class_symbol)))
                status = _snapshot_class_status(item)
                signature = (row.share_class_symbol, status, item.security_class if item else None,
                             item.basis if item else None)
                if event_id in snapshot_events:
                    if snapshot_events[event_id] != signature:
                        raise ValueError(f"conflicting current snapshot assertion {event_id}")
                    continue  # One observed event, despite historical bridge segmentation.
                snapshot_events[event_id] = signature
                evidence_rows.append((event_id, line.vendor_id, row.cik, inputs["company_tickers"]["path"],
                    inputs["company_tickers"]["sha256"], json.dumps({"ticker": row.share_class_symbol,
                    "scope": "snapshot_observation_onward", "common_class": False}), at.date(), None, at,
                    ticker_observed, "current_ticker_snapshot", None, "observed_snapshot"))
                candidate_rows.append({"assertion_id": event_id, "perm_security_id": line.vendor_id, "cik": row.cik,
                    "link_start": at.date(), "link_end": None, "link_basis": "current_ticker_verified", "tier": None,
                    "available_at": at, "evidence_ids": json.dumps([event_id]), "is_common": False,
                    "class_status": "unknown"})
                if status != "unknown" and directory_clock <= snapshot_clock:
                    common_at = max(at, directory_clock)
                    common_id = _event_id(event_id, "directory_class", status, common_at)
                    directory_entry = next(item for item in ops["exports"] if item.get("table") == "nasdaq_symbol_directory" and item.get("file"))
                    evidence_rows.append((common_id, line.vendor_id, row.cik, str(ops_manifest.parent / directory_entry["file"]),
                        directory_entry["sha256"], json.dumps({"security_class": item.security_class,
                            "class_basis": item.basis, "class_status": status}),
                        at.date(), None, common_at, directory_clock, "current_classification", None, "observed_snapshot"))
                    promoted = {**candidate_rows[-1], "available_at": common_at,
                                "evidence_ids": json.dumps([event_id, common_id]), "is_common": status == "common", "class_status": status}
                    if common_at == at:
                        candidate_rows[-1] = promoted
                    else:
                        candidate_rows.append(promoted)
            # Current SEC tickers are possible company classes even if the price
            # universe contains no usable line for them. Count them from observation,
            # never before, so a missing sibling cannot manufacture complete ME.
            for cik, ticker, at in tickers:
                if at > snapshot_clock:
                    continue
                key = bridge.normalize_symbol(ticker)
                represented = any((snapshot - last).days <= bridge.STALE_LINK_DAYS
                                  for _, _, last in symbol_holders.get(key, ()))
                if represented:
                    continue
                event_id = _event_id("unrepresented_current_ticker", cik, ticker, at)
                evidence_rows.append((event_id, None, cik, inputs["company_tickers"]["path"],
                    inputs["company_tickers"]["sha256"], json.dumps({"ticker": ticker, "class_status": "unknown"}),
                    at.date(), None, at, at, "unrepresented_current_ticker", None, "observed_snapshot"))
                unresolved_rows.append((event_id, None, int(cik), cik, at.date(), None, at, None,
                                        json.dumps([event_id]), "current_ticker_without_price_line"))
            _insert_rows(con, "identity_link_evidence", evidence_rows)
            _insert_rows(con, "identity_unresolved_memberships", unresolved_rows)
            _insert_candidates(con, candidate_rows)
            evidence_rows.clear()
            unresolved_rows.clear()
            candidate_rows.clear()
            del retrospective, current_classes, snapshot_events
            con.execute("CREATE TABLE identity_document_assertions(document_id VARCHAR,perm_security_id BIGINT,available_at TIMESTAMP,payload VARCHAR)")
            details["document_reviews"] = details["document_supported_events"] = 0
            if document_review is not None:
                document_candidates = {}
                for evidence, candidate, document in _reviewed_document_events(document_review, vendor, snapshot_clock):
                    details["document_reviews"] += 1
                    _insert_rows(con, "identity_link_evidence", [evidence])
                    if candidate:
                        key = candidate["assertion_id"], candidate["available_at"]
                        document_candidates[key] = (_merge_candidate_evidence(document_candidates[key], candidate)
                            if key in document_candidates else candidate)
                        _insert_rows(con, "identity_document_assertions", [(document["document_id"], document["perm_security_id"],
                            document["available_at"], json.dumps(document, sort_keys=True, default=str))])
                        details["document_supported_events"] += 1
                _insert_candidates(con, list(document_candidates.values()))
                details["document_candidate_events"] = len(document_candidates)
            output_tables = ["identity_candidates", "identity_link_evidence", "identity_unresolved_memberships", "identity_document_assertions"]
            table_orders["identity_candidates"] = ("cik", "perm_security_id", "assertion_id", "available_at")
            table_orders["identity_document_assertions"] = ("document_id",)
        elif phase == "uncertainty":
            load_table("identity_candidates", ("candidates",))
            load_table("identity_document_assertions", ("candidates",))
            # Share matching needs only ids/dates; symbols stay in their sealed phase.
            vendor = ir.read_vendor_lines(con)
            line_by_id = {line.price_security_id: line for line in vendor}
            create_identity_tables(con)
            # Recover uncertainty clocks from bounded retained share-match slices.
            # Rejection summaries have no historical clock, but their raw candidate
            # facts do. Before a supported link exists, a known possible common
            # member must withhold company ME; it cannot vanish from the denominator.
            first_link = {(vid, cik): at for vid, cik, at in con.execute(
                "SELECT perm_security_id,cik,min(available_at) FROM identity_candidates "
                "WHERE link_basis<>'reconstructed_document_symbol_continuity' GROUP BY 1,2").fetchall()}
            params = ir.ReconstructionParams()
            _insert_rows(con, "identity_link_evidence", evidence_rows)
            evidence_rows.clear()
            _insert_rows(con, "identity_unresolved_memberships", unresolved_rows)
            unresolved_rows.clear()
            for begin in range(0, len(vendor), 100):
                began = time.perf_counter()
                ids = [line.vendor_id for line in vendor[begin:begin+100]]
                documents = defaultdict(list)
                for (payload,) in con.execute("SELECT payload FROM identity_document_assertions WHERE perm_security_id IN (SELECT unnest(?))", [ids]).fetchall():
                    document = json.loads(payload)
                    for name in ("link_start", "link_end"):
                        document[name] = dt.date.fromisoformat(document[name])
                    for name in ("available_at", "observed_at"):
                        document[name] = dt.datetime.fromisoformat(document[name])
                    documents[document["perm_security_id"]].append(document)
                matches = ir.match_share_counts(con, params, vendor_ids=ids)
                share_items, _ = ir._share_items(matches)
                for (vid, cik), items in share_items.items():
                    item = min(items, key=lambda value: (value.known_at, str(value.detail)))
                    at, until = item.known_at, first_link.get((vid, cik))
                    if at > snapshot_clock or (until is not None and at >= until):
                        continue
                    line = line_by_id[f"TBLTICKERHISTORY-{vid}"]
                    event_id = _event_id("possible_common_member", vid, cik, at)
                    evidence_rows.append((event_id, vid, cik, inputs["companyfacts"]["path"], inputs["companyfacts"]["sha256"],
                        json.dumps({"kind": item.kind, "known_at": at, **item.detail,
                            "interpretation": "possible common member only; identity not yet supported"}, sort_keys=True, default=str),
                        line.first_trade_date, line.last_trade_date, at, dt.datetime.fromisoformat(inputs["companyfacts"]["received_at"]),
                        "unresolved_share_candidate", None, "modeled_filed_plus_46h_and_vendor_close"))
                    unresolved, resolutions = _resolve_raw_membership((event_id, vid, int(cik), cik,
                        line.first_trade_date, line.last_trade_date, at, until, json.dumps([event_id]),
                        "share_candidate_before_supported_identity"), documents.get(vid, []))
                    unresolved_rows.extend(unresolved)
                    evidence_rows.extend(resolutions)
                    details["document_resolution_events"] = details.get("document_resolution_events", 0) + len(resolutions)
                del matches, share_items, documents
                _insert_rows(con, "identity_link_evidence", evidence_rows)
                evidence_rows.clear()
                _insert_rows(con, "identity_unresolved_memberships", unresolved_rows)
                unresolved_rows.clear()
                details["max_uncertainty_slice_seconds"] = max(details.get("max_uncertainty_slice_seconds", 0), time.perf_counter() - began)
            output_tables = ["identity_link_evidence", "identity_unresolved_memberships"]
        elif phase == "links":
            load_table("identity_candidates", ("candidates",))
            load_table("identity_unresolved_memberships", ("candidates", "uncertainty"))
            for table in ("company_permanent_ids", "security_company_links"):
                con.execute(f"CREATE TABLE {table} ({TABLE_COLUMNS[table]})")
            ciks = [row[0] for row in con.execute("""SELECT cik FROM identity_candidates UNION
                SELECT cik FROM identity_unresolved_memberships ORDER BY cik""").fetchall()]
            for cik in ciks:
                began = time.perf_counter()
                rows = _row_dicts(con, "SELECT * FROM identity_candidates WHERE cik=" + _literal(cik))
                versions = _company_versions(rows, created)
                _insert_rows(con, "security_company_links", versions)
                details["max_company_slice_seconds"] = max(details.get("max_company_slice_seconds", 0), time.perf_counter() - began)
                details["max_company_slice_rows"] = max(details.get("max_company_slice_rows", 0), len(versions))
            details["company_slices"] = len(ciks)
            details["max_transaction_rows"] = 1000
            _insert_rows(con, "company_permanent_ids", [(int(cik), cik, "SEC_CIK_permanent_v1", created) for cik in ciks])
            output_tables = ["company_permanent_ids", "security_company_links"]
        elif phase == "names":
            load_table("identity_vendor_symbols", ("symbols",))
            load_table("identity_candidates", ("candidates",))
            con.execute("CREATE TABLE security_names_history (" + TABLE_COLUMNS["security_names_history"] + ")")
            con.execute("""INSERT INTO security_names_history
                SELECT vendor_id,symbol,NULL,NULL,first_date,last_date,'vendor_ticker_interval_reconstructed',
                       CAST(first_date AS TIMESTAMP) + INTERVAL 22 HOUR FROM identity_vendor_symbols""")
            submissions_clock = dt.datetime.fromisoformat(inputs["submissions"]["received_at"])
            source = Path(inputs["submissions"]["path"])
            if file_sha256(source) != inputs["submissions"]["sha256"]:
                raise ValueError("retained submissions source changed")
            ciks = [row[0] for row in con.execute("SELECT DISTINCT cik FROM identity_candidates ORDER BY cik").fetchall()]
            with SubmissionsArchive(source) as archive:
                for cik in ciks:
                    began = time.perf_counter()
                    member = f"CIK{cik}.json"
                    if member not in archive:
                        continue
                    payload = json.loads(archive.read(member))
                    spans_by_line = defaultdict(set)
                    for vid, first, last in con.execute("SELECT DISTINCT perm_security_id,link_start,link_end "
                            "FROM identity_candidates WHERE cik=?", [cik]).fetchall():
                        spans_by_line[vid].add((first, last))
                    names_rows = set()
                    for vid, spans in spans_by_line.items():
                        if payload.get("name") and any(first <= submissions_clock.date() and
                                (last is None or last >= submissions_clock.date()) for first, last in spans):
                            names_rows.add((vid, None, str(payload["name"]), None, submissions_clock.date(), None,
                                            "sec_current_name_snapshot", submissions_clock))
                        for former in payload.get("formerNames") or ():
                            if not former.get("name") or not former.get("from") or not former.get("to"):
                                continue
                            first = dt.date.fromisoformat(former["from"][:10])
                            last = dt.date.fromisoformat(former["to"][:10])
                            for link_first, link_last in spans:
                                low, high = max(first, link_first), min(last, link_last or dt.date.max)
                                if low <= high:
                                    names_rows.add((vid, None, str(former["name"]), None, low, high,
                                                    "sec_former_name_reconstructed", submissions_clock))
                    _insert_rows(con, "security_names_history", sorted(names_rows, key=lambda row: tuple(str(v) for v in row)))
                    details["max_name_slice_seconds"] = max(details.get("max_name_slice_seconds", 0), time.perf_counter() - began)
                    details["max_name_slice_rows"] = max(details.get("max_name_slice_rows", 0), len(names_rows))
            # Two CIK candidates may carry the same independently observed name.
            # Dedupe the full natural key only, without preferring one candidate.
            con.execute("CREATE TABLE names_distinct AS SELECT DISTINCT * FROM security_names_history")
            con.execute("DROP TABLE security_names_history")
            con.execute("ALTER TABLE names_distinct RENAME TO security_names_history")
            output_tables = ["security_names_history"]
        else:
            for table, phases in (
                ("security_permanent_ids", ("symbols",)), ("company_permanent_ids", ("links",)),
                ("security_company_links", ("links",)), ("security_names_history", ("names",)),
                ("identity_link_evidence", ("candidates", "uncertainty")),
                ("identity_unresolved_memberships", ("candidates", "uncertainty"))):
                load_table(table, phases)
            create_security_master_public_view(con)
            details["audit"] = audit_identity(con)
            linked, operating_linked, operating_count = con.execute("""WITH linked AS (
                SELECT DISTINCT perm_security_id FROM security_company_links
                WHERE tier IN ('high','medium') OR tier IS NULL)
                SELECT (SELECT count(*) FROM linked),
                (SELECT count(*) FROM ri_operating_lines JOIN linked ON vendor_id=perm_security_id),
                (SELECT count(*) FROM ri_operating_lines)""").fetchone()
            details["coverage"] = {**receipts["symbols"]["details"], **receipts["candidates"]["details"],
                "operating_linked": operating_linked, "operating_link_fraction": operating_linked / operating_count,
                "unlinked_lines": receipts["symbols"]["details"]["vendor_lines"] - linked,
                "operating_definition": "retained earnFlag=0, explicitly a vendor proxy"}
            output_tables = list(TABLE_COLUMNS)
        files = []
        for table in output_tables:
            keys = table_orders.get(table) or TABLE_KEYS[table]
            if con.execute(f"SELECT 1 FROM {table} GROUP BY {','.join(keys)} HAVING count(*)>1 LIMIT 1").fetchone():
                raise ValueError(f"duplicate keys before phase seal: {table}")
            files.extend(_export_table(con, table, attempt_root, keys=keys,
                public_name="link_evidence" if table == "identity_link_evidence" else None))
        for item in files:
            item["file"] = (attempt_root / item["file"]).relative_to(out).as_posix()
        con.execute("CHECKPOINT")
    finally:
        con.close()
    result = {"phase": phase, "build_sha256": file_sha256(build_path), "files": files,
              "details": details, "reused": False, "launch": str((attempt_root / "launch.json").relative_to(out))}
    _write_json(phase_root / f"{phase}.json", result)
    if phase == "export":
        _seal_rehearsal_manifest(out, pin, created, result)
    return result


def _seal_rehearsal_manifest(out, pin, created, result):
    manifest = {"schema_version": SCHEMA_VERSION, "rehearsal": True, "snapshot_date": "2026-09-20",
        "allocation_version": ALLOCATION_VERSION, "created_at": str(created), "scope_complete": False,
        "code_sha256": _LOADED_CODE_SHA256, **{key: pin[key] for key in
            ("preparation_manifest_sha256", "retained_database_sha256", "ops_manifest_sha256", "inputs", "code_files", "document_review")},
        "files": result["files"], **result["details"],
        "phases": {name: file_sha256(out / "phases" / f"{name}.json") for name in REHEARSAL_PHASES},
        "end_date_semantics": "inclusive business end; valid_until is exclusive evidence-version end",
        "availability_basis": "reconstructed RI cumulative evidence; observed current snapshots never backfilled",
        "remaining_acceptance": ["ledgered stage publication", "50 delisted cases", "multi-class ME",
                                 "monthly asof coverage", "repeatability", "331 baseline reconciliation"]}
    _write_json(out / "manifest.json", manifest)



def file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _literal(value: object) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def _write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _connect(path: Path) -> duckdb.DuckDBPyConnection:
    spill = path.with_name(path.name + ".spill")
    spill.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect(str(path), config={"memory_limit": "256MB", "threads": 1,
        "preserve_insertion_order": False, "temp_directory": str(spill)})
    con.execute("SET TimeZone='UTC'")
    return con


def _retained_inputs(receipt: Path):
    from .identity_reconstruction import ReconstructionInputs, RetainedInput

    values = json.loads(receipt.read_text(encoding="utf-8"))["inputs"]
    inputs = {}
    for name in ("ticker_history", "companyfacts", "submissions", "company_tickers"):
        item = values[name]
        path = Path(item["path"])
        actual = file_sha256(path)
        if actual != item["sha256"]:
            raise ValueError(f"{name}: retained bytes differ from receipt {receipt}")
        observed = dt.datetime.fromisoformat(item["received_at"])
        if observed.tzinfo:
            observed = observed.astimezone(dt.UTC).replace(tzinfo=None)
        inputs[name] = RetainedInput(name, path, actual, observed, item["receipt_basis"])
    return ReconstructionInputs(**inputs)


def prepare_rehearsal_inputs(work: Path, receipt: Path, step: str) -> dict[str, object]:
    """Resumable bounded preparation: vendor -> facts -> reconstruction.

    Only the selected step's tables are replaced after an interrupted attempt.
    Inputs/code are pinned before any write; completed evidence files are hashed.
    No production connection, network request or whole-stage transaction occurs.
    """
    from . import identity_reconstruction as ir
    import pyarrow as pa
    import pyarrow.parquet as pq

    inputs = _retained_inputs(receipt)
    pin = {"version": PREPARATION_VERSION, "reconstruction_method": ir.METHOD,
           "reconstruction_code_sha256": file_sha256(Path(ir.__file__)), "params_digest": ir.ReconstructionParams().digest(),
           "inputs": {item.name: item.as_detail() for item in inputs.files()}}
    work.mkdir(parents=True, exist_ok=True)
    manifest_path = work / "preparation.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {"pin": pin, "steps": {}}
    if manifest["pin"] != pin:
        raise ValueError("retained preparation input/code pins changed: use a new work directory")
    if step in manifest["steps"]:
        result = manifest["steps"][step]
        for output in result.get("files", []):
            if file_sha256(work / output["file"]) != output["sha256"]:
                raise ValueError(f"prepared output changed: {output['file']}")
        return {**result, "reused": True}
    _write_json(manifest_path, manifest)
    con = _connect(work / "retained.duckdb")
    result: dict[str, object] = {"step": step, "reused": False}
    try:
        if step == "vendor":
            for table in ("ri_vendor_lines", "ri_vendor_symbols", "ri_share_runs", "ri_share_obs"):
                con.execute(f"DROP TABLE IF EXISTS {table}")
            ir.stage_vendor_ticker_history(con, inputs.ticker_history.path, chunks=64)
            result["vendor_lines"] = con.execute("SELECT count(*) FROM ri_vendor_lines").fetchone()[0]
            con.execute("CREATE OR REPLACE TABLE ri_operating_lines AS SELECT DISTINCT securityID::BIGINT AS vendor_id "
                        "FROM read_parquet(?) WHERE securityID > 0 AND earnFlag = '0'", [str(inputs.ticker_history.path)])
            result["operating_proxy_lines"] = con.execute("SELECT count(*) FROM ri_operating_lines").fetchone()[0]
        elif step == "facts":
            if "vendor" not in manifest["steps"]:
                raise ValueError("complete vendor preparation first")
            con.execute("DROP TABLE IF EXISTS ri_share_facts")
            result["share_facts"] = ir.stage_share_facts(con, ir.iter_companyfacts_share_facts(inputs.companyfacts.path), batch=10_000)
        elif step == "reconstruct":
            if "facts" not in manifest["steps"]:
                raise ValueError("complete fact preparation first")
            params = ir.ReconstructionParams()
            lines = ir.read_vendor_lines(con)
            ciks = set()
            for start in range(0, len(lines), 500):
                ciks.update(str(row[1]) for row in ir.match_share_counts(con, params, vendor_ids=[line.vendor_id for line in lines[start:start+500]]))
            filings = ir.read_lifecycle_filings(inputs.submissions.path, sorted(ciks))
            result.update(candidate_ciks=len(ciks), lifecycle_filings=len(filings), vendor_lines=len(lines), evidence_rows=0, files=[])
            artifacts = {item.name + "_sha256": item.sha256 for item in inputs.files()}
            output_dir = work / "evidence"
            output_dir.mkdir(exist_ok=True)
            for ordinal, batch in enumerate(ir.reconstruct_in_batches(con, lines, filings=filings,
                    tickers=ir.read_sec_ticker_snapshot(inputs.company_tickers.path),
                    ticker_observed_at=inputs.company_tickers.received_at, params=params, batch_size=250)):
                rows = batch.evidence_rows(observed_at=inputs.companyfacts.received_at,
                    source_loaded_at=inputs.companyfacts.received_at, run_id="identity-rehearsal-preparation",
                    artifact_sha256=inputs.companyfacts.sha256, artifacts=artifacts, revision_key=inputs.revision_key())
                path = output_dir / f"part-{ordinal:04d}.parquet"
                temporary = path.with_suffix(".parquet.tmp")
                table = pa.Table.from_pylist(rows, schema=ir._evidence_schema())
                pq.write_table(table, temporary, compression="zstd")
                os.replace(temporary, path)
                result["files"].append({"file": path.relative_to(work).as_posix(), "rows": len(rows),
                                       "sha256": file_sha256(path), "bytes": path.stat().st_size})
                result["evidence_rows"] += len(rows)
                print(json.dumps({"step": step, "batch": ordinal, "rows": len(rows)}), flush=True)
        else:
            raise ValueError(f"unknown preparation step {step}")
        con.execute("CHECKPOINT")
    finally:
        con.close()
    manifest["steps"][step] = result
    _write_json(manifest_path, manifest)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare")
    prepare.add_argument("--work", type=Path, required=True)
    prepare.add_argument("--receipt", type=Path, required=True)
    prepare.add_argument("--step", choices=("vendor", "facts", "reconstruct"), required=True)
    rehearsal = commands.add_parser("rehearsal")
    rehearsal.add_argument("--work", type=Path, required=True)
    rehearsal.add_argument("--ops-manifest", type=Path, required=True)
    rehearsal.add_argument("--out", type=Path, required=True)
    rehearsal.add_argument("--phase", choices=REHEARSAL_PHASES, required=True)
    rehearsal.add_argument("--document-review", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        result = prepare_rehearsal_inputs(args.work, args.receipt, args.step)
    else:
        result = build_rehearsal(args.work, args.ops_manifest, args.out, phase=args.phase,
                                 document_review=args.document_review)
    print(json.dumps(result, default=str), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
