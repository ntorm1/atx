"""Optional Arelle validation sidecar with structured, revisioned findings."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.metadata
import json
import shutil
import subprocess
import tempfile
import uuid
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any
from xml.etree import ElementTree as ET

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT
from .source_cache import build_deterministic_source_archive, materialize_cached_source_url
from .warehouse import file_sha256, json_dumps, now_utc_naive, quality_check

PROCESSOR = "arelle"
CORE_VALIDATION_PROFILE = "xbrl21_calc11_round_to_nearest"
EFM_VALIDATION_PROFILE = "xbrl21_efm_calc11_round_to_nearest"


@dataclass(frozen=True)
class ArelleValidationOptions:
    accession_numbers: tuple[str, ...] | None = None
    max_filings: int = 1
    executable: str = "arelleCmdLine"
    internet_connectivity: str = "offline"
    processor_cache_dir: Path | None = None
    request_timeout: int = 120
    process_timeout: int = 900
    user_agent: str = SEC_USER_AGENT
    efm_plugin_path: Path | None = None
    taxonomy_package_paths: tuple[Path, ...] = ()
    use_catalog_taxonomy_packages: bool = True
    run_id: str | None = None


@dataclass(frozen=True)
class ArelleValidationResult:
    attempted_count: int
    succeeded_count: int
    failed_count: int
    unavailable_count: int
    finding_count: int
    error_count: int
    warning_count: int
    inconsistency_count: int
    processor_version: str
    validation_profile: str
    run_id: str


@dataclass(frozen=True)
class _ArelleFinding:
    ordinal: int
    severity: str
    message_code: str
    message: str
    message_attributes: dict[str, str]
    references: list[dict[str, Any]]


def _processor_version() -> str:
    try:
        return importlib.metadata.version("arelle-release")
    except importlib.metadata.PackageNotFoundError:
        return "unavailable"


def _processor_cache_dir(store: DuckDBStore, options: ArelleValidationOptions) -> Path:
    if options.processor_cache_dir is not None:
        return options.processor_cache_dir.expanduser().resolve()
    database_path = store.path.expanduser().resolve()
    return database_path.parent / f"{database_path.name}.arelle-cache"


def _validation_profile(options: ArelleValidationOptions) -> str:
    return (
        EFM_VALIDATION_PROFILE
        if options.efm_plugin_path is not None
        else CORE_VALIDATION_PROFILE
    )


def _taxonomy_package_manifest(options: ArelleValidationOptions) -> list[dict[str, Any]]:
    packages: list[Path] = []
    for configured_path in options.taxonomy_package_paths:
        path = configured_path.expanduser().resolve()
        if path.is_dir():
            packages.extend(sorted(path.glob("*.zip")))
        elif path.is_file():
            packages.append(path)
        else:
            raise ValueError(f"Taxonomy package path does not exist: {configured_path}")
    return [
        {
            "path": str(path),
            "sha256": file_sha256(path),
            "byte_count": path.stat().st_size,
        }
        for path in sorted(set(packages))
    ]


def _catalog_taxonomy_package_paths(
    store: DuckDBStore,
    filing: dict[str, Any],
) -> tuple[Path, ...]:
    rows = store.con.execute(
        """
        SELECT DISTINCT
            edge.package_key,
            package.processor_package_path,
            package.processor_package_sha256
        FROM xbrl_filing_taxonomy_packages edge
        JOIN xbrl_standard_taxonomy_package_revisions package
          ON package.package_revision_id=edge.package_revision_id
        WHERE edge.security_id=? AND edge.accession_number=?
          AND edge.is_latest_revision AND package.is_latest_revision
          AND package.status='cached'
          AND package.package_format IN (
              'oasis_taxonomy_package','atx_normalized_taxonomy_package'
          )
        ORDER BY edge.package_key,package.processor_package_path
        """,
        [filing["security_id"], filing["accession_number"]],
    ).fetchall()
    paths: list[Path] = []
    for _package_key, path_text, sha256 in rows:
        path = Path(str(path_text)).expanduser().resolve()
        if not path.is_file() or file_sha256(path) != str(sha256):
            raise ValueError(
                f"Catalog taxonomy package failed SHA-256 verification: {path}"
            )
        paths.append(path)
    return tuple(paths)


def _target_filings(
    store: DuckDBStore,
    options: ArelleValidationOptions,
) -> list[dict[str, Any]]:
    store.con.register(
        "arelle_accession_filter",
        pd.DataFrame(
            {"accession_number": list(options.accession_numbers or ())},
            dtype="string",
        ),
    )
    try:
        rows = store.con.execute(
            """
            WITH source_documents AS (
                SELECT security_id,accession_number,source_url
                FROM xbrl_filing_contexts
                UNION
                SELECT security_id,accession_number,source_url
                FROM xbrl_filing_facts
            ), grouped_documents AS (
                SELECT
                    security_id,
                    accession_number,
                    list(DISTINCT source_url ORDER BY source_url) AS source_urls
                FROM source_documents
                GROUP BY 1,2
            ), context_format AS (
                SELECT
                    security_id,
                    accession_number,
                    arg_max(instance_format,source_loaded_at) AS instance_format
                FROM xbrl_filing_contexts
                GROUP BY 1,2
            )
            SELECT
                submission.security_id,
                coalesce(identifier.symbol,submission.security_id) AS symbol,
                submission.cik,
                submission.accession_number,
                format.instance_format,
                documents.source_urls
            FROM sec_submissions submission
            JOIN grouped_documents documents
              USING (security_id,accession_number)
            JOIN context_format format
              USING (security_id,accession_number)
            LEFT JOIN security_identifiers identifier
              ON identifier.id_type='cik'
             AND identifier.id_value=submission.cik
            WHERE (
                ? OR submission.accession_number IN (
                    SELECT accession_number FROM arelle_accession_filter
                )
            )
            QUALIFY row_number() OVER (
                PARTITION BY submission.security_id,submission.accession_number
                ORDER BY identifier.updated_at DESC NULLS LAST
            )=1
            ORDER BY submission.acceptance_datetime DESC NULLS LAST,
                     submission.accession_number DESC
            LIMIT ?
            """,
            [not bool(options.accession_numbers), options.max_filings],
        ).fetchall()
    finally:
        store.con.unregister("arelle_accession_filter")
    return [
        {
            "security_id": str(row[0]),
            "symbol": None if row[1] is None else str(row[1]),
            "cik": str(row[2]),
            "accession_number": str(row[3]),
            "instance_format": str(row[4]),
            "source_urls": tuple(str(value) for value in row[5]),
        }
        for row in rows
    ]


def _entrypoint(filing: dict[str, Any]) -> str:
    if filing.get("inline_archive_path"):
        return str(filing["inline_archive_path"])
    source_urls = tuple(sorted(set(filing["source_urls"])))
    if not source_urls:
        raise ValueError(f"No source documents found for {filing['accession_number']}")
    if filing["instance_format"] == "inline_xbrl" and len(source_urls) > 1:
        return json.dumps(
            [{"ixds": [{"file": source_url} for source_url in source_urls]}],
            separators=(",", ":"),
        )
    return str(source_urls[0])


def _materialize_entrypoint_sources(
    store: DuckDBStore,
    *,
    filing: dict[str, Any],
    materialized_root: Path,
) -> dict[str, Any]:
    materialized_urls: list[str] = []
    for source_url in filing["source_urls"]:
        rows = store.con.execute(
            """
            SELECT cache_path,sha256
            FROM raw_source_files
            WHERE dataset_id='xbrl_filing_contexts' AND source_url=?
              AND cache_path IS NOT NULL AND sha256 IS NOT NULL
            ORDER BY fetched_at DESC
            """,
            [source_url],
        ).fetchall()
        local_path: Path | None = None
        for cache_path_text, sha256 in rows:
            try:
                materialized = materialize_cached_source_url(
                    materialized_root,
                    source_url=str(source_url),
                    cache_path=Path(str(cache_path_text)),
                    expected_sha256=str(sha256),
                )
            except ValueError:
                continue
            local_path = materialized.materialized_path
            break
        materialized_urls.append(local_path.as_posix() if local_path is not None else str(source_url))
    original_urls = tuple(str(value) for value in filing["source_urls"])
    materialized_filing = {
        **filing,
        "source_urls": tuple(materialized_urls),
        "original_source_urls": original_urls,
    }
    if not original_urls:
        return materialized_filing
    directory_prefix = original_urls[0].rsplit("/", 1)[0] + "/"
    if any(not source_url.startswith(directory_prefix) for source_url in original_urls):
        return materialized_filing

    entrypoint_names = {source_url.rsplit("/", 1)[-1] for source_url in original_urls}
    rows = store.con.execute(
        """
        SELECT source_url,cache_path,sha256
        FROM raw_source_files
        WHERE dataset_id='xbrl_filing_contexts'
          AND starts_with(source_url,?)
          AND cache_path IS NOT NULL AND sha256 IS NOT NULL
        ORDER BY source_url,fetched_at DESC
        """,
        [directory_prefix],
    ).fetchall()
    members: list[tuple[str, Path, str]] = []
    seen_urls: set[str] = set()
    for source_url_value, cache_path_text, sha256 in rows:
        source_url = str(source_url_value)
        if source_url in seen_urls:
            continue
        member_name = source_url.removeprefix(directory_prefix)
        lower = member_name.lower()
        if member_name not in entrypoint_names and not (
            lower.endswith(".xsd")
            or lower.endswith(("_cal.xml", "_def.xml", "_lab.xml", "_pre.xml"))
        ):
            continue
        try:
            materialize_cached_source_url(
                materialized_root,
                source_url=source_url,
                cache_path=Path(str(cache_path_text)),
                expected_sha256=str(sha256),
            )
        except ValueError:
            continue
        members.append((member_name, Path(str(cache_path_text)), str(sha256)))
        seen_urls.add(source_url)
    if filing["instance_format"] != "inline_xbrl" or len(original_urls) < 2:
        return materialized_filing
    if not entrypoint_names.issubset({member[0] for member in members}):
        return materialized_filing
    archive = build_deterministic_source_archive(
        materialized_root / "archives",
        tuple(members),
    )
    return {
        **materialized_filing,
        "inline_archive_path": str(archive.archive_path),
        "inline_archive_manifest_sha256": archive.manifest_sha256,
        "inline_archive_member_count": archive.member_count,
    }


def _build_command(
    *,
    executable: str,
    entrypoint: str,
    log_path: Path,
    cache_dir: Path,
    options: ArelleValidationOptions,
) -> list[str]:
    command = [
        executable,
        "--file",
        entrypoint,
        "--validate",
        "--calc",
        "c11r",
        "--internetConnectivity",
        options.internet_connectivity,
        "--internetTimeout",
        str(options.request_timeout),
        "--internetRecheck",
        "never",
        "--cacheDirectory",
        str(cache_dir),
        "--httpUserAgent",
        options.user_agent,
        "--disablePersistentConfig",
        "--logFile",
        str(log_path),
        "--logFileMode",
        "w",
        "--logLevel",
        "warning",
    ]
    plugins: list[str] = []
    if options.efm_plugin_path is not None:
        command.append("--efm")
        plugins.append(str(options.efm_plugin_path.expanduser().resolve()))
    for package_path in options.taxonomy_package_paths:
        command.extend(["--packages", str(package_path.expanduser().resolve())])
    if entrypoint.startswith("[{") or entrypoint.lower().endswith(".zip"):
        plugins.append("inlineXbrlDocumentSet")
    if plugins:
        command.extend(["--plugins", "|".join(plugins)])
    return command


def _execute_arelle(command: list[str], *, timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def parse_arelle_xml_log(content: bytes) -> list[_ArelleFinding]:
    root = ET.fromstring(content)
    findings: list[_ArelleFinding] = []
    for ordinal, entry in enumerate(root.findall("entry"), start=1):
        message_element = entry.find("message")
        message = "" if message_element is None else "".join(message_element.itertext()).strip()
        references: list[dict[str, Any]] = []
        for reference in entry.findall("ref"):
            references.append(
                {
                    "attributes": dict(sorted(reference.attrib.items())),
                    "properties": [
                        dict(sorted(prop.attrib.items()))
                        for prop in reference.findall("property")
                    ],
                }
            )
        findings.append(
            _ArelleFinding(
                ordinal=ordinal,
                severity=str(entry.get("level") or "unknown").lower(),
                message_code=str(entry.get("code") or "unknown"),
                message=message or "Arelle emitted an empty validation message",
                message_attributes=(
                    {} if message_element is None else dict(sorted(message_element.attrib.items()))
                ),
                references=references,
            )
        )
    return findings


def _is_error(severity: str) -> bool:
    return severity.startswith("error") or severity == "assertion-not-satisfied"


def _is_warning(severity: str) -> bool:
    return severity.startswith("warning")


def _semantic_outcome(
    *,
    status: str,
    findings: list[_ArelleFinding],
) -> tuple[str, str]:
    if status == "running":
        return "not_evaluated", "not_evaluated"
    if status == "failed":
        return "not_evaluated", "processor_failed"
    if status == "unavailable":
        return "not_evaluated", "processor_unavailable"
    incomplete_dts_codes = {
        "IOerror",
        "arelle:packageLoadingError",
        "xbrl:schemaImportMissing",
    }
    if any(
        finding.message_code in incomplete_dts_codes
        or finding.message_code.startswith("tpe:")
        for finding in findings
    ):
        return "incomplete", "incomplete_dts"
    if any(_is_error(finding.severity) for finding in findings):
        return "resolved", "validation_errors"
    if any(
        _is_warning(finding.severity) or finding.severity == "inconsistency"
        for finding in findings
    ):
        return "resolved", "validation_issues"
    return "resolved", "valid"


def _claim_run(
    store: DuckDBStore,
    *,
    filing: dict[str, Any],
    processor_run_id: str,
    processor_version: str,
    validation_profile: str,
    entrypoint: str,
    command: list[str],
    cache_dir: Path,
    taxonomy_package_manifest: list[dict[str, Any]],
    run_id: str,
    started_at: dt.datetime,
) -> None:
    with store.transaction():
        store.con.execute(
            """
            UPDATE xbrl_processor_findings
            SET is_latest_revision=false
            WHERE processor_run_id IN (
                SELECT processor_run_id
                FROM xbrl_processor_runs
                WHERE security_id=? AND accession_number=?
                  AND processor=? AND processor_version=?
                  AND validation_profile=? AND is_latest_revision
            )
            """,
            [
                filing["security_id"],
                filing["accession_number"],
                PROCESSOR,
                processor_version,
                validation_profile,
            ],
        )
        store.con.execute(
            """
            UPDATE xbrl_processor_runs
            SET is_latest_revision=false
            WHERE security_id=? AND accession_number=?
              AND processor=? AND processor_version=?
              AND validation_profile=? AND is_latest_revision
            """,
            [
                filing["security_id"],
                filing["accession_number"],
                PROCESSOR,
                processor_version,
                validation_profile,
            ],
        )
        store.con.execute(
            """
            INSERT INTO xbrl_processor_runs (
                processor_run_id,processor,processor_version,validation_profile,
                security_id,symbol,cik,accession_number,instance_format,
                entrypoint_json,command_json,internet_connectivity,
                processor_cache_dir,taxonomy_packages_json,
                filing_archive_manifest_sha256,filing_archive_member_count,
                dts_resolution_status,validation_outcome,status,
                started_at,as_of_date,available_at,
                is_latest_revision,run_id,source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'not_evaluated',
                      'not_evaluated','running',?,?,?,true,?,?)
            """,
            [
                processor_run_id,
                PROCESSOR,
                processor_version,
                validation_profile,
                filing["security_id"],
                filing["symbol"],
                filing["cik"],
                filing["accession_number"],
                filing["instance_format"],
                json_dumps(
                    {
                        "file": entrypoint,
                        "source_urls": filing.get(
                            "original_source_urls", filing["source_urls"]
                        ),
                        "archive_manifest_sha256": filing.get(
                            "inline_archive_manifest_sha256"
                        ),
                        "archive_member_count": filing.get(
                            "inline_archive_member_count"
                        ),
                    }
                ),
                json_dumps(command),
                options_internet_connectivity(command),
                str(cache_dir),
                json_dumps(taxonomy_package_manifest),
                filing.get("inline_archive_manifest_sha256"),
                filing.get("inline_archive_member_count"),
                started_at,
                started_at.date(),
                started_at,
                run_id,
                started_at,
            ],
        )


def options_internet_connectivity(command: list[str]) -> str:
    index = command.index("--internetConnectivity")
    return command[index + 1]


def _finding_rows(
    *,
    findings: list[_ArelleFinding],
    filing: dict[str, Any],
    processor_run_id: str,
    processor_version: str,
    validation_profile: str,
    completed_at: dt.datetime,
    run_id: str,
) -> list[list[Any]]:
    rows: list[list[Any]] = []
    for finding in findings:
        identity = "|".join(
            [
                processor_run_id,
                str(finding.ordinal),
                finding.severity,
                finding.message_code,
                finding.message,
                json_dumps(finding.references),
            ]
        )
        rows.append(
            [
                hashlib.sha256(identity.encode()).hexdigest(),
                processor_run_id,
                PROCESSOR,
                processor_version,
                validation_profile,
                filing["security_id"],
                filing["symbol"],
                filing["cik"],
                filing["accession_number"],
                finding.severity,
                finding.message_code,
                finding.message,
                json_dumps(finding.message_attributes),
                json_dumps(finding.references),
                finding.ordinal,
                completed_at.date(),
                completed_at,
                run_id,
                completed_at,
            ]
        )
    return rows


def _finish_run(
    store: DuckDBStore,
    *,
    processor_run_id: str,
    status: str,
    exit_code: int | None,
    findings: list[_ArelleFinding],
    filing: dict[str, Any],
    processor_version: str,
    validation_profile: str,
    completed_at: dt.datetime,
    run_id: str,
    error: Exception | None = None,
) -> None:
    rows = _finding_rows(
        findings=findings,
        filing=filing,
        processor_run_id=processor_run_id,
        processor_version=processor_version,
        validation_profile=validation_profile,
        completed_at=completed_at,
        run_id=run_id,
    )
    error_count = sum(_is_error(finding.severity) for finding in findings)
    warning_count = sum(_is_warning(finding.severity) for finding in findings)
    inconsistency_count = sum(finding.severity == "inconsistency" for finding in findings)
    dts_resolution_status, validation_outcome = _semantic_outcome(
        status=status,
        findings=findings,
    )
    with store.transaction():
        if rows:
            store.con.executemany(
                """
                INSERT INTO xbrl_processor_findings (
                    finding_id,processor_run_id,processor,processor_version,
                    validation_profile,security_id,symbol,cik,accession_number,
                    severity,message_code,message,message_attributes_json,
                    references_json,ordinal,as_of_date,available_at,run_id,
                    source_loaded_at,is_latest_revision
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,true)
                """,
                rows,
            )
        store.con.execute(
            """
            UPDATE xbrl_processor_runs
            SET status=?,exit_code=?,finding_count=?,error_count=?,warning_count=?,
                inconsistency_count=?,dts_resolution_status=?,validation_outcome=?,
                completed_at=?,error_type=?,error_message=?,source_loaded_at=?
            WHERE processor_run_id=?
            """,
            [
                status,
                exit_code,
                len(findings),
                error_count,
                warning_count,
                inconsistency_count,
                dts_resolution_status,
                validation_outcome,
                completed_at,
                None if error is None else type(error).__name__,
                None if error is None else str(error)[:4000],
                completed_at,
                processor_run_id,
            ],
        )


def run_arelle_validation(
    store: DuckDBStore,
    options: ArelleValidationOptions | None = None,
) -> ArelleValidationResult:
    options = options or ArelleValidationOptions()
    if options.max_filings < 1:
        raise ValueError("max_filings must be positive")
    if options.internet_connectivity not in {"online", "offline"}:
        raise ValueError("internet_connectivity must be online or offline")
    if options.request_timeout < 1 or options.process_timeout < 1:
        raise ValueError("timeouts must be positive")
    if options.efm_plugin_path is not None and not options.efm_plugin_path.exists():
        raise ValueError(f"EFM plugin path does not exist: {options.efm_plugin_path}")
    store.initialize()
    run_id = options.run_id or f"arelle-validation-{uuid.uuid4()}"
    processor_version = _processor_version()
    validation_profile = _validation_profile(options)
    executable = shutil.which(options.executable)
    filings = _target_filings(store, options)
    cache_dir = _processor_cache_dir(store, options)
    cache_dir.mkdir(parents=True, exist_ok=True)
    materialized_root = cache_dir.parent / "atx-xbrl-filing-packages"
    attempted = succeeded = failed = unavailable = 0
    finding_count = error_count = warning_count = inconsistency_count = 0

    for filing in filings:
        attempted += 1
        processor_run_id = str(uuid.uuid4())
        entrypoint_filing = _materialize_entrypoint_sources(
            store,
            filing=filing,
            materialized_root=materialized_root,
        )
        entrypoint = _entrypoint(entrypoint_filing)
        catalog_package_paths = (
            _catalog_taxonomy_package_paths(store, filing)
            if options.use_catalog_taxonomy_packages
            else ()
        )
        effective_options = replace(
            options,
            taxonomy_package_paths=tuple(
                dict.fromkeys((*options.taxonomy_package_paths, *catalog_package_paths))
            ),
        )
        taxonomy_package_manifest = _taxonomy_package_manifest(effective_options)
        started_at = now_utc_naive()
        with tempfile.TemporaryDirectory(prefix="atx-arelle-") as temporary_dir:
            log_path = Path(temporary_dir) / "validation-log.xml"
            command = _build_command(
                executable=executable or options.executable,
                entrypoint=entrypoint,
                log_path=log_path,
                cache_dir=cache_dir,
                options=effective_options,
            )
            _claim_run(
                store,
                filing=entrypoint_filing,
                processor_run_id=processor_run_id,
                processor_version=processor_version,
                validation_profile=validation_profile,
                entrypoint=entrypoint,
                command=command,
                cache_dir=cache_dir,
                taxonomy_package_manifest=taxonomy_package_manifest,
                run_id=run_id,
                started_at=started_at,
            )
            if executable is None or processor_version == "unavailable":
                error = RuntimeError(
                    "Arelle is unavailable; install the atx-db[xbrl] optional extra"
                )
                _finish_run(
                    store,
                    processor_run_id=processor_run_id,
                    status="unavailable",
                    exit_code=None,
                    findings=[],
                    filing=filing,
                    processor_version=processor_version,
                    validation_profile=validation_profile,
                    completed_at=now_utc_naive(),
                    run_id=run_id,
                    error=error,
                )
                unavailable += 1
                continue
            try:
                completed = _execute_arelle(command, timeout=options.process_timeout)
                findings = (
                    parse_arelle_xml_log(log_path.read_bytes()) if log_path.is_file() else []
                )
                completed_at = now_utc_naive()
                if completed.returncode != 0:
                    stderr = completed.stderr.strip() or completed.stdout.strip()
                    error = RuntimeError(
                        f"Arelle exited {completed.returncode}: {stderr[:3500]}"
                    )
                    _finish_run(
                        store,
                        processor_run_id=processor_run_id,
                        status="failed",
                        exit_code=completed.returncode,
                        findings=findings,
                        filing=filing,
                        processor_version=processor_version,
                        validation_profile=validation_profile,
                        completed_at=completed_at,
                        run_id=run_id,
                        error=error,
                    )
                    failed += 1
                else:
                    _finish_run(
                        store,
                        processor_run_id=processor_run_id,
                        status="succeeded",
                        exit_code=completed.returncode,
                        findings=findings,
                        filing=filing,
                        processor_version=processor_version,
                        validation_profile=validation_profile,
                        completed_at=completed_at,
                        run_id=run_id,
                    )
                    succeeded += 1
                finding_count += len(findings)
                error_count += sum(_is_error(finding.severity) for finding in findings)
                warning_count += sum(_is_warning(finding.severity) for finding in findings)
                inconsistency_count += sum(
                    finding.severity == "inconsistency" for finding in findings
                )
            except Exception as exc:
                _finish_run(
                    store,
                    processor_run_id=processor_run_id,
                    status="failed",
                    exit_code=None,
                    findings=[],
                    filing=filing,
                    processor_version=processor_version,
                    validation_profile=validation_profile,
                    completed_at=now_utc_naive(),
                    run_id=run_id,
                    error=exc,
                )
                failed += 1

    return ArelleValidationResult(
        attempted_count=attempted,
        succeeded_count=succeeded,
        failed_count=failed,
        unavailable_count=unavailable,
        finding_count=finding_count,
        error_count=error_count,
        warning_count=warning_count,
        inconsistency_count=inconsistency_count,
        processor_version=processor_version,
        validation_profile=validation_profile,
        run_id=run_id,
    )


class ArelleValidationDataset(Dataset):
    dataset_id = "xbrl_processor_runs"
    source_name = "Arelle XBRL/EFM/Calculation 1.1 processor"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: ArelleValidationOptions,
    ) -> DatasetLoadResult:
        result = run_arelle_validation(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="xbrl_processor_runs",
            check_name="processor_execution_failures",
            status=(
                "passed"
                if result.failed_count == 0 and result.unavailable_count == 0
                else "warning"
            ),
            observed_value=float(result.failed_count + result.unavailable_count),
            threshold_value=0.0,
            details={
                "attempted": result.attempted_count,
                "succeeded": result.succeeded_count,
                "findings": result.finding_count,
                "errors": result.error_count,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=result.finding_count,
            source=self.source_name,
            run_id=result.run_id,
            details={
                "attempted": result.attempted_count,
                "succeeded": result.succeeded_count,
                "failed": result.failed_count,
                "unavailable": result.unavailable_count,
                "findings": result.finding_count,
                "errors": result.error_count,
                "warnings": result.warning_count,
                "inconsistencies": result.inconsistency_count,
                "processor_version": result.processor_version,
                "validation_profile": result.validation_profile,
            },
        )
