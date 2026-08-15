"""Discover and cache official standard-taxonomy packages required by SEC filings."""

from __future__ import annotations

import datetime as dt
import hashlib
import io
import os
import re
import uuid
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urldefrag, urljoin, urlsplit

import pandas as pd
from lxml import etree  # type: ignore[import-untyped]
from requests import RequestException

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT, sec_session
from .source_cache import (
    cache_source_payload,
    materialize_cached_source_url,
    read_cached_source_payload,
)
from .warehouse import now_utc_naive, quality_check, record_source_file

SOURCE_NAME = "Official FASB, SEC, and XBRL US taxonomy packages"
_FASB_IMPORT = re.compile(
    r"^https?://xbrl\.fasb\.org/(?P<family>us-gaap|srt)/(?P<version>[^/]+)/",
    flags=re.IGNORECASE,
)
_SEC_IMPORT = re.compile(
    r"^https?://xbrl\.sec\.gov/(?P<family>[^/]+)/(?P<version>[^/]+)/",
    flags=re.IGNORECASE,
)
_XBRL_US_GAAP_REFERENCE = re.compile(
    r"^https?://taxonomies\.xbrl\.us/us-gaap/(?P<version>\d{4})/",
    flags=re.IGNORECASE,
)
_SEC_DIRECTORY_ONLY_PACKAGES = {("exch", "2018"), ("stpr", "2018")}
_SEC_YEAR_ONLY_ARCHIVES = {("dei", "2018")}
_TAXONOMY_DOCUMENT_SUFFIXES = {".xsd", ".xml"}


@dataclass(frozen=True)
class XbrlTaxonomyPackageOptions:
    accession_numbers: tuple[str, ...] | None = None
    max_filings: int = 10
    source_cache_dir: Path | None = None
    package_dir: Path | None = None
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None
    fail_fast: bool = False


@dataclass(frozen=True)
class XbrlTaxonomyPackageFailure:
    package_key: str
    source_url: str
    failure_stage: str
    error_type: str
    error_message: str


@dataclass(frozen=True)
class XbrlTaxonomyPackageResult:
    filing_count: int
    reference_count: int
    matched_reference_count: int
    resolved_reference_count: int
    unmatched_reference_count: int
    requested_package_count: int
    package_count: int
    failed_package_count: int
    oasis_package_count: int
    normalized_package_count: int
    package_revision_count: int
    edge_count: int
    network_request_count: int
    cache_hit_count: int
    package_paths: tuple[str, ...]
    failures: tuple[XbrlTaxonomyPackageFailure, ...]
    run_id: str


@dataclass(frozen=True)
class _FilingPackageDocument:
    security_id: str
    cik: str
    accession_number: str
    source_url: str
    cache_path: Path
    sha256: str


@dataclass(frozen=True)
class _TaxonomyReference:
    document: _FilingPackageDocument
    reference_namespace: str | None
    reference_url: str


@dataclass(frozen=True)
class _PackageRequest:
    package_key: str
    authority: str
    family: str
    version: str
    source_url: str
    source_kind: str = "archive"


@dataclass(frozen=True)
class _PackageFetch:
    content: bytes
    cache_path: Path
    sha256: str
    fetched_at: dt.datetime
    network_request_count: int
    cache_hit: bool


@dataclass(frozen=True)
class _ProcessorPackage:
    package_format: str
    path: Path
    sha256: str
    byte_count: int


@dataclass(frozen=True)
class _PackageCaptureFailure:
    failure: XbrlTaxonomyPackageFailure
    started_at: dt.datetime
    completed_at: dt.datetime


def _default_source_cache_dir(store: DuckDBStore, options: XbrlTaxonomyPackageOptions) -> Path:
    if options.source_cache_dir is not None:
        return options.source_cache_dir.expanduser().resolve()
    if str(store.path) == ":memory:":
        raise ValueError("source_cache_dir is required for an in-memory warehouse")
    database_path = store.path.expanduser().resolve()
    return database_path.parent / f"{database_path.name}.source-cache"


def _default_package_dir(store: DuckDBStore, options: XbrlTaxonomyPackageOptions) -> Path:
    if options.package_dir is not None:
        return options.package_dir.expanduser().resolve()
    if str(store.path) == ":memory:":
        raise ValueError("package_dir is required for an in-memory warehouse")
    database_path = store.path.expanduser().resolve()
    return database_path.parent / f"{database_path.name}.taxonomy-packages"


def _target_filing_package_documents(
    store: DuckDBStore,
    options: XbrlTaxonomyPackageOptions,
) -> list[_FilingPackageDocument]:
    store.con.register(
        "xbrl_taxonomy_accession_filter",
        pd.DataFrame(
            {"accession_number": list(options.accession_numbers or ())},
            dtype="string",
        ),
    )
    try:
        rows = store.con.execute(
            """
            WITH candidates AS (
                SELECT
                    json_extract_string(metadata_json,'$.security_id') AS security_id,
                    json_extract_string(metadata_json,'$.cik') AS cik,
                    json_extract_string(metadata_json,'$.accession_number') AS accession_number,
                    source_url,cache_path,sha256,fetched_at,
                    dense_rank() OVER (
                        ORDER BY json_extract_string(
                            metadata_json,'$.accession_number'
                        )
                    ) AS filing_rank
                FROM raw_source_files
                WHERE dataset_id='xbrl_filing_contexts'
                  AND json_extract_string(
                        metadata_json,'$.artifact_type'
                      ) IN (
                          'filing_package_extension_schema',
                          'filing_package_calculation_linkbase',
                          'filing_package_definition_linkbase',
                          'filing_package_label_linkbase',
                          'filing_package_presentation_linkbase'
                      )
                  AND cache_path IS NOT NULL AND sha256 IS NOT NULL
                  AND (
                      ? OR json_extract_string(
                          metadata_json,'$.accession_number'
                      ) IN (
                          SELECT accession_number FROM xbrl_taxonomy_accession_filter
                      )
                  )
                QUALIFY row_number() OVER (
                    PARTITION BY source_url ORDER BY fetched_at DESC
                )=1
            )
            SELECT security_id,cik,accession_number,source_url,cache_path,sha256
            FROM candidates
            WHERE filing_rank<=?
            ORDER BY accession_number,source_url
            """,
            [not bool(options.accession_numbers), options.max_filings],
        ).fetchall()
    finally:
        store.con.unregister("xbrl_taxonomy_accession_filter")
    return [
        _FilingPackageDocument(
            security_id=str(row[0]),
            cik=str(row[1]),
            accession_number=str(row[2]),
            source_url=str(row[3]),
            cache_path=Path(str(row[4])),
            sha256=str(row[5]),
        )
        for row in rows
    ]


def parse_filing_package_taxonomy_references(
    document: _FilingPackageDocument,
) -> list[_TaxonomyReference]:
    content = read_cached_source_payload(document.cache_path, document.sha256)
    if content is None:
        raise ValueError(
            "Filing-package document cache failed SHA-256 verification: "
            f"{document.cache_path}"
        )
    parser = etree.XMLParser(resolve_entities=False, no_network=True, recover=False)
    root = etree.fromstring(content, parser=parser)
    references: list[_TaxonomyReference] = []
    seen: set[tuple[str | None, str]] = set()
    for element in root.xpath('//*[local-name()="import"]'):
        reference_url = str(element.get("schemaLocation") or "").strip()
        if not reference_url:
            continue
        namespace = (
            None
            if element.get("namespace") is None
            else str(element.get("namespace"))
        )
        reference_url = urldefrag(reference_url).url
        if (namespace, reference_url) in seen:
            continue
        references.append(
            _TaxonomyReference(
                document=document,
                reference_namespace=namespace,
                reference_url=reference_url,
            )
        )
        seen.add((namespace, reference_url))
    for value in root.xpath('//@*[local-name()="href"]'):
        reference_url = urldefrag(str(value).strip()).url
        parsed = urlsplit(reference_url)
        if parsed.scheme.lower() not in {"http", "https"} or not parsed.hostname:
            continue
        if (None, reference_url) in seen:
            continue
        references.append(
            _TaxonomyReference(
                document=document,
                reference_namespace=None,
                reference_url=reference_url,
            )
        )
        seen.add((None, reference_url))
    return references


def taxonomy_package_request(reference_url: str) -> _PackageRequest | None:
    xbrl_us = _XBRL_US_GAAP_REFERENCE.match(reference_url)
    if xbrl_us:
        version = xbrl_us.group("version")
        if version != "2009":
            return None
        return _PackageRequest(
            package_key=f"XBRL_US:us-gaap:{version}",
            authority="XBRL_US",
            family="us-gaap",
            version=version,
            source_url=(
                "https://taxonomies.xbrl.us/us-gaap/2009/doc/"
                "XBRLUS-USGAAP-Taxonomies-2009-01-31.zip"
            ),
        )
    fasb = _FASB_IMPORT.match(reference_url)
    if fasb:
        family = fasb.group("family").lower()
        version = fasb.group("version")
        try:
            year = int(version[:4])
        except ValueError:
            return None
        filename = (
            f"{family}-{version}-01-31.zip"
            if year <= 2021
            else f"{family}-{version}.zip"
        )
        return _PackageRequest(
            package_key=f"FASB:{family}:{version}",
            authority="FASB",
            family=family,
            version=version,
            source_url=f"https://xbrl.fasb.org/{family}/{version}/{filename}",
        )
    sec = _SEC_IMPORT.match(reference_url)
    if sec:
        family = sec.group("family").lower()
        version = sec.group("version")
        try:
            year = int(version[:4])
        except ValueError:
            return None
        if (family, version) in _SEC_DIRECTORY_ONLY_PACKAGES:
            return _PackageRequest(
                package_key=f"SEC:{family}:{version}",
                authority="SEC",
                family=family,
                version=version,
                source_url=f"https://xbrl.sec.gov/{family}/{version}/",
                source_kind="sec_directory",
            )
        if year <= 2020:
            schema_name = reference_url.rsplit("/", 1)[-1]
            if not schema_name.lower().endswith(".xsd"):
                return None
            archive_name = (
                f"{family}-{version}.zip"
                if year >= 2019 or (family, version) in _SEC_YEAR_ONLY_ARCHIVES
                else f"{schema_name[:-4]}.zip"
            )
            return _PackageRequest(
                package_key=f"SEC:{family}:{version}",
                authority="SEC",
                family=family,
                version=version,
                source_url=(
                    f"https://xbrl.sec.gov/{family}/{version}/{archive_name}"
                ),
            )
        return _PackageRequest(
            package_key=f"SEC:standard:{version}",
            authority="SEC",
            family="sec-standard",
            version=version,
            source_url=f"https://xbrl.sec.gov/{version}.zip",
        )
    return None


def _validate_taxonomy_zip(content: bytes, source_url: str) -> None:
    try:
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            if not archive.namelist() or archive.testzip() is not None:
                raise ValueError(f"Taxonomy package contains corrupt members: {source_url}")
    except zipfile.BadZipFile as exc:
        raise ValueError(f"Taxonomy package is not a valid ZIP: {source_url}") from exc


def _taxonomy_package_metadata(content: bytes) -> tuple[bool, bool]:
    with zipfile.ZipFile(io.BytesIO(content)) as archive:
        members = {
            PurePosixPath(name).as_posix()
            for name in archive.namelist()
            if not name.endswith("/")
        }
    has_metadata = any(
        name.endswith("/META-INF/taxonomyPackage.xml") for name in members
    )
    has_catalog = any(name.endswith("/META-INF/catalog.xml") for name in members)
    return has_metadata, has_catalog


def _safe_archive_parts(member_name: str) -> tuple[str, ...]:
    member = PurePosixPath(member_name.replace("\\", "/"))
    if (
        member.is_absolute()
        or not member.parts
        or any(part in {"", ".", ".."} for part in member.parts)
    ):
        raise ValueError(f"Unsafe taxonomy archive member path: {member_name}")
    return tuple(member.parts)


def _normalized_taxonomy_package_bytes(
    request: _PackageRequest,
    fetched: _PackageFetch,
) -> bytes:
    with zipfile.ZipFile(io.BytesIO(fetched.content)) as source:
        source_members = [
            (name, source.read(name))
            for name in source.namelist()
            if not name.endswith("/")
        ]
    parsed_members = [
        (name, _safe_archive_parts(name), content)
        for name, content in source_members
        if "META-INF" not in _safe_archive_parts(name)
    ]
    top_levels = {parts[0] for _name, parts, _content in parsed_members}
    strip_common_root = len(top_levels) == 1 and all(
        len(parts) > 1 for _name, parts, _content in parsed_members
    )
    host = {
        "FASB": "xbrl.fasb.org",
        "SEC": "xbrl.sec.gov",
        "XBRL_US": "taxonomies.xbrl.us",
    }[request.authority]
    normalized_members: dict[str, bytes] = {}
    for original_name, parts, content in parsed_members:
        lower_parts = tuple(part.lower() for part in parts)
        if host in lower_parts:
            host_index = lower_parts.index(host)
            target_parts = (host, *parts[host_index + 1 :])
        elif lower_parts[:2] == (
            request.family.lower(),
            request.version.lower(),
        ):
            target_parts = (host, *parts)
        else:
            relative_parts = parts[1:] if strip_common_root else parts
            if tuple(part.lower() for part in relative_parts[:2]) == (
                request.family.lower(),
                request.version.lower(),
            ):
                target_parts = (host, *relative_parts)
            elif (
                lower_parts[:1] == (request.family.lower(),)
                and tuple(part.lower() for part in relative_parts[:1])
                == (request.version.lower(),)
            ):
                target_parts = (host, request.family, *relative_parts)
            else:
                target_parts = (
                    host,
                    request.family,
                    request.version,
                    *relative_parts,
                )
        target_name = PurePosixPath(*target_parts).as_posix()
        if target_name in normalized_members:
            raise ValueError(
                f"Duplicate normalized taxonomy member {target_name}: {original_name}"
            )
        normalized_members[target_name] = content

    package_root = (
        f"atx-{request.authority.lower()}-{request.family}-{request.version}-"
        f"{fetched.sha256[:12]}"
    )
    tp_namespace = "http://xbrl.org/2016/taxonomy-package"
    metadata = etree.Element(
        f"{{{tp_namespace}}}taxonomyPackage",
        nsmap={"tp": tp_namespace},
        attrib={"{http://www.w3.org/XML/1998/namespace}lang": "en-US"},
    )
    for name, value in (
        ("identifier", f"urn:atx:taxonomy-package:{fetched.sha256}"),
        ("name", f"ATX normalized {request.package_key}"),
        (
            "description",
            "Deterministic OASIS Taxonomy Package wrapper around an official "
            f"{request.authority} archive.",
        ),
        ("version", request.version),
        ("publisher", request.authority),
    ):
        etree.SubElement(metadata, f"{{{tp_namespace}}}{name}").text = value
    catalog_namespace = "urn:oasis:names:tc:entity:xmlns:xml:catalog"
    catalog = etree.Element(
        f"{{{catalog_namespace}}}catalog",
        nsmap={None: catalog_namespace},
    )
    url_prefix = (
        f"{host}/"
        if request.authority == "SEC" and request.family == "sec-standard"
        else f"{host}/{request.family}/{request.version}/"
    )
    local_prefix = f"../{url_prefix}"
    for scheme in ("http", "https"):
        etree.SubElement(
            catalog,
            f"{{{catalog_namespace}}}rewriteURI",
            uriStartString=f"{scheme}://{url_prefix}",
            rewritePrefix=local_prefix,
        )
    normalized_members[
        f"{package_root}/META-INF/taxonomyPackage.xml"
    ] = etree.tostring(metadata, encoding="UTF-8", xml_declaration=True)
    normalized_members[f"{package_root}/META-INF/catalog.xml"] = etree.tostring(
        catalog,
        encoding="UTF-8",
        xml_declaration=True,
    )

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for relative_name, content in sorted(normalized_members.items()):
            archive_name = f"{package_root}/{relative_name}"
            if relative_name.startswith(f"{package_root}/"):
                archive_name = relative_name
            info = zipfile.ZipInfo(archive_name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100444 << 16
            archive.writestr(info, content)
    return buffer.getvalue()


def _processor_package(
    *,
    request: _PackageRequest,
    fetched: _PackageFetch,
    materialized_path: Path,
    package_dir: Path,
) -> _ProcessorPackage:
    has_metadata, has_catalog = _taxonomy_package_metadata(fetched.content)
    if has_metadata and has_catalog:
        return _ProcessorPackage(
            package_format="oasis_taxonomy_package",
            path=materialized_path,
            sha256=fetched.sha256,
            byte_count=len(fetched.content),
        )
    normalized = _normalized_taxonomy_package_bytes(request, fetched)
    normalized_sha256 = hashlib.sha256(normalized).hexdigest()
    target = (
        package_dir
        / "processor-packages"
        / normalized_sha256[:2]
        / f"{normalized_sha256}.zip"
    )
    if read_cached_source_payload(target, normalized_sha256) is None:
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(f".{target.name}.{uuid.uuid4().hex}.tmp")
        try:
            temporary.write_bytes(normalized)
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
    return _ProcessorPackage(
        package_format="atx_normalized_taxonomy_package",
        path=target,
        sha256=normalized_sha256,
        byte_count=len(normalized),
    )


def _read_cached_package(store: DuckDBStore, source_url: str) -> _PackageFetch | None:
    rows = store.con.execute(
        """
        SELECT cache_path,sha256,fetched_at
        FROM raw_source_files
        WHERE dataset_id='xbrl_taxonomy_packages' AND source_url=?
          AND cache_path IS NOT NULL AND sha256 IS NOT NULL
        ORDER BY fetched_at DESC
        """,
        [source_url],
    ).fetchall()
    for cache_path_text, sha256, fetched_at in rows:
        cache_path = Path(str(cache_path_text))
        content = read_cached_source_payload(cache_path, str(sha256))
        if content is None:
            continue
        _validate_taxonomy_zip(content, source_url)
        return _PackageFetch(
            content=content,
            cache_path=cache_path,
            sha256=str(sha256),
            fetched_at=fetched_at,
            network_request_count=0,
            cache_hit=True,
        )
    return None


def _fetch_package(
    store: DuckDBStore,
    *,
    request: _PackageRequest,
    session: Any,
    source_cache_dir: Path,
    request_timeout: int,
) -> _PackageFetch:
    cached = _read_cached_package(store, request.source_url)
    if cached is not None:
        return cached
    if request.source_kind == "sec_directory":
        content, network_request_count = _fetch_sec_directory_package(
            request=request,
            session=session,
            request_timeout=request_timeout,
        )
    else:
        response = session.get(request.source_url, timeout=request_timeout)
        response.raise_for_status()
        content = bytes(response.content)
        network_request_count = 1
    _validate_taxonomy_zip(content, request.source_url)
    cached_payload = cache_source_payload(source_cache_dir, content)
    return _PackageFetch(
        content=content,
        cache_path=cached_payload.cache_path,
        sha256=cached_payload.sha256,
        fetched_at=now_utc_naive(),
        network_request_count=network_request_count,
        cache_hit=False,
    )


def _fetch_sec_directory_package(
    *,
    request: _PackageRequest,
    session: Any,
    request_timeout: int,
) -> tuple[bytes, int]:
    """Assemble an immutable ZIP when an official SEC family has no archive."""

    directory_response = session.get(request.source_url, timeout=request_timeout)
    directory_response.raise_for_status()
    root = etree.HTML(bytes(directory_response.content))
    if root is None:
        raise ValueError(f"SEC taxonomy directory is not valid HTML: {request.source_url}")
    member_urls: list[tuple[str, str]] = []
    directory = urlsplit(request.source_url)
    for href_value in root.xpath("//a/@href"):
        href = str(href_value).strip()
        member_url = urljoin(request.source_url, href)
        parsed = urlsplit(member_url)
        relative_path = (
            parsed.path[len(directory.path) :]
            if parsed.path.startswith(directory.path)
            else ""
        )
        member_name = PurePosixPath(relative_path).name
        if (
            parsed.scheme != directory.scheme
            or parsed.netloc != directory.netloc
            or parsed.query
            or parsed.fragment
            or not member_name
            or member_name != relative_path
            or PurePosixPath(member_name).suffix.lower()
            not in _TAXONOMY_DOCUMENT_SUFFIXES
        ):
            continue
        member_urls.append((member_name, member_url))
    member_urls = sorted(set(member_urls))
    if not member_urls:
        raise ValueError(
            f"SEC taxonomy directory contains no XSD/XML members: {request.source_url}"
        )

    members: list[tuple[str, bytes]] = []
    for member_name, member_url in member_urls:
        response = session.get(member_url, timeout=request_timeout)
        response.raise_for_status()
        content = bytes(response.content)
        if not content:
            raise ValueError(f"SEC taxonomy member is empty: {member_url}")
        members.append((member_name, content))

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for member_name, content in members:
            archive_name = PurePosixPath(
                request.family,
                request.version,
                member_name,
            ).as_posix()
            info = zipfile.ZipInfo(archive_name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100444 << 16
            archive.writestr(info, content)
    return buffer.getvalue(), 1 + len(members)


def _insert_package_capture_attempt(
    store: DuckDBStore,
    *,
    run_id: str,
    request: _PackageRequest,
    started_at: dt.datetime,
    completed_at: dt.datetime,
    fetched: _PackageFetch | None,
    processor_package: _ProcessorPackage | None,
    package_revision_id: str | None,
    failure: XbrlTaxonomyPackageFailure | None,
    source_loaded_at: dt.datetime,
) -> None:
    attempt_id = hashlib.sha256(
        f"{run_id}|{request.package_key}".encode()
    ).hexdigest()
    store.con.execute(
        """
        UPDATE xbrl_taxonomy_package_capture_attempts
        SET is_latest_revision=false
        WHERE package_key=? AND attempt_id<>? AND is_latest_revision
        """,
        [request.package_key, attempt_id],
    )
    store.con.execute(
        """
        INSERT OR REPLACE INTO xbrl_taxonomy_package_capture_attempts (
            attempt_id,run_id,package_key,authority,taxonomy_family,
            taxonomy_version,source_url,source_kind,status,failure_stage,
            cache_hit,network_request_count,sha256,byte_count,
            package_revision_id,package_format,processor_package_path,
            processor_package_sha256,processor_package_byte_count,
            error_type,error_message,started_at,completed_at,available_at,
            as_of_date,is_latest_revision,source_loaded_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,true,?)
        """,
        [
            attempt_id,
            run_id,
            request.package_key,
            request.authority,
            request.family,
            request.version,
            request.source_url,
            request.source_kind,
            "failed" if failure is not None else "succeeded",
            None if failure is None else failure.failure_stage,
            None if fetched is None else fetched.cache_hit,
            None if fetched is None else fetched.network_request_count,
            None if fetched is None else fetched.sha256,
            None if fetched is None else len(fetched.content),
            package_revision_id,
            None if processor_package is None else processor_package.package_format,
            None if processor_package is None else str(processor_package.path),
            None if processor_package is None else processor_package.sha256,
            None if processor_package is None else processor_package.byte_count,
            None if failure is None else failure.error_type,
            None if failure is None else failure.error_message,
            started_at,
            completed_at,
            completed_at,
            completed_at.date(),
            source_loaded_at,
        ],
    )


def capture_xbrl_taxonomy_packages(
    store: DuckDBStore,
    options: XbrlTaxonomyPackageOptions | None = None,
) -> XbrlTaxonomyPackageResult:
    options = options or XbrlTaxonomyPackageOptions()
    if options.max_filings < 1 or options.request_timeout < 1:
        raise ValueError("max_filings and request_timeout must be positive")
    store.initialize()
    run_id = options.run_id or f"xbrl-taxonomy-packages-{uuid.uuid4()}"
    documents = _target_filing_package_documents(store, options)
    if not documents:
        raise RuntimeError("No cached filing-package documents matched the package options")
    references = [
        item
        for document in documents
        for item in parse_filing_package_taxonomy_references(document)
    ]
    request_by_key: dict[str, _PackageRequest] = {}
    request_for_reference: dict[tuple[str, str], _PackageRequest] = {}
    unmatched = 0
    for item in references:
        request = taxonomy_package_request(item.reference_url)
        if request is None:
            unmatched += 1
            continue
        request_by_key[request.package_key] = request
        request_for_reference[(item.document.source_url, item.reference_url)] = request

    source_cache_dir = _default_source_cache_dir(store, options)
    package_dir = _default_package_dir(store, options)
    session = sec_session(options.user_agent)
    fetch_by_key: dict[str, _PackageFetch] = {}
    materialized_by_key: dict[str, Path] = {}
    processor_by_key: dict[str, _ProcessorPackage] = {}
    started_by_key: dict[str, dt.datetime] = {}
    completed_by_key: dict[str, dt.datetime] = {}
    failure_by_key: dict[str, _PackageCaptureFailure] = {}
    network_requests = cache_hits = 0
    for package_key, request in sorted(request_by_key.items()):
        started_at = now_utc_naive()
        started_by_key[package_key] = started_at
        failure_stage = "fetch"
        try:
            fetched = _fetch_package(
                store,
                request=request,
                session=session,
                source_cache_dir=source_cache_dir,
                request_timeout=options.request_timeout,
            )
            failure_stage = "materialize"
            materialized = materialize_cached_source_url(
                package_dir,
                source_url=request.source_url,
                cache_path=fetched.cache_path,
                expected_sha256=fetched.sha256,
            )
            record_source_file(
                store,
                dataset_id="xbrl_taxonomy_packages",
                source_url=request.source_url,
                cache_path=fetched.cache_path,
                status="cached",
                metadata={
                    "package_key": package_key,
                    "authority": request.authority,
                    "taxonomy_family": request.family,
                    "taxonomy_version": request.version,
                    "source_kind": request.source_kind,
                    "materialized_path": str(materialized.materialized_path),
                    "cache_hit": fetched.cache_hit,
                },
                sha256=fetched.sha256,
            )
            failure_stage = "normalize"
            processor_package = _processor_package(
                request=request,
                fetched=fetched,
                materialized_path=materialized.materialized_path,
                package_dir=package_dir,
            )
        except (RequestException, OSError, ValueError) as exc:
            if options.fail_fast:
                raise
            completed_at = now_utc_naive()
            failure = XbrlTaxonomyPackageFailure(
                package_key=package_key,
                source_url=request.source_url,
                failure_stage=failure_stage,
                error_type=type(exc).__name__,
                error_message=str(exc)[:4000],
            )
            failure_by_key[package_key] = _PackageCaptureFailure(
                failure=failure,
                started_at=started_at,
                completed_at=completed_at,
            )
            completed_by_key[package_key] = completed_at
            continue
        fetch_by_key[package_key] = fetched
        materialized_by_key[package_key] = materialized.materialized_path
        processor_by_key[package_key] = processor_package
        completed_by_key[package_key] = now_utc_naive()
        network_requests += fetched.network_request_count
        cache_hits += int(fetched.cache_hit)

    captured_at = now_utc_naive()
    package_revision_by_key: dict[str, str] = {}
    package_revision_count = 0
    edge_count = 0
    with store.transaction():
        for package_key in sorted(fetch_by_key):
            request = request_by_key[package_key]
            fetched = fetch_by_key[package_key]
            processor_package = processor_by_key[package_key]
            revision_id = hashlib.sha256(
                f"{package_key}|{fetched.sha256}".encode()
            ).hexdigest()
            package_revision_by_key[package_key] = revision_id
            store.con.execute(
                """
                UPDATE xbrl_standard_taxonomy_package_revisions
                SET is_latest_revision=false
                WHERE package_key=? AND package_revision_id<>? AND is_latest_revision
                """,
                [package_key, revision_id],
            )
            existing_row = store.con.execute(
                "SELECT count(*) FROM xbrl_standard_taxonomy_package_revisions WHERE package_revision_id=?",
                [revision_id],
            ).fetchone()
            if existing_row is None:
                raise RuntimeError("Taxonomy package revision count returned no row")
            existing = int(existing_row[0])
            if not existing:
                store.con.execute(
                    """
                    INSERT INTO xbrl_standard_taxonomy_package_revisions (
                        package_revision_id,package_key,authority,taxonomy_family,
                        taxonomy_version,source_url,sha256,byte_count,cache_path,
                        materialized_path,status,fetched_at,as_of_date,available_at,
                        is_latest_revision,run_id,source_loaded_at,package_format,
                        processor_package_path,processor_package_sha256,
                        processor_package_byte_count
                    ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,true,?,?,?,?,?,?)
                    """,
                    [
                        revision_id,
                        package_key,
                        request.authority,
                        request.family,
                        request.version,
                        request.source_url,
                        fetched.sha256,
                        len(fetched.content),
                        str(fetched.cache_path),
                        str(materialized_by_key[package_key]),
                        "cached",
                        fetched.fetched_at,
                        fetched.fetched_at.date(),
                        fetched.fetched_at,
                        run_id,
                        captured_at,
                        processor_package.package_format,
                        str(processor_package.path),
                        processor_package.sha256,
                        processor_package.byte_count,
                    ],
                )
                package_revision_count += 1
            else:
                store.con.execute(
                    """
                    UPDATE xbrl_standard_taxonomy_package_revisions
                    SET is_latest_revision=true,materialized_path=?,source_loaded_at=?,
                        package_format=?,processor_package_path=?,
                        processor_package_sha256=?,processor_package_byte_count=?
                    WHERE package_revision_id=?
                    """,
                    [
                        str(materialized_by_key[package_key]),
                        captured_at,
                        processor_package.package_format,
                        str(processor_package.path),
                        processor_package.sha256,
                        processor_package.byte_count,
                        revision_id,
                    ],
                )

        for item in references:
            request = request_for_reference.get(
                (item.document.source_url, item.reference_url)
            )
            if request is None or request.package_key not in package_revision_by_key:
                continue
            revision_id = package_revision_by_key[request.package_key]
            edge_id = hashlib.sha256(
                "|".join(
                    [
                        item.document.security_id,
                        item.document.accession_number,
                        item.document.source_url,
                        item.reference_url,
                        revision_id,
                    ]
                ).encode()
            ).hexdigest()
            store.con.execute(
                """
                UPDATE xbrl_filing_taxonomy_packages
                SET is_latest_revision=false
                WHERE security_id=? AND accession_number=?
                  AND source_document_url=? AND reference_url=?
                  AND filing_package_edge_id<>? AND is_latest_revision
                """,
                [
                    item.document.security_id,
                    item.document.accession_number,
                    item.document.source_url,
                    item.reference_url,
                    edge_id,
                ],
            )
            existing_row = store.con.execute(
                """
                SELECT count(*) FROM xbrl_filing_taxonomy_packages
                WHERE filing_package_edge_id=?
                """,
                [edge_id],
            ).fetchone()
            if existing_row is None:
                raise RuntimeError("Filing taxonomy edge count returned no row")
            existing = int(existing_row[0])
            if existing:
                store.con.execute(
                    """
                    UPDATE xbrl_filing_taxonomy_packages
                    SET is_latest_revision=true,source_loaded_at=?
                    WHERE filing_package_edge_id=?
                    """,
                    [captured_at, edge_id],
                )
                continue
            store.con.execute(
                """
                INSERT INTO xbrl_filing_taxonomy_packages (
                    filing_package_edge_id,security_id,cik,accession_number,
                    source_document_url,reference_namespace,reference_url,
                    package_revision_id,package_key,as_of_date,available_at,
                    is_latest_revision,run_id,source_loaded_at
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,true,?,?)
                """,
                [
                    edge_id,
                    item.document.security_id,
                    item.document.cik,
                    item.document.accession_number,
                    item.document.source_url,
                    item.reference_namespace,
                    item.reference_url,
                    revision_id,
                    request.package_key,
                    captured_at.date(),
                    captured_at,
                    run_id,
                    captured_at,
                ],
            )
            edge_count += 1

        for package_key, request in sorted(request_by_key.items()):
            failure_state = failure_by_key.get(package_key)
            _insert_package_capture_attempt(
                store,
                run_id=run_id,
                request=request,
                started_at=started_by_key[package_key],
                completed_at=completed_by_key[package_key],
                fetched=fetch_by_key.get(package_key),
                processor_package=processor_by_key.get(package_key),
                package_revision_id=package_revision_by_key.get(package_key),
                failure=None if failure_state is None else failure_state.failure,
                source_loaded_at=captured_at,
            )

    return XbrlTaxonomyPackageResult(
        filing_count=len({document.accession_number for document in documents}),
        reference_count=len(references),
        matched_reference_count=len(request_for_reference),
        resolved_reference_count=sum(
            request.package_key in fetch_by_key
            for request in request_for_reference.values()
        ),
        unmatched_reference_count=unmatched,
        requested_package_count=len(request_by_key),
        package_count=len(fetch_by_key),
        failed_package_count=len(failure_by_key),
        oasis_package_count=sum(
            package.package_format == "oasis_taxonomy_package"
            for package in processor_by_key.values()
        ),
        normalized_package_count=sum(
            package.package_format == "atx_normalized_taxonomy_package"
            for package in processor_by_key.values()
        ),
        package_revision_count=package_revision_count,
        edge_count=edge_count,
        network_request_count=network_requests,
        cache_hit_count=cache_hits,
        package_paths=tuple(
            str(processor_by_key[key].path) for key in sorted(processor_by_key)
        ),
        failures=tuple(
            failure_by_key[key].failure for key in sorted(failure_by_key)
        ),
        run_id=run_id,
    )


class XbrlTaxonomyPackageDataset(Dataset):
    dataset_id = "xbrl_standard_taxonomy_packages"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: XbrlTaxonomyPackageOptions,
    ) -> DatasetLoadResult:
        result = capture_xbrl_taxonomy_packages(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="xbrl_standard_taxonomy_package_revisions",
            check_name="taxonomy_packages_captured",
            status="passed" if result.package_count else "warning",
            observed_value=float(result.package_count),
            threshold_value=1.0,
            details={
                "filings": result.filing_count,
                "references": result.reference_count,
                "matched_references": result.matched_reference_count,
                "resolved_references": result.resolved_reference_count,
                "unmatched_references": result.unmatched_reference_count,
                "requested_packages": result.requested_package_count,
                "failed_packages": result.failed_package_count,
                "network_requests": result.network_request_count,
                "cache_hits": result.cache_hit_count,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=result.package_revision_count + result.edge_count,
            source=self.source_name,
            run_id=result.run_id,
            details={
                "filings": result.filing_count,
                "references": result.reference_count,
                "matched_references": result.matched_reference_count,
                "resolved_references": result.resolved_reference_count,
                "unmatched_references": result.unmatched_reference_count,
                "requested_packages": result.requested_package_count,
                "packages": result.package_count,
                "failed_packages": result.failed_package_count,
                "failures": result.failures,
                "oasis_packages": result.oasis_package_count,
                "normalized_packages": result.normalized_package_count,
                "package_revisions": result.package_revision_count,
                "edges": result.edge_count,
                "network_requests": result.network_request_count,
                "cache_hits": result.cache_hit_count,
                "package_paths": result.package_paths,
            },
        )
