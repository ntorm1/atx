from __future__ import annotations

import subprocess
from pathlib import Path

from atx_db import xbrl_processor
from atx_db.quality import run_warehouse_quality_checks
from atx_db.source_cache import cache_source_payload, source_url_materialization_path
from atx_db.warehouse import record_source_file
from atx_db.xbrl_processor import (
    ArelleValidationOptions,
    ArelleValidationResult,
    parse_arelle_xml_log,
    run_arelle_validation,
)

ARELLE_LOG = b"""<?xml version="1.0" encoding="utf-8"?>
<log>
  <entry code="EFM.6.05.16" level="error">
    <message qname="dei:DocumentType">[EFM.6.05.16] Invalid document type</message>
    <ref href="filing.htm#fact-1" sourceLine="17">
      <property name="QName" value="dei:DocumentType"/>
    </ref>
  </entry>
  <entry code="calc11e:inconsistentCalculation" level="warning-semantic">
    <message>[calc11e:inconsistentCalculation] Calculation differs</message>
  </entry>
</log>
"""


def _filing() -> dict[str, object]:
    return {
        "security_id": "SEC-CIK-0000004904",
        "symbol": "AEP",
        "cik": "0000004904",
        "accession_number": "0000004904-23-000081",
        "instance_format": "inline_xbrl",
        "source_urls": (
            "https://www.sec.gov/Archives/aep-cover.htm",
            "https://www.sec.gov/Archives/aep-report.htm",
        ),
    }


def test_parse_arelle_xml_log_preserves_message_and_reference_payloads() -> None:
    findings = parse_arelle_xml_log(ARELLE_LOG)

    assert [(finding.message_code, finding.severity) for finding in findings] == [
        ("EFM.6.05.16", "error"),
        ("calc11e:inconsistentCalculation", "warning-semantic"),
    ]
    assert findings[0].message_attributes == {"qname": "dei:DocumentType"}
    assert findings[0].references == [
        {
            "attributes": {"href": "filing.htm#fact-1", "sourceLine": "17"},
            "properties": [{"name": "QName", "value": "dei:DocumentType"}],
        }
    ]


def test_arelle_validation_persists_versioned_structured_findings(
    tmp_path,
    tmp_store,
    monkeypatch,
) -> None:
    monkeypatch.setattr(xbrl_processor, "_target_filings", lambda _store, _options: [_filing()])
    monkeypatch.setattr(xbrl_processor, "_processor_version", lambda: "2.44.2")
    monkeypatch.setattr(xbrl_processor.shutil, "which", lambda _executable: "arelleCmdLine")

    def execute(command: list[str], *, timeout: int) -> subprocess.CompletedProcess[str]:
        assert timeout == 30
        assert "--efm" not in command
        assert command[command.index("--calc") + 1] == "c11r"
        assert command[command.index("--plugins") + 1] == "inlineXbrlDocumentSet"
        assert '"ixds"' in command[command.index("--file") + 1]
        Path(command[command.index("--logFile") + 1]).write_bytes(ARELLE_LOG)
        return subprocess.CompletedProcess(command, 0, "", "")

    monkeypatch.setattr(xbrl_processor, "_execute_arelle", execute)
    options = ArelleValidationOptions(
        max_filings=1,
        internet_connectivity="offline",
        processor_cache_dir=tmp_path / "arelle-cache",
        process_timeout=30,
        run_id="arelle-fixture-run",
    )

    first = run_arelle_validation(tmp_store, options)
    second = run_arelle_validation(tmp_store, options)

    assert (
        first.attempted_count,
        first.succeeded_count,
        first.finding_count,
        first.error_count,
        first.warning_count,
    ) == (1, 1, 2, 1, 1)
    assert second.succeeded_count == 1
    assert tmp_store.con.execute(
        """
        SELECT count(*),count(*) FILTER (WHERE is_latest_revision),
               min(dts_resolution_status),min(validation_outcome)
        FROM xbrl_processor_runs
        """
    ).fetchone() == (2, 1, "resolved", "validation_errors")
    assert tmp_store.con.execute(
        """
        SELECT count(*),count(*) FILTER (WHERE is_latest_revision)
        FROM xbrl_processor_findings
        """
    ).fetchone() == (4, 2)
    assert tmp_store.con.execute(
        """
        SELECT severity,message_code,message_attributes_json,references_json
        FROM xbrl_processor_findings
        ORDER BY available_at,ordinal
        LIMIT 1
        """
    ).fetchone() == (
        "error",
        "EFM.6.05.16",
        '{"qname": "dei:DocumentType"}',
        '[{"attributes": {"href": "filing.htm#fact-1", "sourceLine": "17"}, '
        '"properties": [{"name": "QName", "value": "dei:DocumentType"}]}]',
    )
    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("xbrl_processor_runs", "xbrl_processor_findings"),
        record=False,
    )
    assert all(check.status == "passed" for check in checks)


def test_arelle_nonzero_exit_preserves_exit_code_and_log_findings(
    tmp_path,
    tmp_store,
    monkeypatch,
) -> None:
    monkeypatch.setattr(xbrl_processor, "_target_filings", lambda _store, _options: [_filing()])
    monkeypatch.setattr(xbrl_processor, "_processor_version", lambda: "2.44.2")
    monkeypatch.setattr(xbrl_processor.shutil, "which", lambda _executable: "arelleCmdLine")

    def execute(command: list[str], *, timeout: int) -> subprocess.CompletedProcess[str]:
        Path(command[command.index("--logFile") + 1]).write_bytes(ARELLE_LOG)
        return subprocess.CompletedProcess(command, 2, "", "processor failure")

    monkeypatch.setattr(xbrl_processor, "_execute_arelle", execute)
    result = run_arelle_validation(
        tmp_store,
        ArelleValidationOptions(
            internet_connectivity="offline",
            processor_cache_dir=tmp_path / "arelle-cache",
            run_id="arelle-nonzero",
        ),
    )

    assert (result.failed_count, result.finding_count, result.error_count) == (1, 2, 1)
    assert tmp_store.con.execute(
        """
        SELECT status,exit_code,finding_count,error_type,
               dts_resolution_status,validation_outcome
        FROM xbrl_processor_runs
        WHERE run_id='arelle-nonzero'
        """
    ).fetchone() == (
        "failed",
        2,
        2,
        "RuntimeError",
        "not_evaluated",
        "processor_failed",
    )


def test_arelle_efm_profile_requires_and_loads_explicit_sec_plugin(tmp_path) -> None:
    plugin_path = tmp_path / "EDGAR"
    plugin_path.mkdir()
    options = ArelleValidationOptions(efm_plugin_path=plugin_path)

    command = xbrl_processor._build_command(
        executable="arelleCmdLine",
        entrypoint="filing.xml",
        log_path=tmp_path / "log.xml",
        cache_dir=tmp_path / "cache",
        options=options,
    )

    assert "--efm" in command
    assert command[command.index("--plugins") + 1] == str(plugin_path.resolve())
    assert xbrl_processor._validation_profile(options) == (
        "xbrl21_efm_calc11_round_to_nearest"
    )


def test_arelle_entrypoints_use_verified_url_materialization(tmp_path, tmp_store) -> None:
    filing = _filing()
    expected_paths: list[Path] = []
    for index, source_url in enumerate(filing["source_urls"]):
        cached = cache_source_payload(tmp_path / "objects", f"document {index}".encode())
        record_source_file(
            tmp_store,
            dataset_id="xbrl_filing_contexts",
            source_url=str(source_url),
            cache_path=cached.cache_path,
            sha256=cached.sha256,
        )
        expected_paths.append(
            source_url_materialization_path(tmp_path / "packages", str(source_url))
        )

    materialized = xbrl_processor._materialize_entrypoint_sources(
        tmp_store,
        filing=filing,
        materialized_root=tmp_path / "packages",
    )

    assert materialized["source_urls"] == tuple(path.as_posix() for path in expected_paths)
    assert [path.read_bytes() for path in expected_paths] == [b"document 0", b"document 1"]
    assert Path(materialized["inline_archive_path"]).is_file()
    assert materialized["inline_archive_member_count"] == 2
    assert xbrl_processor._entrypoint(materialized).endswith(".zip")


def test_arelle_xml_entrypoint_materializes_adjacent_extension_schema(
    tmp_path,
    tmp_store,
) -> None:
    directory = "https://www.sec.gov/Archives/edgar/data/820027/filing/"
    instance_url = f"{directory}amp-20100930.xml"
    schema_url = f"{directory}amp-20100930.xsd"
    for source_url, content in (
        (instance_url, b"instance"),
        (schema_url, b"schema"),
    ):
        cached = cache_source_payload(tmp_path / "objects", content)
        record_source_file(
            tmp_store,
            dataset_id="xbrl_filing_contexts",
            source_url=source_url,
            cache_path=cached.cache_path,
            sha256=cached.sha256,
        )
    filing = {
        **_filing(),
        "instance_format": "xbrl_xml",
        "source_urls": (instance_url,),
    }

    materialized = xbrl_processor._materialize_entrypoint_sources(
        tmp_store,
        filing=filing,
        materialized_root=tmp_path / "packages",
    )

    assert "inline_archive_path" not in materialized
    assert Path(str(materialized["source_urls"][0])).read_bytes() == b"instance"
    assert source_url_materialization_path(
        tmp_path / "packages", schema_url
    ).read_bytes() == b"schema"


def test_taxonomy_package_loading_findings_mark_dts_incomplete() -> None:
    findings = parse_arelle_xml_log(
        b"""<log><entry code="arelle:packageLoadingError" level="error">
        <message>Package has no metadata</message></entry></log>"""
    )

    assert xbrl_processor._semantic_outcome(
        status="succeeded",
        findings=findings,
    ) == ("incomplete", "incomplete_dts")


def test_arelle_taxonomy_package_manifest_hashes_directory_zips(tmp_path) -> None:
    package_dir = tmp_path / "packages"
    package_dir.mkdir()
    (package_dir / "b.zip").write_bytes(b"second taxonomy")
    (package_dir / "a.zip").write_bytes(b"first taxonomy")
    (package_dir / "ignored.txt").write_text("not a package")
    options = ArelleValidationOptions(taxonomy_package_paths=(package_dir,))

    manifest = xbrl_processor._taxonomy_package_manifest(options)
    command = xbrl_processor._build_command(
        executable="arelleCmdLine",
        entrypoint="filing.xml",
        log_path=tmp_path / "log.xml",
        cache_dir=tmp_path / "cache",
        options=options,
    )

    assert [Path(item["path"]).name for item in manifest] == ["a.zip", "b.zip"]
    assert all(len(item["sha256"]) == 64 for item in manifest)
    assert command[command.index("--packages") + 1] == str(package_dir.resolve())


def test_arelle_cli_maps_bounded_validation_options(tmp_path, monkeypatch, capsys) -> None:
    from atx_db import cli

    captured: list[ArelleValidationOptions] = []

    def fake_run(_store, options: ArelleValidationOptions) -> ArelleValidationResult:
        captured.append(options)
        return ArelleValidationResult(
            attempted_count=1,
            succeeded_count=1,
            failed_count=0,
            unavailable_count=0,
            finding_count=2,
            error_count=1,
            warning_count=1,
        inconsistency_count=0,
        processor_version="2.44.2",
        validation_profile="xbrl21_calc11_round_to_nearest",
        run_id="arelle-cli-run",
        )

    monkeypatch.setattr(cli, "run_arelle_validation", fake_run)
    code = cli.main(
        [
            "run-arelle-validation",
            "--db-path",
            str(tmp_path / "cli.duckdb"),
            "--accession",
            "0000004904-23-000081",
            "--max-filings",
            "1",
            "--process-timeout",
            "30",
            "--run-id",
            "arelle-cli-run",
        ]
    )

    assert code == 0
    assert captured == [
        ArelleValidationOptions(
            accession_numbers=("0000004904-23-000081",),
            max_filings=1,
            internet_connectivity="offline",
            process_timeout=30,
            run_id="arelle-cli-run",
        )
    ]
    assert '"finding_count": 2' in capsys.readouterr().out


def test_arelle_unavailable_is_durable_not_an_import_failure(
    tmp_store,
    monkeypatch,
) -> None:
    monkeypatch.setattr(xbrl_processor, "_target_filings", lambda _store, _options: [_filing()])
    monkeypatch.setattr(xbrl_processor, "_processor_version", lambda: "unavailable")
    monkeypatch.setattr(xbrl_processor.shutil, "which", lambda _executable: None)

    result = run_arelle_validation(
        tmp_store,
        ArelleValidationOptions(run_id="arelle-unavailable"),
    )

    assert (result.attempted_count, result.unavailable_count, result.failed_count) == (1, 1, 0)
    assert tmp_store.con.execute(
        """
        SELECT status,error_type,error_message
        FROM xbrl_processor_runs
        WHERE run_id='arelle-unavailable'
        """
    ).fetchone() == (
        "unavailable",
        "RuntimeError",
        "Arelle is unavailable; install the atx-db[xbrl] optional extra",
    )
