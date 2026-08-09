from __future__ import annotations

import datetime as dt
import hashlib
import logging
import os
import re
import shutil
import time
import zipfile
from collections.abc import Iterable, Sequence
from contextlib import suppress
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urljoin, urlparse

import requests

from .connection import DuckDBStore, resolve_data_dir
from .dataset import DatasetLoadResult
from .thirteenf import (
    SEC_13F_DATASETS_PAGE,
    SEC_USER_AGENT,
    ThirteenFDataSet,
    requests_session,
)
from .warehouse import file_sha256, record_source_file

ARCHIVE_DATASET_ID = "sec_13f_archive"
LOGGER = logging.getLogger(__name__)
REQUIRED_MEMBERS = (
    "SUBMISSION.tsv",
    "COVERPAGE.tsv",
    "SUMMARYPAGE.tsv",
    "INFOTABLE.tsv",
)
_MONTHS = {
    "jan": 1,
    "feb": 2,
    "mar": 3,
    "apr": 4,
    "may": 5,
    "jun": 6,
    "jul": 7,
    "aug": 8,
    "sep": 9,
    "oct": 10,
    "nov": 11,
    "dec": 12,
}
_RANGE_RE = re.compile(
    r"(?P<start_day>\d{2})(?P<start_month>[a-z]{3})(?P<start_year>\d{4})-"
    r"(?P<end_day>\d{2})(?P<end_month>[a-z]{3})(?P<end_year>\d{4})_form13f\.zip$",
    re.IGNORECASE,
)
_QUARTER_RE = re.compile(r"(?P<year>\d{4})q(?P<quarter>[1-4])_form13f\.zip$", re.IGNORECASE)


@dataclass(frozen=True, order=True)
class ThirteenFArchive:
    period_end: dt.date
    period_start: dt.date
    source_period: str
    url: str = field(compare=False)


@dataclass(frozen=True)
class ThirteenFArchiveBackfillOptions:
    start: dt.date = dt.date(2013, 4, 1)
    end: dt.date | None = None
    urls: tuple[str, ...] | None = None
    cache_dir: Path = field(default_factory=lambda: resolve_data_dir() / "cache" / "sec-13f")
    extract_dir: Path = field(default_factory=lambda: resolve_data_dir() / "staging" / "sec-13f")
    keep_extracted: bool = False
    compute_source_hash: bool = True
    request_timeout: int = 300
    max_download_attempts: int = 4
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None
    fail_fast: bool = True
    replace_loaded_archives: bool = False
    defer_indexes: bool = True


@dataclass(frozen=True)
class ThirteenFArchiveLoadResult:
    archive: ThirteenFArchive
    zip_path: Path
    submission_rows: int
    cover_page_rows: int
    summary_page_rows: int
    holding_rows: int
    sha256: str | None
    elapsed_seconds: float


def archive_from_url(url: str) -> ThirteenFArchive:
    name = Path(urlparse(url).path).name
    match = _RANGE_RE.search(name)
    if match:
        start = dt.date(
            int(match.group("start_year")),
            _MONTHS[match.group("start_month").lower()],
            int(match.group("start_day")),
        )
        end = dt.date(
            int(match.group("end_year")),
            _MONTHS[match.group("end_month").lower()],
            int(match.group("end_day")),
        )
        return ThirteenFArchive(end, start, Path(name).stem, url)

    match = _QUARTER_RE.search(name)
    if match:
        year = int(match.group("year"))
        quarter = int(match.group("quarter"))
        start_month = 3 * (quarter - 1) + 1
        start = dt.date(year, start_month, 1)
        next_quarter = dt.date(year + (quarter == 4), 1 if quarter == 4 else start_month + 3, 1)
        end = next_quarter - dt.timedelta(days=1)
        return ThirteenFArchive(end, start, Path(name).stem, url)

    raise ValueError(f"Unrecognized SEC Form 13F archive name: {name!r}")


def discover_archives(
    session: requests.Session,
    *,
    timeout: int,
    page_url: str = SEC_13F_DATASETS_PAGE,
) -> tuple[ThirteenFArchive, ...]:
    response = session.get(page_url, timeout=timeout)
    response.raise_for_status()
    hrefs = re.findall(r"href=[\"']([^\"']+?_form13f\.zip)[\"']", response.text, re.IGNORECASE)
    archives: dict[str, ThirteenFArchive] = {}
    for href in hrefs:
        url = urljoin(page_url, href)
        archive = archive_from_url(url)
        archives[archive.source_period] = archive
    if not archives:
        raise RuntimeError("Could not discover any Form 13F archives from the SEC data page")
    return tuple(sorted(archives.values()))


def select_archives(
    archives: Iterable[ThirteenFArchive],
    *,
    start: dt.date,
    end: dt.date | None,
) -> tuple[ThirteenFArchive, ...]:
    upper = end or dt.date.max
    return tuple(
        archive
        for archive in sorted(archives)
        if archive.period_end >= start and archive.period_start <= upper
    )


def _valid_archive(path: Path) -> bool:
    if not path.is_file() or path.stat().st_size == 0 or not zipfile.is_zipfile(path):
        return False
    with zipfile.ZipFile(path) as archive:
        return all(member in _archive_member_names(archive) for member in REQUIRED_MEMBERS)


def _archive_member_names(archive: zipfile.ZipFile) -> dict[str, str]:
    """Map required basenames to ZIP members across SEC archive layouts.

    SEC began wrapping the TSV files in a period-named directory in 2025.  Older
    archives keep them at the ZIP root, so selecting by normalized basename is
    the stable contract across both layouts.
    """
    names: dict[str, str] = {}
    required_by_upper = {member.upper(): member for member in REQUIRED_MEMBERS}
    for name in archive.namelist():
        basename = Path(name.replace("\\", "/")).name.upper()
        member = required_by_upper.get(basename)
        if member is not None:
            if member in names:
                raise ValueError(f"Duplicate required member {member!r} in archive")
            names[member] = name
    return names


def download_archive(
    session: requests.Session,
    archive: ThirteenFArchive,
    *,
    cache_dir: Path,
    timeout: int,
    max_attempts: int,
) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    destination = cache_dir / Path(urlparse(archive.url).path).name
    if _valid_archive(destination):
        return destination

    partial = destination.with_suffix(destination.suffix + ".part")
    if _valid_archive(partial):
        os.replace(partial, destination)
        return destination
    for attempt in range(1, max_attempts + 1):
        try:
            offset = partial.stat().st_size if partial.exists() else 0
            headers = {"Range": f"bytes={offset}-"} if offset else {}
            with session.get(archive.url, timeout=timeout, stream=True, headers=headers) as response:
                response.raise_for_status()
                append = offset > 0 and response.status_code == 206
                mode = "ab" if append else "wb"
                with partial.open(mode) as handle:
                    for chunk in response.iter_content(chunk_size=8 * 1024 * 1024):
                        if chunk:
                            handle.write(chunk)
            if not _valid_archive(partial):
                raise ValueError(f"Downloaded archive failed validation: {archive.url}")
            os.replace(partial, destination)
            return destination
        except Exception:
            if attempt >= max_attempts:
                raise
            time.sleep(min(2 ** (attempt - 1), 8))
    raise AssertionError("unreachable")


def _extract_members(zip_path: Path, destination: Path) -> dict[str, Path]:
    destination.mkdir(parents=True, exist_ok=True)
    extracted: dict[str, Path] = {}
    with zipfile.ZipFile(zip_path) as archive:
        member_names = _archive_member_names(archive)
        for member in REQUIRED_MEMBERS:
            info = archive.getinfo(member_names[member])
            target = destination / member
            if not target.is_file() or target.stat().st_size != info.file_size:
                partial = target.with_suffix(target.suffix + ".part")
                with archive.open(info) as source, partial.open("wb") as output:
                    shutil.copyfileobj(source, output, length=8 * 1024 * 1024)
                os.replace(partial, target)
            extracted[member] = target
    return extracted


def _read_csv(path: Path) -> str:
    escaped = path.resolve().as_posix().replace("'", "''")
    return f"read_csv('{escaped}', delim='\\t', header=true, all_varchar=true, strict_mode=false)"


def _date(column: str) -> str:
    return f"try_strptime(nullif(trim({column}), ''), '%d-%b-%Y')::DATE"


def _load_archive_tables(
    store: DuckDBStore,
    archive: ThirteenFArchive,
    members: dict[str, Path],
    *,
    run_id: str | None,
) -> tuple[int, int, int, int]:
    con = store.con
    source_period = archive.source_period
    submissions = _read_csv(members["SUBMISSION.tsv"])
    covers = _read_csv(members["COVERPAGE.tsv"])
    summaries = _read_csv(members["SUMMARYPAGE.tsv"])
    holdings = _read_csv(members["INFOTABLE.tsv"])

    with store.transaction():
        con.execute("DELETE FROM thirteenf_holdings WHERE source_period = ?", [source_period])
        for table in ("thirteenf_summary_pages", "thirteenf_cover_pages", "thirteenf_submissions"):
            con.execute(f"DELETE FROM {table} WHERE source_period = ?", [source_period])

        con.execute(
            f"""
            INSERT INTO thirteenf_submissions (
                accession_number, filing_date, submission_type, cik,
                period_of_report, source_period
            )
            SELECT
                trim(ACCESSION_NUMBER), {_date('FILING_DATE')}, trim(SUBMISSIONTYPE),
                lpad(trim(CIK), 10, '0'), {_date('PERIODOFREPORT')}, ?
            FROM {submissions}
            """,
            [source_period],
        )
        con.execute(
            f"""
            INSERT INTO thirteenf_cover_pages (
                accession_number, report_calendar_or_quarter, is_amendment,
                amendment_no, amendment_type, filing_manager_name,
                filing_manager_city, filing_manager_state_or_country, report_type,
                form_13f_file_number, crd_number, sec_file_number, source_period
            )
            SELECT
                trim(ACCESSION_NUMBER), {_date('REPORTCALENDARORQUARTER')},
                nullif(trim(ISAMENDMENT), ''), nullif(trim(AMENDMENTNO), ''),
                nullif(trim(AMENDMENTTYPE), ''), nullif(trim(FILINGMANAGER_NAME), ''),
                nullif(trim(FILINGMANAGER_CITY), ''),
                nullif(trim(FILINGMANAGER_STATEORCOUNTRY), ''), nullif(trim(REPORTTYPE), ''),
                nullif(trim(FORM13FFILENUMBER), ''), nullif(trim(CRDNUMBER), ''),
                nullif(trim(SECFILENUMBER), ''), ?
            FROM {covers}
            """,
            [source_period],
        )
        con.execute(
            f"""
            INSERT INTO thirteenf_summary_pages (
                accession_number, other_included_managers_count, table_entry_total,
                table_value_total, is_confidential_omitted, source_period
            )
            WITH normalized AS (
                SELECT
                    trim(ACCESSION_NUMBER) AS accession_number,
                    try_cast(nullif(trim(OTHERINCLUDEDMANAGERSCOUNT), '') AS BIGINT)
                        AS other_included_managers_count,
                    try_cast(nullif(trim(TABLEENTRYTOTAL), '') AS BIGINT) AS table_entry_total,
                    try_cast(nullif(trim(TABLEVALUETOTAL), '') AS DOUBLE) AS raw_table_value_total,
                    nullif(trim(ISCONFIDENTIALOMITTED), '') AS is_confidential_omitted
                FROM {summaries}
            )
            SELECT
                n.accession_number, n.other_included_managers_count, n.table_entry_total,
                n.raw_table_value_total * CASE
                    WHEN s.period_of_report <= DATE '2022-12-31' THEN 1000
                    ELSE 1
                END,
                n.is_confidential_omitted, ?
            FROM normalized n
            LEFT JOIN thirteenf_submissions s
              ON s.accession_number = n.accession_number
             AND s.source_period = ?
            """,
            [source_period, source_period],
        )
        con.execute(
            f"""
            INSERT INTO thirteenf_holdings (
                accession_number, security_id, infotable_sk, name_of_issuer,
                title_of_class, cusip, figi, value_usd, share_quantity,
                share_quantity_type, put_call, investment_discretion, other_manager,
                voting_auth_sole, voting_auth_shared, voting_auth_none, source_period,
                run_id
            )
            WITH normalized AS (
                SELECT
                    trim(ACCESSION_NUMBER) AS accession_number,
                    try_cast(nullif(trim(INFOTABLE_SK), '') AS BIGINT) AS infotable_sk,
                    nullif(trim(NAMEOFISSUER), '') AS name_of_issuer,
                    nullif(trim(TITLEOFCLASS), '') AS title_of_class,
                    upper(regexp_replace(trim(CUSIP), '[^0-9A-Za-z]', '', 'g')) AS cusip,
                    nullif(trim(FIGI), '') AS figi,
                    try_cast(nullif(trim(VALUE), '') AS DOUBLE) AS raw_value,
                    try_cast(nullif(trim(SSHPRNAMT), '') AS DOUBLE) AS share_quantity,
                    nullif(trim(SSHPRNAMTTYPE), '') AS share_quantity_type,
                    nullif(upper(trim(PUTCALL)), '') AS put_call,
                    nullif(trim(INVESTMENTDISCRETION), '') AS investment_discretion,
                    nullif(trim(OTHERMANAGER), '') AS other_manager,
                    try_cast(nullif(trim(VOTING_AUTH_SOLE), '') AS DOUBLE) AS voting_auth_sole,
                    try_cast(nullif(trim(VOTING_AUTH_SHARED), '') AS DOUBLE) AS voting_auth_shared,
                    try_cast(nullif(trim(VOTING_AUTH_NONE), '') AS DOUBLE) AS voting_auth_none
                FROM {holdings}
            )
            SELECT
                n.accession_number,
                'US-CUSIP-' || n.cusip,
                n.infotable_sk,
                n.name_of_issuer,
                n.title_of_class,
                n.cusip,
                n.figi,
                n.raw_value * CASE WHEN s.period_of_report <= DATE '2022-12-31' THEN 1000 ELSE 1 END,
                n.share_quantity,
                n.share_quantity_type,
                n.put_call,
                n.investment_discretion,
                n.other_manager,
                n.voting_auth_sole,
                n.voting_auth_shared,
                n.voting_auth_none,
                ?,
                ?
            FROM normalized n
            LEFT JOIN thirteenf_submissions s
              ON s.accession_number = n.accession_number
             AND s.source_period = ?
            WHERE n.cusip <> ''
            """,
            [source_period, run_id, source_period],
        )

        counts_list: list[int] = []
        for table in (
                "thirteenf_submissions",
                "thirteenf_cover_pages",
                "thirteenf_summary_pages",
                "thirteenf_holdings",
        ):
            row = con.execute(
                f"SELECT count(*) FROM {table} WHERE source_period = ?", [source_period]
            ).fetchone()
            if row is None:
                raise RuntimeError(f"Could not count loaded {table} rows")
            counts_list.append(int(row[0]))
        counts = tuple(counts_list)
    return counts  # type: ignore[return-value]


def _clean_extracted(paths: Sequence[Path], directory: Path) -> None:
    for path in paths:
        if path.is_file() and path.parent == directory:
            path.unlink()
    with suppress(OSError):
        directory.rmdir()


def load_archive(
    store: DuckDBStore,
    archive: ThirteenFArchive,
    options: ThirteenFArchiveBackfillOptions,
    *,
    session: requests.Session | None = None,
) -> ThirteenFArchiveLoadResult:
    started = time.perf_counter()
    own_session = session is None
    session = session or requests_session(options.user_agent)
    try:
        zip_path = download_archive(
            session,
            archive,
            cache_dir=options.cache_dir,
            timeout=options.request_timeout,
            max_attempts=options.max_download_attempts,
        )
    finally:
        if own_session:
            session.close()

    stage_dir = options.extract_dir / archive.source_period
    members = _extract_members(zip_path, stage_dir)
    sha256 = file_sha256(zip_path) if options.compute_source_hash else None
    try:
        counts = _load_archive_tables(store, archive, members, run_id=options.run_id)
        record_source_file(
            store,
            dataset_id=ARCHIVE_DATASET_ID,
            source_url=archive.url,
            cache_path=zip_path,
            status="loaded",
            sha256=sha256,
            metadata={
                "source_period": archive.source_period,
                "period_start": archive.period_start,
                "period_end": archive.period_end,
                "submission_rows": counts[0],
                "cover_page_rows": counts[1],
                "summary_page_rows": counts[2],
                "holding_rows": counts[3],
                "run_id": options.run_id,
            },
        )
    finally:
        if not options.keep_extracted:
            _clean_extracted(tuple(members.values()), stage_dir)

    return ThirteenFArchiveLoadResult(
        archive=archive,
        zip_path=zip_path,
        submission_rows=counts[0],
        cover_page_rows=counts[1],
        summary_page_rows=counts[2],
        holding_rows=counts[3],
        sha256=sha256,
        elapsed_seconds=time.perf_counter() - started,
    )


def backfill_archives(
    store: DuckDBStore,
    options: ThirteenFArchiveBackfillOptions,
) -> tuple[ThirteenFArchiveLoadResult, ...]:
    ThirteenFDataSet().ensure_schema(store)
    session = requests_session(options.user_agent)
    try:
        archives = (
            tuple(archive_from_url(url) for url in options.urls)
            if options.urls is not None
            else discover_archives(session, timeout=options.request_timeout)
        )
        selected = select_archives(archives, start=options.start, end=options.end)
        if not options.replace_loaded_archives:
            loaded_urls = {
                str(row[0])
                for row in store.con.execute(
                    """
                    SELECT DISTINCT source_url
                    FROM raw_source_files
                    WHERE dataset_id = ? AND status = 'loaded'
                    """,
                    [ARCHIVE_DATASET_ID],
                ).fetchall()
            }
            selected = tuple(archive for archive in selected if archive.url not in loaded_urls)
        results: list[ThirteenFArchiveLoadResult] = []
        defer_indexes = options.defer_indexes and len(selected) > 1
        if defer_indexes:
            store.con.execute("DROP INDEX IF EXISTS idx_thirteenf_holdings_cusip")
            store.con.execute("DROP INDEX IF EXISTS idx_thirteenf_submissions_accession")
        try:
            for index, archive in enumerate(selected, start=1):
                try:
                    result = load_archive(store, archive, options, session=session)
                    results.append(result)
                    LOGGER.info(
                        "loaded SEC 13F archive %s (%s/%s): %s holdings in %.1fs",
                        archive.source_period,
                        index,
                        len(selected),
                        result.holding_rows,
                        result.elapsed_seconds,
                    )
                except Exception as exc:
                    record_source_file(
                        store,
                        dataset_id=ARCHIVE_DATASET_ID,
                        source_url=archive.url,
                        status="failed",
                        metadata={
                            "source_period": archive.source_period,
                            "error": str(exc),
                            "run_id": options.run_id,
                        },
                    )
                    if options.fail_fast:
                        raise
            return tuple(results)
        finally:
            if defer_indexes:
                store.con.execute(
                    "CREATE INDEX IF NOT EXISTS idx_thirteenf_holdings_cusip ON thirteenf_holdings(cusip)"
                )
                store.con.execute(
                    "CREATE INDEX IF NOT EXISTS idx_thirteenf_submissions_accession "
                    "ON thirteenf_submissions(accession_number)"
                )
    finally:
        session.close()


def archive_backfill_result(results: Sequence[ThirteenFArchiveLoadResult]) -> DatasetLoadResult:
    return DatasetLoadResult(
        dataset_id=ARCHIVE_DATASET_ID,
        rows_loaded=sum(result.holding_rows for result in results),
        source=SEC_13F_DATASETS_PAGE,
        details={
            "archive_count": len(results),
            "submission_rows": sum(result.submission_rows for result in results),
            "holding_rows": sum(result.holding_rows for result in results),
            "source_periods": [result.archive.source_period for result in results],
        },
    )


def archive_manifest_sha256(archives: Sequence[ThirteenFArchive]) -> str:
    payload = "\n".join(
        f"{archive.period_start.isoformat()}|{archive.period_end.isoformat()}|{archive.source_period}|{archive.url}"
        for archive in sorted(archives)
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()
