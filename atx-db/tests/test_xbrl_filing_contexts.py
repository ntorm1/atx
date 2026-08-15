from __future__ import annotations

import datetime as dt
import json
from dataclasses import replace
from pathlib import Path

import pytest

from atx_db import xbrl_filing_contexts
from atx_db.connection import DuckDBStore
from atx_db.quality import run_warehouse_quality_checks
from atx_db.xbrl_filing_contexts import (
    XbrlFilingContextDataset,
    XbrlFilingContextOptions,
    discover_legacy_instance_document,
    filing_package_document_candidates,
    inline_resource_document_candidates,
    parse_xbrl_filing,
)


def test_inline_resource_candidates_prefer_primary_base_then_size() -> None:
    payload = {
        "directory": {
            "item": [
                {"name": "aep-20230526_d2.htm", "size": "90000"},
                {"name": "aep-20230526.htm", "size": "24000000"},
                {"name": "other-inline.htm", "size": "30000000"},
                {"name": "R1.htm", "size": "50000000"},
                {"name": "0000004904-23-000081-index.html", "size": "1000"},
            ]
        }
    }

    assert inline_resource_document_candidates(
        payload,
        primary_document="aep-20230526_d2.htm",
    ) == ("aep-20230526.htm", "other-inline.htm")


def test_filing_package_candidates_include_entrypoints_schema_and_linkbases() -> None:
    payload = {
        "directory": {
            "item": [
                {"name": "issuer.xsd"},
                {"name": "issuer_cal.xml"},
                {"name": "issuer_def.xml"},
                {"name": "issuer_lab.xml"},
                {"name": "issuer_pre.xml"},
                {"name": "issuer_htm.xml"},
                {"name": "FilingSummary.xml"},
                {"name": "R1.htm"},
            ]
        }
    }

    assert filing_package_document_candidates(
        payload,
        entrypoint_documents=("cover.htm", "report.htm"),
    ) == (
        "cover.htm",
        "issuer.xsd",
        "issuer_cal.xml",
        "issuer_def.xml",
        "issuer_lab.xml",
        "issuer_pre.xml",
        "report.htm",
    )


def test_inline_loader_falls_back_to_resource_bearing_companion(
    tmp_path, tmp_store, monkeypatch
) -> None:
    accession = "0000004904-23-000081"
    primary_document = "aep-20230526_d2.htm"
    companion_document = "aep-20230526.htm"
    tmp_store.con.execute(
        """
        INSERT INTO sec_submissions (
            security_id,cik,accession_number,filing_date,report_date,
            acceptance_datetime,form,primary_document,size,is_xbrl,is_inline_xbrl,
            source_url,source_loaded_at
        ) VALUES (
            'SEC-CIK-0000004904','0000004904',?,DATE '2023-05-26',
            DATE '2023-05-26',TIMESTAMP '2023-05-26 11:17:59','8-K',?,93147,
            true,true,'https://data.sec.gov/submissions/fixture.json',
            TIMESTAMP '2023-05-26 11:18:00'
        )
        """,
        [accession, primary_document],
    )
    cover = b"""<html xmlns:ix="http://www.xbrl.org/2013/inlineXBRL">
      <body><ix:nonNumeric name="dei:DocumentType" contextRef="C1">8-K</ix:nonNumeric></body>
    </html>"""
    companion = b"""<html xmlns:ix="http://www.xbrl.org/2013/inlineXBRL"
      xmlns:xbrli="http://www.xbrl.org/2003/instance"
      xmlns:iso4217="http://www.xbrl.org/2003/iso4217">
      <ix:header><ix:resources>
        <xbrli:context id="C1"><xbrli:entity><xbrli:identifier scheme="http://www.sec.gov/CIK">0000004904</xbrli:identifier></xbrli:entity><xbrli:period><xbrli:instant>2023-05-26</xbrli:instant></xbrli:period></xbrli:context>
        <xbrli:unit id="USD"><xbrli:measure>iso4217:USD</xbrli:measure></xbrli:unit>
      </ix:resources></ix:header>
      <body><ix:nonFraction name="us-gaap:Assets" contextRef="C1" unitRef="USD" decimals="0">123</ix:nonFraction></body>
    </html>"""
    index_payload = {
        "directory": {
            "item": [
                {"name": primary_document, "size": str(len(cover))},
                {"name": companion_document, "size": str(len(companion))},
                {"name": "aep-20230526.xsd", "size": "10"},
                {"name": "aep-20230526_cal.xml", "size": "10"},
                {"name": "aep-20230526_def.xml", "size": "10"},
                {"name": "aep-20230526_lab.xml", "size": "10"},
                {"name": "aep-20230526_pre.xml", "size": "10"},
            ]
        }
    }

    class FakeResponse:
        def __init__(self, content: bytes, payload=None) -> None:
            self.content = content
            self._payload = payload

        def raise_for_status(self) -> None:
            return None

        def json(self):
            return self._payload

    class FakeSession:
        def get(self, url: str, timeout: int):
            del timeout
            if url.endswith("index.json"):
                return FakeResponse(json.dumps(index_payload).encode(), index_payload)
            if url.endswith(primary_document):
                return FakeResponse(cover)
            if url.endswith(companion_document):
                return FakeResponse(companion)
            raise AssertionError(url)

    monkeypatch.setattr(xbrl_filing_contexts, "sec_session", lambda _user_agent: FakeSession())

    options = XbrlFilingContextOptions(
        symbols=(),
        forms=(),
        accession_numbers=(accession,),
        max_filings=1,
        source_cache_dir=tmp_path / "source-cache",
        run_id="companion-fallback-test",
    )
    result = XbrlFilingContextDataset().load(
        tmp_store,
        options,
    )

    assert result.details["requests"] == 3
    assert result.details["source_artifacts"] == 3
    assert result.details["source_cache_hits"] == 0
    assert result.details["contexts"] == 1
    assert result.details["facts"] == 2
    assert tmp_store.con.execute(
        """
        SELECT primary_document,instance_document,instance_format
        FROM xbrl_filing_contexts
        WHERE accession_number=?
        """,
        [accession],
    ).fetchone() == (primary_document, companion_document, "inline_xbrl")
    assert tmp_store.con.execute(
        """
        SELECT concept,source_url
        FROM xbrl_filing_facts
        WHERE accession_number=?
        ORDER BY concept
        """,
        [accession],
    ).fetchall() == [
        (
            "Assets",
            "https://www.sec.gov/Archives/edgar/data/4904/"
            f"000000490423000081/{companion_document}",
        ),
        (
            "DocumentType",
            "https://www.sec.gov/Archives/edgar/data/4904/"
            f"000000490423000081/{primary_document}",
        ),
    ]
    cached_sources = tmp_store.con.execute(
        """
        SELECT cache_path,sha256,status
        FROM raw_source_files
        WHERE dataset_id='xbrl_filing_contexts'
        ORDER BY source_url
        """
    ).fetchall()
    assert len(cached_sources) == 3
    assert all(
        cache_path and len(sha256) == 64 and status == "cached"
        for cache_path, sha256, status in cached_sources
    )
    assert all(
        (tmp_path / "source-cache") in Path(path).parents for path, _, _ in cached_sources
    )

    class OfflineSession:
        def get(self, url: str, timeout: int):
            raise AssertionError(f"unexpected network request: {url}, {timeout}")

    monkeypatch.setattr(xbrl_filing_contexts, "sec_session", lambda _user_agent: OfflineSession())
    replay = XbrlFilingContextDataset().load(tmp_store, options)
    assert replay.details["requests"] == 0
    assert replay.details["source_artifacts"] == 3
    assert replay.details["source_cache_hits"] == 3
    assert replay.details["facts"] == 2

    package_payloads = {
        "aep-20230526.xsd": b"extension schema",
        "aep-20230526_cal.xml": b"calculation linkbase",
        "aep-20230526_def.xml": b"definition linkbase",
        "aep-20230526_lab.xml": b"label linkbase",
        "aep-20230526_pre.xml": b"presentation linkbase",
    }

    class PackageSession:
        def get(self, url: str, timeout: int):
            del timeout
            for name, payload in package_payloads.items():
                if url.endswith(name):
                    return FakeResponse(payload)
            raise AssertionError(url)

    monkeypatch.setattr(xbrl_filing_contexts, "sec_session", lambda _user_agent: PackageSession())
    package = XbrlFilingContextDataset().load(
        tmp_store,
        replace(options, capture_filing_package=True, run_id="filing-package-test"),
    )
    assert package.details["requests"] == 5
    assert package.details["source_artifacts"] == 8
    assert package.details["source_cache_hits"] == 3
    assert package.details["filing_package_artifacts"] == 5
    assert tmp_store.con.execute(
        """
        SELECT count(*) FROM raw_source_files
        WHERE dataset_id='xbrl_filing_contexts'
        """
    ).fetchone()[0] == 8
    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=(
            "xbrl_filing_contexts",
            "xbrl_filing_dimensions",
            "xbrl_filing_facts",
        ),
        record=False,
    )
    assert all(check.status == "passed" for check in checks)

LEGACY_INSTANCE = b"""<?xml version="1.0" encoding="UTF-8"?>
<xbrli:xbrl
  xmlns:xbrli="http://www.xbrl.org/2003/instance"
  xmlns:xbrldi="http://xbrl.org/2006/xbrldi"
  xmlns:iso4217="http://www.xbrl.org/2003/iso4217"
  xmlns:us-gaap="http://fasb.org/us-gaap/2010-01-31"
  xmlns:aig="http://www.aig.com/20100331">
  <xbrli:context id="I2010Q1">
    <xbrli:entity>
      <xbrli:identifier scheme="http://www.sec.gov/CIK">0000005272</xbrli:identifier>
      <xbrli:segment>
        <xbrldi:explicitMember dimension="us-gaap:StatementBusinessSegmentsAxis">aig:InsuranceMember</xbrldi:explicitMember>
      </xbrli:segment>
    </xbrli:entity>
    <xbrli:period><xbrli:instant>2010-03-31</xbrli:instant></xbrli:period>
  </xbrli:context>
  <xbrli:unit id="USD"><xbrli:measure>iso4217:USD</xbrli:measure></xbrli:unit>
  <us-gaap:LiabilitiesAndStockholdersEquity contextRef="I2010Q1" unitRef="USD" decimals="-6">100000000</us-gaap:LiabilitiesAndStockholdersEquity>
  <aig:RedeemableNoncontrollingInterest contextRef="I2010Q1" unitRef="USD" decimals="-6">5000000</aig:RedeemableNoncontrollingInterest>
</xbrli:xbrl>
"""


def _filing() -> dict[str, object]:
    return {
        "security_id": "SEC-CIK-0000005272",
        "cik": "0000005272",
        "accession_number": "0001047469-10-004918",
        "form": "10-Q",
        "filing_date": dt.date(2010, 5, 7),
        "report_date": dt.date(2010, 3, 31),
        "acceptance_datetime": dt.datetime(2010, 5, 7, 7, 14, 8),
        "primary_document": "a2198531z10-q.htm",
        "instance_document": "aig-20100331.xml",
        "instance_format": "xbrl_xml",
    }


def test_discover_legacy_instance_document_ignores_linkbases_and_rendered_reports() -> None:
    payload = {
        "directory": {
            "item": [
                {"name": "aig-20100331.xml"},
                {"name": "aig-20100331.xsd"},
                {"name": "aig-20100331_cal.xml"},
                {"name": "aig-20100331_def.xml"},
                {"name": "aig-20100331_lab.xml"},
                {"name": "aig-20100331_pre.xml"},
                {"name": "FilingSummary.xml"},
                {"name": "R1.xml"},
            ]
        }
    }

    assert discover_legacy_instance_document(payload) == "aig-20100331.xml"


def test_discover_legacy_instance_document_rejects_ambiguous_packages() -> None:
    with pytest.raises(ValueError, match="found 2 candidates"):
        discover_legacy_instance_document(
            {
                "directory": {
                    "item": [
                        {"name": "issuer-a.xml"},
                        {"name": "issuer-b.xml"},
                    ]
                }
            }
        )


def test_parse_legacy_xbrl_xml_preserves_instance_and_namespace_lineage() -> None:
    contexts, dimensions, facts = parse_xbrl_filing(
        LEGACY_INSTANCE,
        filing=_filing(),
        source_url=("https://www.sec.gov/Archives/edgar/data/5272/000104746910004918/aig-20100331.xml"),
        run_id="legacy-fixture",
        source_loaded_at=dt.datetime(2026, 8, 14, 12),
    )

    assert len(contexts) == 1
    assert contexts.iloc[0]["period_type"] == "instant"
    assert contexts.iloc[0]["instant_date"] == dt.date(2010, 3, 31)
    assert contexts.iloc[0]["primary_document"] == "a2198531z10-q.htm"
    assert contexts.iloc[0]["instance_document"] == "aig-20100331.xml"
    assert contexts.iloc[0]["instance_format"] == "xbrl_xml"

    assert len(dimensions) == 1
    assert dimensions.iloc[0]["dimension_taxonomy"] == "us-gaap"
    assert dimensions.iloc[0]["member_taxonomy"] == "aig"
    assert dimensions.iloc[0]["member_concept"] == "InsuranceMember"

    assert list(facts["qname"]) == [
        "us-gaap:LiabilitiesAndStockholdersEquity",
        "aig:RedeemableNoncontrollingInterest",
    ]
    assert list(facts["taxonomy"]) == ["us-gaap", "aig"]
    assert list(facts["numeric_value"]) == [100_000_000.0, 5_000_000.0]
    assert set(facts["fact_kind"]) == {"nonFraction"}
    assert set(facts["unit_measures_json"]) == {'["iso4217:USD"]'}
    assert set(facts["instance_document"]) == {"aig-20100331.xml"}


def test_legacy_instance_lineage_migration_is_current(tmp_store: DuckDBStore) -> None:
    assert tmp_store.con.execute(
        "SELECT count(*) FROM schema_migrations WHERE CAST(version AS INTEGER)=279"
    ).fetchone() == (1,)
    for table_name in (
        "xbrl_filing_contexts",
        "xbrl_filing_dimensions",
        "xbrl_filing_facts",
    ):
        columns = {
            row[1]
            for row in tmp_store.con.execute(
                f"PRAGMA table_info('{table_name}')"
            ).fetchall()
        }
        assert {
            "filing_primary_document",
            "instance_document",
            "instance_format",
        } <= columns
