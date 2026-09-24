"""Bounded evidence for resuming an incomplete full companyfacts archive load.

No warehouse migrations or persistent completion tables are required. Receipts
are mutable, so only extant, compatible receipts can authorize a skip. Missing
receipts cause replay; a receipt with contradictory retained evidence fails.
"""
from __future__ import annotations

import datetime as dt
import json
import logging
import uuid
from collections.abc import Iterator
from dataclasses import dataclass
from typing import Any

from .connection import DuckDBStore

_FETCH_ROWS = 256
_MAX_LINEAGE = 32
LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class VerifiedCompanyFactsMember:
    rows: int
    run_id: str
    unresolved: dict[str, Any] | None


def _rows(store: DuckDBStore) -> Iterator[tuple[Any, ...]]:
    # Every caller exhausts this query before issuing another or reopening.
    while batch := store.con.fetchmany(_FETCH_ROWS):
        yield from batch


def _source_incomplete_run(
    store: DuckDBStore, run_id: str, started_at: dt.datetime, finished_at: dt.datetime,
) -> bool:
    """A normal loader return can still have durably recorded source failures.

    Dataset.run's succeeded status records a normal return. Only this run-bound
    positive source-error observation allows such a terminal run into lineage;
    mutable per-member error receipts are not required to survive later retries.
    """
    return store.con.execute(
        """SELECT 1 FROM data_quality_checks
           WHERE dataset_id='sec_company_facts' AND table_name='sec_company_facts'
             AND check_name='source_completeness' AND status='failed'
             AND observed_value > 0 AND checked_at BETWEEN ? AND ?
             AND json_extract_string(try_cast(details_json AS JSON), '$.run_id') = ?
             AND try_cast(json_extract_string(try_cast(details_json AS JSON),
                                             '$.failed_target_count') AS BIGINT) > 0
           LIMIT 1""", [started_at, finished_at, run_id],
    ).fetchone() is not None


def _lineage(store: DuckDBStore, options: Any) -> dict[str, tuple[dt.datetime, dt.datetime]]:
    if (options.symbol_source != "archive_members" or options.companyfacts_zip is None
            or options.symbol_limit is not None or options.symbol_offset != 0
            or options.skip_loaded_targets):
        raise ValueError("companyfacts resume requires the full archive with replacement enabled")
    expected = {
        "companyfacts_zip": str(options.companyfacts_zip),
        "symbol_source": "archive_members", "symbol_limit": None, "symbol_offset": 0,
        "skip_loaded_targets": False,
        "as_of_date": options.as_of_date.isoformat() if options.as_of_date else None,
        "universe_id": options.universe_id,
    }
    lineage: dict[str, tuple[dt.datetime, dt.datetime]] = {}
    prior_id = options.resume_from_run_id
    current = store.con.execute(
        "SELECT started_at FROM dataset_runs WHERE dataset_id='sec_company_facts' AND run_id=?",
        [options.run_id],
    ).fetchone()
    successor_start = current[0] if current is not None else None
    while prior_id is not None:
        try:
            valid_uuid = str(uuid.UUID(prior_id)) == prior_id
        except (ValueError, AttributeError, TypeError):
            valid_uuid = False
        if (not valid_uuid or prior_id == options.run_id or prior_id in lineage
                or len(lineage) >= _MAX_LINEAGE):
            raise ValueError("companyfacts resume has invalid or cyclic dataset UUID lineage")
        row = store.con.execute(
            """SELECT source, status, started_at, finished_at, params_json
               FROM dataset_runs WHERE dataset_id='sec_company_facts' AND run_id=?""",
            [prior_id],
        ).fetchone()
        if (row is None or row[0] != "SEC companyfacts" or row[1] not in ("failed", "succeeded")
                or row[2] is None or row[3] is None or row[3] < row[2]):
            raise ValueError("companyfacts resume requires a terminal failed or source-incomplete dataset UUID")
        if row[1] == "succeeded" and not _source_incomplete_run(store, prior_id, row[2], row[3]):
            raise ValueError("companyfacts resume succeeded UUID lacks durable source-incomplete evidence")
        params = json.loads(row[4])
        if (not isinstance(params, dict)
                or any(key not in params or params[key] != value for key, value in expected.items())
                or sorted(set(params.get("concepts", []))) != sorted(set(options.concepts))):
            raise ValueError("companyfacts resume prior archive path, allowlist or scope does not match")
        if successor_start is not None and row[3] > successor_start:
            raise ValueError("companyfacts resume ancestor was not terminal before its successor")
        lineage[prior_id] = (row[2], row[3])
        successor_start = row[2]
        prior_id = params.get("resume_from_run_id")
    return lineage


def _projection(*, facts: bool) -> str:
    # Name and type of every common fact/point value are identical on both
    # sides. JSON struct encoding preserves field boundaries and NULLs.
    metric, filed = ("concept", "filed_date") if facts else ("metric", "as_of_date")
    return f"""struct_pack(
        source := source, security_id := security_id, metric := {metric},
        taxonomy := taxonomy, unit := unit, period_start := period_start,
        period_end := period_end, filed_date := {filed}, fiscal_year := fiscal_year,
        fiscal_period := fiscal_period, form := form, accession_number := accession_number,
        value := value, available_at := available_at, run_id := run_id)"""


def _digest_sums() -> str:
    # Four 64-bit limbs summed into HUGEINT: fixed aggregate state per identity,
    # multiplicity-sensitive, with cryptographic row fingerprints. This avoids
    # a full-history join/sort or collecting 20M point identifiers in Python.
    return ", ".join(
        f"sum(cast(cast('0x' || substr(digest, {offset}, 16) AS UBIGINT) AS HUGEINT))"
        for offset in (1, 17, 33, 49)
    )


def verify_companyfacts_resume(
    store: DuckDBStore, options: Any, *, archive_sha256: str,
    allowlist_sha256: str, target_ciks: set[str], source_url_prefix: str,
    unresolved_prefix: str, duplicate_members: int,
) -> tuple[dict[str, VerifiedCompanyFactsMember], dict[str, Any]]:
    """Verify loaded receipts before any issuer mutation; replay all other members.

    Python retains issuer/security aggregates, never fact/point rows. SQL scans
    each SEC surface once across all owners for shared-row fingerprints,
    grouping by security (with run ownership inside each row digest);
    thus several issuers sharing a historical security cannot hide a missing
    point by matching only its count. Digest sums are a multiset fingerprint,
    not a mathematical row-by-row equality proof (SHA-256 collision assumption).
    """
    lineage = _lineage(store, options)
    if duplicate_members:
        raise ValueError("companyfacts resume cannot verify duplicate archive member identities")
    receipts: dict[str, tuple[str, int]] = {}
    archive_bytes = options.companyfacts_zip.stat().st_size
    # Source receipts contain one small metadata object per archive member.
    store.con.execute(
        """SELECT source_url, sha256, byte_count, fetched_at, status, metadata_json
           FROM raw_source_files WHERE dataset_id='sec_company_facts' AND cache_path=?""",
        [str(options.companyfacts_zip)],
    )
    for source_url, sha, byte_count, fetched_at, status, raw_metadata in _rows(store):
        try:
            metadata = json.loads(raw_metadata)
        except (ValueError, TypeError):
            continue  # No usable proof: replay this member.
        if not isinstance(metadata, dict) or metadata.get("run_id") not in lineage:
            continue
        # Failed and unavailable receipts are observations, not completion.
        # Empty members are cheap to replay and re-establish their zero-row cleanup.
        if status not in ("loaded", "empty"):
            continue
        cik = metadata.get("cik")
        started, finished = lineage[metadata["run_id"]]
        if (not isinstance(cik, str) or cik not in target_ciks
                or source_url != f"{source_url_prefix}#CIK{cik}.json"
                or metadata.get("symbol") != f"CIK{cik}"
                or metadata.get("source_mode") != "bulk_zip"
                or sha != archive_sha256 or metadata.get("archive_sha256") != archive_sha256
                or metadata.get("allowlist_sha256") != allowlist_sha256
                or byte_count != archive_bytes
                or fetched_at is None or not started <= fetched_at <= finished):
            raise ValueError("companyfacts resume source receipt identity, digest or timing does not match")
        count = metadata.get("rows")
        if type(count) is not int or (count <= 0 if status == "loaded" else count != 0):
            raise ValueError("companyfacts resume source receipt has invalid row evidence")
        if status == "loaded":
            if cik in receipts:
                raise ValueError("companyfacts resume has ambiguous loaded member receipts")
            receipts[cik] = (metadata["run_id"], count)
    details: dict[str, Any] = {
        "resume_from_run_id": options.resume_from_run_id,
        "resume_lineage": list(reversed(lineage)),
        "completion_evidence": "matching_receipt_and_retained_fact_point_sha256_multisets",
        "resume_empty_policy": "replay_zero_row_cleanup",
        "resume_unavailable_policy": "reobserve_without_replacement",
        "resume_proof_connection_reopens": 0,
        "resume_verified_rows": 0, "previously_completed_targets": 0,
    }
    if not receipts:
        return {}, details
    LOGGER.info("companyfacts resume receipts=%d lineage_runs=%d; inventorying retained issuer counts",
                len(receipts), len(lineage))
    placeholders = ",".join("?" for _ in lineage)
    run_ids = list(lineage)
    # All spellings/owners contribute to counts so an extra legacy or foreign-run
    # fact cannot masquerade as a complete canonical issuer replacement.
    counts: dict[int, int] = {}
    store.con.execute("""SELECT try_cast(cik AS BIGINT), count(*) FROM sec_company_facts
        WHERE regexp_full_match(trim(cik), '[0-9]+') GROUP BY try_cast(cik AS BIGINT)""")
    for cik_number, count in _rows(store):
        counts[cik_number] = count
    # The result is exhausted and only issuer aggregates survive in Python.
    # Release metadata/count scan pages before opening the wider fact scan.
    details["resume_proof_connection_reopens"] += int(reopen_companyfacts_store(store))
    expected: dict[str, tuple[int, ...]] = {}
    issuer_groups: dict[str, set[str]] = {}
    issuer_rows: dict[str, int] = {}
    bad_issuers: set[str] = set()
    LOGGER.info("companyfacts resume aggregating retained fact fingerprints")
    store.con.execute(
        f"""WITH projected AS (
            SELECT cik, run_id, security_id, source_url,
                   sha256(to_json({_projection(facts=True)})) AS digest
            FROM sec_company_facts WHERE source='SEC companyfacts'
        ) SELECT cik, security_id, min(run_id), max(run_id), count(run_id),
                 min(source_url), max(source_url), count(*), {_digest_sums()}
          FROM projected GROUP BY cik, security_id""",
    )
    for cik, security_id, first_run, last_run, owned_count, first_url, last_url, count, *sums in _rows(store):
        stats = (count, *sums)
        expected[security_id] = tuple(
            a + b for a, b in zip(expected.get(security_id, (0,) * 5), stats, strict=True)
        )
        if cik in receipts:
            issuer_groups.setdefault(cik, set()).add(security_id)
            issuer_rows[cik] = issuer_rows.get(cik, 0) + count
            if (first_run != receipts[cik][0] or last_run != receipts[cik][0] or owned_count != count
                    or first_url != f"{source_url_prefix}#CIK{cik}.json"
                    or last_url != first_url):
                bad_issuers.add(cik)
    # Facts and points each read the complete retained history. Keeping both
    # phases on one connection needlessly retains the earlier scan's buffers.
    details["resume_proof_connection_reopens"] += int(reopen_companyfacts_store(store))
    actual: dict[str, tuple[int, ...]] = {}
    LOGGER.info("companyfacts resume fact_identity_groups=%d; aggregating retained point fingerprints", len(expected))
    store.con.execute(
        f"""WITH projected AS (
            SELECT run_id, security_id, symbol,
                   sha256(to_json({_projection(facts=False)})) AS digest
            FROM fundamental_points WHERE source='SEC companyfacts'
        ) SELECT security_id, count(*), {_digest_sums()},
                 count(symbol) FILTER (WHERE run_id IN ({placeholders}))
          FROM projected GROUP BY security_id""", run_ids,
    )
    for security_id, count, *rest in _rows(store):
        sums, symbols = rest[:4], rest[4]
        if symbols:
            continue  # Archive points cannot carry current ticker symbols.
        actual[security_id] = (count, *sums)
    details["resume_proof_connection_reopens"] += int(reopen_companyfacts_store(store))
    for cik, (_run_id, count) in receipts.items():
        if (cik in bad_issuers or counts.get(int(cik), 0) != count
                or issuer_rows.get(cik, 0) != count
                or any(expected[key] != actual.get(key) for key in issuer_groups.get(cik, ()))):
            raise ValueError(f"companyfacts resume retained fact/point evidence does not match CIK {cik}")
    unresolved: dict[str, dict[str, Any]] = {}
    LOGGER.info("companyfacts resume point_identity_groups=%d; recovering unresolved issuer summaries", len(actual))
    store.con.execute(
        f"""SELECT cik, arg_min(security_id, available_at), min(available_at),
                   count(*) FILTER (WHERE starts_with(security_id, ?)),
                   count(*) FILTER (WHERE entity_id IS NULL)
            FROM sec_company_facts WHERE run_id IN ({placeholders})
              AND (starts_with(security_id, ?) OR entity_id IS NULL)
            GROUP BY cik""", [unresolved_prefix, *run_ids, unresolved_prefix],
    )
    for cik, security_id, available_at, security_count, entity_count in _rows(store):
        if cik in receipts:
            if security_id is None or available_at is None:
                raise ValueError(f"companyfacts resume cannot recover filing availability for CIK {cik}")
            unresolved[cik] = {
                "cik": cik, "security_id": security_id, "available_at": available_at,
                "security_unresolved_count": security_count,
                "entity_unresolved_count": entity_count,
            }
    verified = {cik: VerifiedCompanyFactsMember(count, run_id, unresolved.get(cik))
                for cik, (run_id, count) in receipts.items()}
    details.update(previously_completed_targets=len(verified),
                   resume_verified_rows=sum(member.rows for member in verified.values()))
    return verified, details


def reopen_companyfacts_store(store: DuckDBStore) -> bool:
    """Recycle a configured persistent store after consumed proof or committed work."""
    if (str(store.path).startswith(":memory:") or not store.path.is_file()
            or store.analytical_memory_limit is None or store.analytical_threads is None):
        return False
    temporary = store.con.execute("""SELECT EXISTS (
        SELECT 1 FROM duckdb_tables() WHERE temporary AND NOT internal
    ) OR EXISTS (SELECT 1 FROM duckdb_views() WHERE temporary AND NOT internal)""").fetchone()
    if temporary is None or temporary[0]:
        raise RuntimeError("companyfacts cannot bound connection lifetime with caller-owned temporary objects")
    store.close()
    store.reopen()
    return True
