from __future__ import annotations

import datetime as dt
import json
import logging
import re
from collections.abc import Generator, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

import pandas as pd

from ._submissions_archive import SubmissionsArchive
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT, sec_session
from .warehouse import cik_security_id, insert_frame, quality_check, record_source_file, symbol_key

SOURCE_NAME = "SEC submissions API"
SUBMISSIONS_URL = "https://data.sec.gov/submissions/CIK{cik}.json"
LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class EarningsReleaseCandidate:
    """An immutable CIK-owned 8-K Item 2.02 metadata candidate.

    ``sec_submissions`` is an index of filing metadata; it deliberately does
    not claim that an EX-99 document was present in the bulk archive.  The
    accession is the only input used by the archive-document fetcher.
    """

    cik: str
    accession_number: str
    filing_date: dt.date | None
    report_date: dt.date | None
    acceptance_datetime: dt.datetime | None
    acceptance_datetime_raw: str | None
    primary_document: str | None
    source_url: str
    run_id: str | None


def select_earnings_release_candidates(
    store: DuckDBStore,
    *,
    history_start: dt.date | None = None,
    history_end: dt.date | None = None,
    ciks: tuple[str, ...] | None = None,
    after: tuple[str, str] | None = None,
    limit: int | None = None,
) -> tuple[EarningsReleaseCandidate, ...]:
    """Return stable, issuer-generic Item 2.02 candidates from loaded metadata.

    This performs discovery only.  It intentionally retains the source CIK
    even when ``security_id`` is an unresolved placeholder: identity joins are
    a later, fact-time concern and may not be repaired with a current ticker.
    """

    if limit is not None and limit < 1:
        raise ValueError("earnings-release candidate limit must be positive")
    normalized_ciks = tuple(sorted({_normalized_cik(cik) for cik in ciks or ()}))
    if normalized_ciks:
        scope = pd.DataFrame({"cik": normalized_ciks})
        store.con.register("earnings_release_candidate_cik_scope", scope)
        scope_join = "JOIN earnings_release_candidate_cik_scope scope ON scope.cik = s.cik"
    else:
        scope_join = ""
    try:
        rows = store.con.execute(
            f"""
            SELECT
                s.cik,
                s.accession_number,
                s.filing_date,
                s.report_date,
                s.acceptance_datetime,
                s.acceptance_datetime_raw,
                s.primary_document,
                s.source_url,
                s.run_id
            FROM sec_submissions s
            {scope_join}
            WHERE upper(trim(s.form)) = '8-K'
              AND regexp_matches(coalesce(s.items, ''), '(^|[^0-9])2\\.02([^0-9]|$)')
              AND (? IS NULL OR s.report_date >= ?)
              AND (? IS NULL OR s.report_date <= ?)
              AND (? IS NULL OR s.cik > ? OR (s.cik = ? AND s.accession_number > ?))
            QUALIFY row_number() OVER (
                PARTITION BY s.cik, s.accession_number
                ORDER BY s.acceptance_datetime DESC NULLS LAST,
                         s.filing_date DESC NULLS LAST,
                         s.source_loaded_at DESC NULLS LAST,
                         s.source_url
            ) = 1
            ORDER BY s.cik, s.accession_number
            LIMIT coalesce(?, 9223372036854775807)
            """,
            [history_start, history_start, history_end, history_end,
             after[0] if after else None, after[0] if after else None,
             after[0] if after else None, after[1] if after else None, limit],
        ).fetchall()
    finally:
        if normalized_ciks:
            store.con.unregister("earnings_release_candidate_cik_scope")
    return tuple(EarningsReleaseCandidate(*row) for row in rows)


@dataclass(frozen=True)
class SecSubmissionsOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    ciks: tuple[str, ...] = ()
    forms: tuple[str, ...] | None = ("10-K", "10-Q", "8-K")
    include_history_files: bool = False
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None


def _parse_date(value: Any) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def _parse_acceptance(value: Any) -> pd.Timestamp | None:
    if not value:
        return None
    parsed = pd.to_datetime(value, errors="coerce", utc=True)
    if pd.isna(parsed):
        return None
    return cast(pd.Timestamp, parsed.tz_convert(None))


def _columnar_filings(payload: dict[str, Any], key: str = "recent") -> pd.DataFrame:
    recent = payload.get("filings", {}).get(key, {})
    if not recent:
        return pd.DataFrame()
    keys = list(recent.keys())
    if not keys:
        return pd.DataFrame()
    length = len(recent[keys[0]])
    rows = [{column: recent[column][index] for column in keys} for index in range(length)]
    return pd.DataFrame(rows)


def _normalize(
    frame: pd.DataFrame,
    *,
    security_id: str,
    cik: str,
    source_url: str,
    run_id: str | None,
    forms: set[str] | None,
) -> pd.DataFrame:
    if frame.empty:
        return frame
    if forms is not None:
        frame = frame[frame["form"].isin(forms)].reset_index(drop=True)
    if frame.empty:
        return frame
    return pd.DataFrame(
        {
            "security_id": security_id,
            "cik": cik,
            "accession_number": frame["accessionNumber"].str.strip(),
            "filing_date": frame["filingDate"].map(_parse_date),
            "report_date": frame["reportDate"].map(_parse_date),
            "acceptance_datetime": frame["acceptanceDateTime"].map(_parse_acceptance),
            # Keep SEC's original offset-bearing evidence alongside the legacy
            # normalized timestamp. A later source must reject old, naive rows
            # instead of relabeling a warehouse UTC value as an exact clock.
            "acceptance_datetime_raw": frame["acceptanceDateTime"].astype("string").str.strip(),
            "form": frame["form"].str.strip(),
            "primary_document": frame["primaryDocument"].str.strip(),
            "primary_doc_description": frame["primaryDocDescription"].str.strip(),
            "file_number": frame.get("fileNumber", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "film_number": frame.get("filmNumber", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "items": frame.get("items", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "size": pd.to_numeric(frame.get("size", pd.Series([None] * len(frame))), errors="coerce").astype("Int64"),
            "is_xbrl": frame.get("isXBRL", pd.Series([None] * len(frame))).map(lambda value: None if value in (None, "") else bool(int(value))),
            "is_inline_xbrl": frame.get("isInlineXBRL", pd.Series([None] * len(frame))).map(lambda value: None if value in (None, "") else bool(int(value))),
            "act": frame.get("act", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "source_url": source_url,
            "run_id": run_id,
        }
    )


def _normalized_cik(value: str | int) -> str:
    return f"{int(str(value).strip()):010d}"


def _targets(
    store: DuckDBStore,
    symbols: tuple[str, ...],
    ciks: tuple[str, ...] = (),
) -> list[tuple[str, str, str]]:
    frame = pd.DataFrame({"ticker": sorted({symbol_key(symbol) for symbol in symbols})})
    store.con.register("submission_symbol_lookup", frame)
    try:
        symbol_rows = store.con.execute(
            """
            SELECT l.ticker, t.cik, t.security_id
            FROM submission_symbol_lookup l
            JOIN sec_company_tickers t ON t.ticker = l.ticker
            QUALIFY row_number() OVER (
                PARTITION BY l.ticker
                ORDER BY t.source_loaded_at DESC, t.cik
            ) = 1
            """
        ).fetchall()
    finally:
        store.con.unregister("submission_symbol_lookup")

    cik_frame = pd.DataFrame({"cik": sorted({_normalized_cik(cik) for cik in ciks})})
    store.con.register("submission_cik_lookup", cik_frame)
    try:
        cik_rows = store.con.execute(
            """
            SELECT l.cik,t.ticker,t.security_id
            FROM submission_cik_lookup l
            LEFT JOIN sec_company_tickers t ON t.cik=l.cik
            QUALIFY row_number() OVER (
                PARTITION BY l.cik
                ORDER BY t.source_loaded_at DESC,t.ticker
            )=1
            """
        ).fetchall()
    finally:
        store.con.unregister("submission_cik_lookup")

    targets = {
        (cik, security_id): (ticker, cik, security_id)
        for ticker, cik, security_id in symbol_rows
    }
    for cik, ticker, security_id in cik_rows:
        resolved_security_id = security_id or cik_security_id(cik)
        targets[(cik, resolved_security_id)] = (
            ticker or f"CIK-{cik}",
            cik,
            resolved_security_id,
        )
    return sorted(targets.values(), key=lambda row: (row[0], row[1]))


class SecSubmissionsDataset(Dataset):
    dataset_id = "sec_submissions"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecSubmissionsOptions) -> DatasetLoadResult:
        session = sec_session(options.user_agent)
        forms = None if options.forms is None else set(options.forms)
        rows: list[pd.DataFrame] = []
        for symbol, cik, security_id in _targets(store, options.symbols, options.ciks):
            url = SUBMISSIONS_URL.format(cik=cik)
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            payload = response.json()
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={"symbol": symbol, "cik": cik},
            )
            rows.append(_normalize(_columnar_filings(payload), security_id=security_id, cik=cik, source_url=url, run_id=options.run_id, forms=forms))
            if options.include_history_files:
                for item in payload.get("filings", {}).get("files", []):
                    name = item.get("name")
                    if not name:
                        continue
                    history_url = f"https://data.sec.gov/submissions/{name}"
                    history_response = session.get(history_url, timeout=options.request_timeout)
                    history_response.raise_for_status()
                    rows.append(
                        _normalize(
                            _columnar_filings({"filings": {"recent": history_response.json()}}),
                            security_id=security_id,
                            cik=cik,
                            source_url=history_url,
                            run_id=options.run_id,
                            forms=forms,
                        )
                    )
        frame = pd.concat([frame for frame in rows if not frame.empty], ignore_index=True) if rows else pd.DataFrame()
        loaded = self._replace_rows(store, frame)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="sec_submissions",
            check_name="rows_loaded",
            status="passed" if loaded > 0 else "warning",
            observed_value=float(loaded),
            threshold_value=1.0,
            details={"symbols": options.symbols, "ciks": options.ciks, "forms": options.forms},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=loaded,
            source="SEC submissions API",
            run_id=options.run_id,
            details={"symbols": options.symbols, "ciks": options.ciks, "forms": options.forms},
        )

    def _replace_rows(self, store: DuckDBStore, frame: pd.DataFrame) -> int:
        return _replace_submission_rows(store, frame)


def _replace_submission_rows(store: DuckDBStore, frame: pd.DataFrame) -> int:
    if frame.empty:
        return 0
    with store.transaction():
        store.con.register("sec_submissions_load", frame)
        try:
            store.con.execute(
                """
                DELETE FROM sec_submissions AS dst
                USING sec_submissions_load AS src
                WHERE dst.security_id = src.security_id
                  AND dst.accession_number = src.accession_number
                """
            )
            insert_frame(store, frame, "sec_submissions", "sec_submissions_insert")
        finally:
            store.con.unregister("sec_submissions_load")
    return len(frame)


BULK_SOURCE_NAME = "SEC submissions bulk archive"
_BULK_MAIN_MEMBER = re.compile(r"^CIK(\d{10})\.json$")
_BULK_REOPEN_FLUSHES = 8
_BULK_PROGRESS_MEMBERS = 2000

# Every column _normalize reads positionally; history members omit some of them
# (notably primaryDocDescription), so bulk frames are padded before normalizing.
_NORMALIZE_REQUIRED_COLUMNS = (
    "accessionNumber",
    "filingDate",
    "reportDate",
    "acceptanceDateTime",
    "form",
    "primaryDocument",
    "primaryDocDescription",
)


@dataclass(frozen=True)
class SecSubmissionsBulkOptions:
    zip_path: Path
    forms: tuple[str, ...] | None = ("10-K", "10-Q", "8-K")
    ciks: tuple[str, ...] | None = None
    include_history_files: bool = True
    run_id: str | None = None
    batch_ciks: int = 2000
    resume_from_run_id: str | None = None


@dataclass(frozen=True)
class _VerifiedBulkPrefix:
    last_cik: str
    rows: int
    ciks: int
    main_members: int
    history_members: int
    lineage: tuple[str, ...]
    archive_sha256: str


def _bulk_resume_lineage(
    store: DuckDBStore, options: SecSubmissionsBulkOptions,
) -> tuple[tuple[str, ...], dt.datetime]:
    """Only explicitly linked, compatible failed attempts may supply a prefix."""
    if options.forms is not None or options.ciks is not None or not options.include_history_files:
        raise ValueError("SEC submissions resume requires all forms, all CIKs and history")
    expected = {
        "zip_path": str(options.zip_path), "forms": None, "ciks": None,
        "include_history_files": True, "batch_ciks": options.batch_ciks,
    }
    lineage: list[str] = []
    prior_id = options.resume_from_run_id
    earliest = None
    while prior_id is not None:
        if not isinstance(prior_id, str) or prior_id in lineage or prior_id == options.run_id:
            raise ValueError("SEC submissions resume has invalid or cyclic lineage")
        row = store.con.execute(
            """SELECT source, status, started_at, finished_at, params_json
               FROM dataset_runs WHERE run_id = ? AND dataset_id = 'sec_submissions'""",
            [prior_id],
        ).fetchone()
        if row is None or row[0] != BULK_SOURCE_NAME or row[1] != "failed" or row[3] is None:
            raise ValueError("SEC submissions resume requires a terminal failed bulk dataset run")
        params = json.loads(row[4])
        if not isinstance(params, dict) or any(key not in params or params[key] != value
                                               for key, value in expected.items()):
            raise ValueError("SEC submissions resume prior archive path or scope does not match")
        if earliest is not None and row[3] > earliest:
            raise ValueError("SEC submissions resume ancestor was not terminal before its successor")
        earliest = row[2]
        lineage.append(prior_id)
        prior_id = params.get("resume_from_run_id")
    if earliest is None:
        raise ValueError("SEC submissions resume requires a prior dataset run")
    return tuple(reversed(lineage)), earliest


def _bulk_retained_rows(store: DuckDBStore) -> Generator[tuple[Any, ...], None, None]:
    """Consume the active ordered query without materializing the filing corpus."""
    while rows := store.con.fetchmany(4096):
        yield from rows


def _bulk_source_keys(columnar: dict[str, Any]) -> Iterator[tuple[str, str | None]]:
    """Match _normalize's strip semantics without parsing every date again.

    Non-string accession/form values are deliberately unsupported for resume;
    a fresh load remains available instead of guessing pandas coercion behavior.
    """
    if not columnar:
        return
    if any(not isinstance(values, list) for values in columnar.values()):
        raise ValueError("SEC submissions resume found malformed columnar filings")
    length = len(next(iter(columnar.values())))
    if any(len(values) != length for values in columnar.values()):
        raise ValueError("SEC submissions resume found malformed columnar filings")
    accessions = columnar.get("accessionNumber", [None] * length)
    forms = columnar.get("form", [None] * length)
    for accession, form in zip(accessions, forms, strict=True):
        if not isinstance(accession, str) or (form is not None and not isinstance(form, str)):
            raise ValueError("SEC submissions resume requires string accession and form keys")
        yield accession.strip(), None if form is None else form.strip()


def _verify_bulk_prefix(
    store: DuckDBStore,
    options: SecSubmissionsBulkOptions,
    archive: SubmissionsArchive,
    security_map: dict[str, str],
) -> _VerifiedBulkPrefix:
    """Prove a contiguous committed prefix; a maximum CIK alone proves nothing.

    DuckDB sorts under the caller's existing budget/spill configuration. Python
    holds one CIK's deduplicated keys and at most 4096 retained filing rows.
    No writes occur until the complete candidate prefix has been verified.
    """
    lineage, started_at = _bulk_resume_lineage(store, options)
    source_stat = options.zip_path.stat()
    archive.assert_unchanged()
    archive_sha = archive.sha256
    receipt = store.con.execute(
        """SELECT 1 FROM raw_source_files
           WHERE dataset_id = 'sec_submissions' AND cache_path = ?
             AND sha256 = ? AND byte_count = ? AND fetched_at <= ?
             AND status = 'available' LIMIT 1""",
        [str(options.zip_path), archive_sha, source_stat.st_size, started_at],
    ).fetchone()
    if receipt is None:
        raise ValueError("SEC submissions resume archive hash lacks a matching pre-attempt receipt")
    placeholders = ",".join("?" for _ in lineage)
    groups = store.con.execute(
        f"""SELECT run_id, count(*), count(DISTINCT cik), min(cik), max(cik)
            FROM sec_submissions WHERE run_id IN ({placeholders}) GROUP BY run_id""",
        list(lineage),
    ).fetchall()
    by_run = {row[0]: row[1:] for row in groups}
    row_count = cik_count = 0
    last_cik = ""
    for prior_id in lineage:
        if prior_id not in by_run:
            continue  # An interrupted successor may not yet have committed a batch.
        rows, ciks, first, last = by_run[prior_id]
        if ciks % options.batch_ciks or first <= last_cik:
            raise ValueError("SEC submissions resume lineage does not end in ordered complete batches")
        row_count += rows
        cik_count += ciks
        last_cik = last
    if not row_count or f"CIK{last_cik}.json" not in archive:
        raise ValueError("SEC submissions resume has no committed archive boundary")
    LOGGER.info("SEC submissions resume verifying prefix: runs=%s rows=%d boundary=%s",
                lineage, row_count, last_cik)
    store.con.execute(
        f"""SELECT cik, security_id, accession_number, form, source_url
            FROM sec_submissions WHERE run_id IN ({placeholders})
            ORDER BY cik, accession_number""",
        list(lineage),
    )
    retained = _bulk_retained_rows(store)
    verified_rows = verified_ciks = processed = history_read = 0
    try:
        for member in archive.main_members(through=f"CIK{last_cik}.json"):
            cik = member[3:13]
            if cik > last_cik:
                break
            payload = json.loads(archive.read(member))
            expected: dict[str, tuple[str | None, str]] = {}

            def add_keys(
                columnar: dict[str, Any], source_member: str, target: dict[str, tuple[str | None, str]],
            ) -> None:
                for accession, form in _bulk_source_keys(columnar):
                    target.setdefault(accession, (form, f"{options.zip_path}!{source_member}"))

            add_keys(payload.get("filings", {}).get("recent", {}), member, expected)
            for item in payload.get("filings", {}).get("files", []):
                name = item.get("name")
                if not name:
                    continue
                if name not in archive:
                    raise ValueError("SEC submissions resume prefix is missing referenced history")
                add_keys(json.loads(archive.read(name)), name, expected)
                history_read += 1
            security_id = security_map.get(cik) or cik_security_id(cik)
            for accession in sorted(expected):
                form, source_url = expected[accession]
                actual = next(retained, None)
                if actual != (cik, security_id, accession, form, source_url):
                    raise ValueError(f"SEC submissions resume prefix rows do not match archive at CIK {cik}")
                verified_rows += 1
            verified_ciks += bool(expected)
            processed += 1
            if processed % _BULK_PROGRESS_MEMBERS == 0:
                LOGGER.info("SEC submissions resume verification: main_members=%d/%d verified_rows=%d",
                            processed, archive.main_member_count, verified_rows)
        if next(retained, None) is not None or (verified_rows, verified_ciks) != (row_count, cik_count):
            raise ValueError("SEC submissions resume retained rows exceed the verified prefix")
    finally:
        retained.close()
    final_stat = options.zip_path.stat()
    if (source_stat.st_size, source_stat.st_mtime_ns) != (final_stat.st_size, final_stat.st_mtime_ns):
        raise ValueError("SEC submissions resume archive changed during verification")
    LOGGER.info("SEC submissions resume verified: main_members=%d/%d rows=%d boundary=%s",
                processed, archive.main_member_count, verified_rows, last_cik)
    return _VerifiedBulkPrefix(last_cik, verified_rows, verified_ciks, processed,
                               history_read, lineage, archive_sha)


def _pad_normalize_columns(frame: pd.DataFrame) -> pd.DataFrame:
    if frame.empty:
        return frame
    for column in _NORMALIZE_REQUIRED_COLUMNS:
        if column not in frame.columns:
            frame[column] = None
    return frame


def _cik_security_map(store: DuckDBStore) -> dict[str, str]:
    rows = store.con.execute(
        """
        SELECT cik, security_id
        FROM sec_company_tickers
        QUALIFY row_number() OVER (
            PARTITION BY cik
            ORDER BY source_loaded_at DESC, ticker
        ) = 1
        """
    ).fetchall()
    return {cik: security_id for cik, security_id in rows if security_id}


def _reopen_bulk_store(store: DuckDBStore) -> bool:
    """Release retained state only in a configured, persistent, idle session.

    Call only after a batch has committed and all loader relations/cursors have
    been released. The load owns the session while running; caller-owned
    temporary tables or registered views prevent recycling that session.
    """
    if (
        str(store.path).startswith(":memory:")
        or not store.path.is_file()
        or store.analytical_memory_limit is None
        or store.analytical_threads is None
    ):
        # In particular, never destroy an anonymous or named in-memory database.
        return False
    temporary_objects = store.con.execute(
        """
        SELECT EXISTS (
            SELECT 1 FROM duckdb_tables() WHERE temporary AND NOT internal
        ) OR EXISTS (
            SELECT 1 FROM duckdb_views() WHERE temporary AND NOT internal
        )
        """
    ).fetchone()
    if temporary_objects is None or temporary_objects[0]:
        return False
    # close() checkpoints first; reopen() restores the recorded analytical caps
    # and base session settings without initializing or migrating the warehouse.
    store.close()
    store.reopen()
    return True


class SecSubmissionsBulkDataset(Dataset):
    """Load the complete SEC bulk ``submissions.zip`` corpus without API traffic.

    The official bulk archive contains one ``CIK##########.json`` member per
    entity (metadata plus the columnar ``filings.recent`` block) and separate
    ``CIK##########-submissions-NNN.json`` members holding the older filing
    history that the main member points at via ``filings.files``.
    """

    dataset_id = "sec_submissions"
    source_name = BULK_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecSubmissionsBulkOptions) -> DatasetLoadResult:
        if options.batch_ciks < 1:
            raise ValueError("SEC submissions batch_ciks must be positive")
        forms = None if options.forms is None else set(options.forms)
        cik_scope = (
            None
            if options.ciks is None
            else {_normalized_cik(cik) for cik in options.ciks}
        )
        security_map = _cik_security_map(store)

        loaded = 0
        ciks_loaded = 0
        main_members_processed = 0
        flush_count = 0
        history_members_read = 0
        missing_history_members = 0
        pending: list[pd.DataFrame] = []
        pending_ciks = 0
        verified_prefix: _VerifiedBulkPrefix | None = None
        verified_prior_rows = 0

        def log_progress() -> None:
            LOGGER.info(
                "SEC submissions bulk progress: main_members_processed=%d/%d "
                "ciks_loaded=%d committed_rows=%d flushes=%d",
                main_members_processed,
                main_member_count,
                ciks_loaded,
                loaded + verified_prior_rows,
                flush_count,
            )

        def flush() -> None:
            nonlocal pending, pending_ciks, loaded, flush_count
            if not pending:
                return
            frame = pd.concat(pending, ignore_index=True)
            frame = frame.drop_duplicates(
                subset=["security_id", "accession_number"], keep="first"
            ).reset_index(drop=True)
            pending = []
            pending_ciks = 0
            loaded += _replace_submission_rows(store, frame)
            flush_count += 1
            del frame
            log_progress()
            if flush_count % _BULK_REOPEN_FLUSHES == 0 and _reopen_bulk_store(store):
                LOGGER.info(
                    "SEC submissions bulk connection reopened: committed_rows=%d flushes=%d",
                    loaded,
                    flush_count,
                )

        with SubmissionsArchive(options.zip_path) as archive:
            main_member_count = archive.main_member_count
            if options.resume_from_run_id is not None:
                verified_prefix = _verify_bulk_prefix(
                    store, options, archive, security_map,
                )
                main_members_processed = verified_prefix.main_members
                ciks_loaded = verified_prefix.ciks
                history_members_read = verified_prefix.history_members
                verified_prior_rows = verified_prefix.rows
            after_member = None if verified_prefix is None else f"CIK{verified_prefix.last_cik}.json"
            for member in archive.main_members(after=after_member):
                cik = cast(re.Match[str], _BULK_MAIN_MEMBER.match(member)).group(1)
                if verified_prefix is not None and cik <= verified_prefix.last_cik:
                    continue
                if cik_scope is not None and cik not in cik_scope:
                    continue
                payload = json.loads(archive.read(member))
                security_id = security_map.get(cik) or cik_security_id(cik)
                source_url = f"{options.zip_path}!{member}"
                frames = [
                    _normalize(
                        _pad_normalize_columns(_columnar_filings(payload)),
                        security_id=security_id,
                        cik=cik,
                        source_url=source_url,
                        run_id=options.run_id,
                        forms=forms,
                    )
                ]
                if options.include_history_files:
                    for item in payload.get("filings", {}).get("files", []):
                        name = item.get("name")
                        if not name:
                            continue
                        if name not in archive:
                            missing_history_members += 1
                            continue
                        history_payload = json.loads(archive.read(name))
                        history_members_read += 1
                        frames.append(
                            _normalize(
                                _pad_normalize_columns(
                                    _columnar_filings(
                                        {"filings": {"recent": history_payload}}
                                    )
                                ),
                                security_id=security_id,
                                cik=cik,
                                source_url=f"{options.zip_path}!{name}",
                                run_id=options.run_id,
                                forms=forms,
                            )
                        )
                frames = [frame for frame in frames if not frame.empty]
                main_members_processed += 1
                if frames:
                    pending.extend(frames)
                    pending_ciks += 1
                    ciks_loaded += 1
                if pending_ciks >= options.batch_ciks:
                    flush()
                elif main_members_processed % _BULK_PROGRESS_MEMBERS == 0:
                    log_progress()
            archive.assert_unchanged()
        flush()
        log_progress()

        resume_details: dict[str, Any] = {}
        if verified_prefix is not None:
            resume_details = {
                "resume_from_run_id": options.resume_from_run_id,
                "verified_prior_run_ids": verified_prefix.lineage,
                "verified_prior_rows": verified_prefix.rows,
                "verified_prior_main_members": verified_prefix.main_members,
                "resume_after_cik": verified_prefix.last_cik,
                "archive_sha256": verified_prefix.archive_sha256,
            }
        coverage_details = {
            "main_members_processed": main_members_processed,
            "covered_rows": loaded + verified_prior_rows,
            "scope_complete": options.forms is None and cik_scope is None and options.include_history_files
                              and main_members_processed == main_member_count and missing_history_members == 0,
            **resume_details,
        }

        record_source_file(
            store,
            dataset_id=self.dataset_id,
            source_url=str(options.zip_path),
            status="fetched",
            metadata={
                "main_members": main_member_count,
                "ciks_loaded": ciks_loaded,
                "history_members_read": history_members_read,
                "missing_history_members": missing_history_members,
                **coverage_details,
            },
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="sec_submissions",
            check_name="rows_loaded",
            status="passed" if loaded + verified_prior_rows > 0 else "warning",
            observed_value=float(loaded + verified_prior_rows),
            threshold_value=1.0,
            details={
                "zip_path": str(options.zip_path),
                "forms": options.forms,
                "ciks": options.ciks,
                **coverage_details,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=loaded,
            source=BULK_SOURCE_NAME,
            run_id=options.run_id,
            details={
                "zip_path": str(options.zip_path),
                "main_members": main_member_count,
                "ciks_loaded": ciks_loaded,
                "history_members_read": history_members_read,
                "missing_history_members": missing_history_members,
                "forms": options.forms,
                **coverage_details,
            },
        )
