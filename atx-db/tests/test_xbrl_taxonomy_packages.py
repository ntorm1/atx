from __future__ import annotations

import io
import zipfile
from pathlib import Path

import requests

from atx_db import xbrl_processor, xbrl_taxonomy_packages
from atx_db.quality import run_warehouse_quality_checks
from atx_db.source_cache import cache_source_payload
from atx_db.warehouse import record_source_file
from atx_db.xbrl_taxonomy_packages import (
    XbrlTaxonomyPackageOptions,
    capture_xbrl_taxonomy_packages,
    taxonomy_package_request,
)


def _zip_payload(name: str) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr(f"{name}/2022/{name}-2022.xsd", name)
    return buffer.getvalue()


def test_taxonomy_package_request_supports_legacy_and_current_names() -> None:
    legacy = taxonomy_package_request(
        "https://xbrl.fasb.org/us-gaap/2020/elts/us-gaap-2020.xsd"
    )
    current = taxonomy_package_request(
        "https://xbrl.fasb.org/srt/2026/elts/srt-2026.xsd"
    )
    sec = taxonomy_package_request(
        "https://xbrl.sec.gov/dei/2022/dei-2022.xsd"
    )
    legacy_sec = taxonomy_package_request(
        "http://xbrl.sec.gov/country/2011/country-2011-01-31.xsd"
    )
    sec_2020 = taxonomy_package_request(
        "https://xbrl.sec.gov/country/2020/country-2020-01-31.xsd"
    )
    directory_only_sec = taxonomy_package_request(
        "https://xbrl.sec.gov/stpr/2018/stpr-2018-01-31.xsd"
    )
    sec_dei_2018 = taxonomy_package_request(
        "https://xbrl.sec.gov/dei/2018/dei-2018-01-31.xsd"
    )
    sec_exch_2018 = taxonomy_package_request(
        "https://xbrl.sec.gov/exch/2018/exch-2018-01-31.xsd"
    )
    xbrl_us = taxonomy_package_request(
        "http://taxonomies.xbrl.us/us-gaap/2009/elts/"
        "us-gaap-2009-01-31.xsd"
    )

    assert legacy is not None and legacy.source_url.endswith(
        "/us-gaap-2020-01-31.zip"
    )
    assert current is not None and current.source_url.endswith("/srt-2026.zip")
    assert sec is not None and sec.source_url == "https://xbrl.sec.gov/2022.zip"
    assert legacy_sec is not None
    assert legacy_sec.package_key == "SEC:country:2011"
    assert legacy_sec.source_url == (
        "https://xbrl.sec.gov/country/2011/country-2011-01-31.zip"
    )
    assert sec_2020 is not None
    assert sec_2020.source_url == (
        "https://xbrl.sec.gov/country/2020/country-2020.zip"
    )
    assert directory_only_sec is not None
    assert directory_only_sec.source_kind == "sec_directory"
    assert directory_only_sec.source_url == "https://xbrl.sec.gov/stpr/2018/"
    assert sec_dei_2018 is not None
    assert sec_dei_2018.source_url == "https://xbrl.sec.gov/dei/2018/dei-2018.zip"
    assert sec_exch_2018 is not None
    assert sec_exch_2018.source_kind == "sec_directory"
    assert sec_exch_2018.source_url == "https://xbrl.sec.gov/exch/2018/"
    assert xbrl_us is not None
    assert xbrl_us.package_key == "XBRL_US:us-gaap:2009"
    assert xbrl_us.source_url.endswith(
        "/doc/XBRLUS-USGAAP-Taxonomies-2009-01-31.zip"
    )
    assert taxonomy_package_request("http://www.xbrl.org/2003/xbrl-instance.xsd") is None


def test_sec_directory_only_taxonomy_is_assembled_deterministically(
    tmp_path,
    tmp_store,
) -> None:
    request = taxonomy_package_request(
        "https://xbrl.sec.gov/stpr/2018/stpr-2018-01-31.xsd"
    )
    assert request is not None
    payloads = {
        request.source_url: b"""
            <html><body>
              <a href="../">Parent</a>
              <a href="stpr-2018-01-31.xsd">schema</a>
              <a href="stpr-lab-2018-01-31.xml">labels</a>
              <a href="index.htm">index</a>
            </body></html>
        """,
        f"{request.source_url}stpr-2018-01-31.xsd": b"<schema/>",
        f"{request.source_url}stpr-lab-2018-01-31.xml": b"<linkbase/>",
    }

    class FakeResponse:
        def __init__(self, content: bytes) -> None:
            self.content = content

        def raise_for_status(self) -> None:
            return None

    class FakeSession:
        def get(self, url: str, timeout: int):
            assert timeout == 30
            return FakeResponse(payloads[url])

    first = xbrl_taxonomy_packages._fetch_package(
        tmp_store,
        request=request,
        session=FakeSession(),
        source_cache_dir=tmp_path / "source-cache",
        request_timeout=30,
    )
    second_content, second_requests = (
        xbrl_taxonomy_packages._fetch_sec_directory_package(
            request=request,
            session=FakeSession(),
            request_timeout=30,
        )
    )

    assert first.network_request_count == 3
    assert first.content == second_content
    assert second_requests == 3
    with zipfile.ZipFile(io.BytesIO(first.content)) as archive:
        assert archive.namelist() == [
            "stpr/2018/stpr-2018-01-31.xsd",
            "stpr/2018/stpr-lab-2018-01-31.xml",
        ]
    normalized = xbrl_taxonomy_packages._normalized_taxonomy_package_bytes(
        request,
        first,
    )
    with zipfile.ZipFile(io.BytesIO(normalized)) as archive:
        names = archive.namelist()
        assert any(
            name.endswith(
                "/xbrl.sec.gov/stpr/2018/stpr-2018-01-31.xsd"
            )
            for name in names
        )
        assert not any("/stpr/2018/2018/" in name for name in names)


def test_filing_package_linkbase_references_are_discovered(tmp_path) -> None:
    content = b"""<link:linkbase
      xmlns:link="http://www.xbrl.org/2003/linkbase"
      xmlns:xlink="http://www.w3.org/1999/xlink">
      <link:loc xlink:type="locator"
        xlink:href="http://taxonomies.xbrl.us/us-gaap/2009/elts/us-roles-2009-01-31.xsd#role"/>
      <link:loc xlink:type="locator" xlink:href="local-extension.xsd#concept"/>
    </link:linkbase>"""
    cached = cache_source_payload(tmp_path / "objects", content)
    document = xbrl_taxonomy_packages._FilingPackageDocument(
        security_id="SEC-CIK-0000820027",
        cik="0000820027",
        accession_number="0001104659-10-056357",
        source_url="https://www.sec.gov/Archives/amp-20100930_pre.xml",
        cache_path=cached.cache_path,
        sha256=cached.sha256,
    )

    references = (
        xbrl_taxonomy_packages.parse_filing_package_taxonomy_references(document)
    )

    assert [reference.reference_url for reference in references] == [
        "http://taxonomies.xbrl.us/us-gaap/2009/elts/"
        "us-roles-2009-01-31.xsd"
    ]


def test_capture_taxonomy_packages_persists_revisions_edges_and_cache_replay(
    tmp_path,
    tmp_store,
    monkeypatch,
) -> None:
    accession = "0000004904-23-000081"
    schema_url = (
        "https://www.sec.gov/Archives/edgar/data/4904/"
        "000000490423000081/aep-20230526.xsd"
    )
    schema_content = b"""<?xml version="1.0"?>
    <xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
      <xs:import namespace="http://fasb.org/us-gaap/2022"
        schemaLocation="https://xbrl.fasb.org/us-gaap/2022/elts/us-gaap-2022.xsd"/>
      <xs:import namespace="http://fasb.org/srt/2022"
        schemaLocation="https://xbrl.fasb.org/srt/2022/elts/srt-2022.xsd"/>
      <xs:import namespace="http://xbrl.sec.gov/dei/2022"
        schemaLocation="https://xbrl.sec.gov/dei/2022/dei-2022.xsd"/>
      <xs:import namespace="http://xbrl.sec.gov/exch/2022"
        schemaLocation="https://xbrl.sec.gov/exch/2022/exch-2022.xsd"/>
      <xs:import namespace="http://www.xbrl.org/2003/instance"
        schemaLocation="http://www.xbrl.org/2003/xbrl-instance-2003-12-31.xsd"/>
    </xs:schema>"""
    cached_schema = cache_source_payload(tmp_path / "objects", schema_content)
    record_source_file(
        tmp_store,
        dataset_id="xbrl_filing_contexts",
        source_url=schema_url,
        cache_path=cached_schema.cache_path,
        sha256=cached_schema.sha256,
        status="cached",
        metadata={
            "security_id": "SEC-CIK-0000004904",
            "cik": "0000004904",
            "accession_number": accession,
            "artifact_type": "filing_package_extension_schema",
        },
    )

    payloads = {
        "https://xbrl.fasb.org/us-gaap/2022/us-gaap-2022.zip": _zip_payload(
            "us-gaap"
        ),
        "https://xbrl.fasb.org/srt/2022/srt-2022.zip": _zip_payload("srt"),
        "https://xbrl.sec.gov/2022.zip": _zip_payload("sec"),
    }

    class FakeResponse:
        def __init__(self, content: bytes) -> None:
            self.content = content

        def raise_for_status(self) -> None:
            return None

    class FakeSession:
        def get(self, url: str, timeout: int):
            assert timeout == 30
            return FakeResponse(payloads[url])

    monkeypatch.setattr(
        xbrl_taxonomy_packages,
        "sec_session",
        lambda _user_agent: FakeSession(),
    )
    options = XbrlTaxonomyPackageOptions(
        accession_numbers=(accession,),
        max_filings=1,
        source_cache_dir=tmp_path / "source-cache",
        package_dir=tmp_path / "packages",
        request_timeout=30,
        run_id="taxonomy-package-test",
    )
    first = capture_xbrl_taxonomy_packages(tmp_store, options)

    assert (
        first.filing_count,
        first.reference_count,
        first.matched_reference_count,
        first.unmatched_reference_count,
        first.package_count,
        first.package_revision_count,
        first.edge_count,
        first.network_request_count,
        first.cache_hit_count,
    ) == (1, 5, 4, 1, 3, 3, 4, 3, 0)
    assert (first.oasis_package_count, first.normalized_package_count) == (0, 3)
    assert first.requested_package_count == 3
    assert first.resolved_reference_count == 4
    assert first.failed_package_count == 0
    assert first.failures == ()
    assert all(Path(path).is_file() for path in first.package_paths)
    for package_path in first.package_paths:
        with zipfile.ZipFile(package_path) as package:
            names = package.namelist()
            assert any(name.endswith("/META-INF/taxonomyPackage.xml") for name in names)
            assert any(name.endswith("/META-INF/catalog.xml") for name in names)
    catalog_paths = xbrl_processor._catalog_taxonomy_package_paths(
        tmp_store,
        {
            "security_id": "SEC-CIK-0000004904",
            "accession_number": accession,
        },
    )
    assert tuple(str(path) for path in catalog_paths) == first.package_paths

    class OfflineSession:
        def get(self, url: str, timeout: int):
            raise AssertionError(f"unexpected network request: {url}, {timeout}")

    monkeypatch.setattr(
        xbrl_taxonomy_packages,
        "sec_session",
        lambda _user_agent: OfflineSession(),
    )
    second = capture_xbrl_taxonomy_packages(tmp_store, options)

    assert second.package_count == 3
    assert second.requested_package_count == 3
    assert second.resolved_reference_count == 4
    assert second.failed_package_count == 0
    assert (second.oasis_package_count, second.normalized_package_count) == (0, 3)
    assert second.package_revision_count == 0
    assert second.edge_count == 0
    assert second.network_request_count == 0
    assert second.cache_hit_count == 3
    assert tmp_store.con.execute(
        """
        SELECT
            count(*),
            count(*) FILTER (WHERE is_latest_revision),
            count(*) FILTER (
                WHERE package_format='atx_normalized_taxonomy_package'
            )
        FROM xbrl_standard_taxonomy_package_revisions
        """
    ).fetchone() == (3, 3, 3)
    assert tmp_store.con.execute(
        """
        SELECT count(*),count(*) FILTER (WHERE is_latest_revision)
        FROM xbrl_filing_taxonomy_packages
        """
    ).fetchone() == (4, 4)
    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=(
            "xbrl_standard_taxonomy_packages",
            "xbrl_filing_taxonomy_packages",
        ),
        record=False,
    )
    assert len(checks) == 5
    assert all(check.status == "passed" for check in checks)


def test_capture_taxonomy_packages_isolates_and_supersedes_package_failures(
    tmp_path,
    tmp_store,
    monkeypatch,
) -> None:
    accession = "0000000001-11-000001"
    schema_url = "https://www.sec.gov/Archives/test/failure-isolation.xsd"
    schema_content = b"""<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
      <xs:import namespace="http://fasb.org/us-gaap/2022"
        schemaLocation="https://xbrl.fasb.org/us-gaap/2022/elts/us-gaap-2022.xsd"/>
      <xs:import namespace="http://xbrl.sec.gov/currency/2011"
        schemaLocation="https://xbrl.sec.gov/currency/2011/currency-2011-01-31.xsd"/>
    </xs:schema>"""
    cached_schema = cache_source_payload(tmp_path / "objects", schema_content)
    record_source_file(
        tmp_store,
        dataset_id="xbrl_filing_contexts",
        source_url=schema_url,
        cache_path=cached_schema.cache_path,
        sha256=cached_schema.sha256,
        status="cached",
        metadata={
            "security_id": "SEC-CIK-0000000001",
            "cik": "0000000001",
            "accession_number": accession,
            "artifact_type": "filing_package_extension_schema",
        },
    )
    us_gaap_url = "https://xbrl.fasb.org/us-gaap/2022/us-gaap-2022.zip"
    currency_url = (
        "https://xbrl.sec.gov/currency/2011/currency-2011-01-31.zip"
    )

    class FakeResponse:
        def __init__(self, content: bytes) -> None:
            self.content = content

        def raise_for_status(self) -> None:
            return None

    class PartiallyUnavailableSession:
        def get(self, url: str, timeout: int):
            assert timeout == 30
            if url == currency_url:
                raise requests.ConnectionError("publisher unavailable")
            assert url == us_gaap_url
            return FakeResponse(_zip_payload("us-gaap"))

    monkeypatch.setattr(
        xbrl_taxonomy_packages,
        "sec_session",
        lambda _user_agent: PartiallyUnavailableSession(),
    )
    first = capture_xbrl_taxonomy_packages(
        tmp_store,
        XbrlTaxonomyPackageOptions(
            accession_numbers=(accession,),
            max_filings=1,
            source_cache_dir=tmp_path / "source-cache",
            package_dir=tmp_path / "packages",
            request_timeout=30,
            run_id="taxonomy-failure-isolation",
        ),
    )

    assert first.requested_package_count == 2
    assert first.package_count == 1
    assert first.failed_package_count == 1
    assert first.matched_reference_count == 2
    assert first.resolved_reference_count == 1
    assert first.edge_count == 1
    assert first.failures[0].package_key == "SEC:currency:2011"
    assert first.failures[0].error_type == "ConnectionError"
    assert tmp_store.con.execute(
        """
        SELECT package_key,status,failure_stage,error_type,is_latest_revision
        FROM xbrl_taxonomy_package_capture_attempts
        WHERE run_id='taxonomy-failure-isolation'
        ORDER BY package_key
        """
    ).fetchall() == [
        ("FASB:us-gaap:2022", "succeeded", None, None, True),
        ("SEC:currency:2011", "failed", "fetch", "ConnectionError", True),
    ]
    failed_checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("xbrl_taxonomy_package_capture_attempts",),
        record=False,
    )
    assert [check.status for check in failed_checks] == [
        "passed",
        "passed",
        "warning",
    ]

    class RecoveredSession:
        def get(self, url: str, timeout: int):
            assert timeout == 30
            assert url == currency_url
            return FakeResponse(_zip_payload("currency"))

    monkeypatch.setattr(
        xbrl_taxonomy_packages,
        "sec_session",
        lambda _user_agent: RecoveredSession(),
    )
    recovered = capture_xbrl_taxonomy_packages(
        tmp_store,
        XbrlTaxonomyPackageOptions(
            accession_numbers=(accession,),
            max_filings=1,
            source_cache_dir=tmp_path / "source-cache",
            package_dir=tmp_path / "packages",
            request_timeout=30,
            run_id="taxonomy-failure-recovered",
        ),
    )

    assert recovered.package_count == 2
    assert recovered.failed_package_count == 0
    assert recovered.resolved_reference_count == 2
    assert recovered.edge_count == 1
    assert tmp_store.con.execute(
        """
        SELECT status,count(*)
        FROM xbrl_taxonomy_package_capture_attempts
        WHERE is_latest_revision
        GROUP BY status
        """
    ).fetchall() == [("succeeded", 2)]
    recovered_checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("xbrl_taxonomy_package_capture_attempts",),
        record=False,
    )
    assert all(check.status == "passed" for check in recovered_checks)
