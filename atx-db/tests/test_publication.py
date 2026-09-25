"""Tier1-S4 T8: scheduled full-universe publication with a manifest and a diff.

C4: the manifest's source pins, evidence labels, release gates and eligibility.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import types
from contextlib import contextmanager
from pathlib import Path

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

CREATED_AT = dt.datetime(2026, 9, 19, tzinfo=dt.UTC)
ALL_GATES = ("item_coverage", "provider_slos", "critical_dqc", "source_completeness", "universe_certification")
CERTIFIED_RULES = json.dumps(
    {"evidence_status": "verified_dated", "identity_basis": "verified_dated", "availability_basis": "verified"}
)


def _seed_release_inputs(store, *, universe_rows=2):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-1','CIK-0000000001','AAA','Alpha Inc','test'),"
        "('SEC-2','CIK-0000000002','BBB','Beta Inc','test')"
    )
    values = ",".join(
        f"('m-{i}','us_listed_v1','SEC-{i}','AAA',DATE '2024-01-02',NULL,"
        f"TIMESTAMP '2024-01-02 22:00:00','common','XNAS',true,'000000000{i}',5,'member',"
        f"'{{}}',1,DATE '2024-01-02','t','r')"
        for i in range(1, universe_rows + 1)
    )
    store.con.execute(
        "INSERT INTO universe_us_listed_membership (membership_id, universe_id, security_id, "
        "symbol, valid_from, valid_to, available_at, security_type, exchange_code, has_cik, cik, "
        "market_cap_decile, reason, rules_json, decision_count, as_of_date, source, run_id) "
        "VALUES " + values
    )
    # Include all six relations, multiple sources, unresolved securities and historical
    # revisions: publication must not silently narrow the full warehouse to a sample.
    store.con.execute("""
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, is_latest_revision
        ) VALUES
            ('f1','test','SEC-1',1001,'revenue','annual',DATE '2024-01-02',1,
             DATE '2024-01-02',TIMESTAMP '2024-01-02','[]','[]','test','direct',false),
            ('f2','other','SEC-2',1001,'revenue','annual',DATE '2024-01-02',2,
             DATE '2024-01-02',TIMESTAMP '2024-01-02','[]','[]','test','direct',true);
        INSERT INTO derived_metric_values (
            derived_value_id, source, security_id, metric_code, metric_window, period_end,
            value, available_at, inputs_hash, as_of_date
        ) VALUES ('d1','test','SEC-1','test','q',DATE '2024-01-02',1,
                  TIMESTAMP '2024-01-02','hash',DATE '2024-01-02');
        INSERT INTO market_daily_metrics (
            market_daily_id, source, security_id, trade_date, available_at, inputs_hash, as_of_date
        ) VALUES ('p1','test','SEC-1',DATE '2024-01-02',TIMESTAMP '2024-01-02','hash',DATE '2024-01-02');
        INSERT INTO delisting_events (
            delisting_event_id, source, listing_status_source, source_listing_status_id,
            symbol, delist_date, as_of_date, available_at, delist_code, delist_reason,
            delisting_return_type, return_policy, return_confidence, evidence_source,
            evidence_source_table, method, evidence_confidence
        ) VALUES
            ('e1','test','test','l1','XXX',DATE '2024-01-02',DATE '2024-01-02',
             TIMESTAMP '2024-01-02','test','test','unknown','test','test','test','test','test','test'),
            ('e2','test','test','l2','YYY',DATE '2024-01-02',DATE '2024-01-02',
             TIMESTAMP '2024-01-02','test','test','unknown','test','test','test','test','test','test');
    """)


def _seed_all_gates_passing(con, as_of=dt.date(2026, 9, 18)):
    """A warehouse whose every stored release gate passes, measured on ``as_of``.

    Activation produced every released dataset at 09:00; raw receipts are pinned at 07:00;
    completeness, critical DQC and SLO evidence were recorded at 10:00 (after the data);
    FY2015..FY(as_of.year-1) coverage has 110 items at 95 % on a complete 3000-name cohort
    ranked on the strict-identity panel over the certified strict universe.
    """

    from atx_db.item_coverage import DEFAULT_SOURCE, DEFAULT_UNIVERSE_ID
    from atx_db.market_daily import MARKET_DAILY_STRICT_SOURCE_NAME
    from atx_db.publication import RELEASE_DATASET_STAGES, RELEASE_SOURCES

    def at(hour):
        return dt.datetime.combine(as_of, dt.time(hour))

    _seed_release_inputs(types.SimpleNamespace(con=con))
    con.execute(
        "UPDATE universe_us_listed_membership SET rules_json = ? WHERE universe_id = 'us_listed_v1'", [CERTIFIED_RULES]
    )
    con.executemany(
        "INSERT INTO activation_stage_runs (stage, run_id, status, started_at, finished_at) "
        "VALUES (?, 'run5', 'completed', ?, ?)",
        [[stage, at(8), at(9)] for stage in sorted(set(RELEASE_DATASET_STAGES.values()))],
    )
    con.executemany(
        "INSERT INTO raw_source_files (source_id, dataset_id, source_url, sha256, byte_count, fetched_at, "
        "status, metadata_json) VALUES (?, ?, ?, ?, 1, ?, 'available', '{}')",
        [
            [f"src-{key}", dataset_id, f"file:///{key}", hashlib.sha256(key.encode()).hexdigest(), at(7)]
            for key, dataset_id in RELEASE_SOURCES
        ],
    )
    # A fixture critical check: the DQC gate can never pass on an empty critical set.
    con.execute(
        "INSERT OR REPLACE INTO quality_check_registry (check_name, dataset_id, table_name, severity, enabled) "
        "VALUES ('fixture_critical_check', 'fixture', 'fixture', 'critical', true)"
    )
    con.execute(
        "INSERT INTO data_quality_checks (check_id, dataset_id, table_name, check_name, status, severity, "
        "details_json, checked_at) SELECT 'pass-' || check_name, dataset_id, coalesce(table_name, dataset_id), "
        "check_name, 'passed', 'critical', '{}', ? FROM quality_check_registry WHERE enabled AND severity = 'critical'",
        [at(10)],
    )
    con.executemany(
        "INSERT INTO data_quality_checks (check_id, dataset_id, table_name, check_name, status, severity, "
        "details_json, checked_at) VALUES (?, ?, 'raw_source_files', 'source_completeness', 'passed', 'error', '{}', ?)",
        [[f"complete-{key}", dataset_id, at(10)] for key, dataset_id in RELEASE_SOURCES],
    )
    con.execute(
        """
        INSERT INTO api_schema_coverage_snapshot (coverage_snapshot_id, dataset_id, schema_code, schema_version,
            source_relation, time_column, observed_at, record_count, security_count, condition,
            failed_slos_json, slo_version, run_id)
        SELECT 'slo-' || dataset_id || '-' || schema_code, dataset_id, schema_code, '1', 'fixture', 'as_of_date',
               ?, 1, 1, 'available', '[]', slo_version, 'fixture'
        FROM api_schema_coverage_slo WHERE is_active
        QUALIFY row_number() OVER (PARTITION BY dataset_id, schema_code ORDER BY valid_from DESC, slo_version DESC) = 1
        """,
        [at(10)],
    )
    con.execute(
        """
        INSERT INTO item_coverage_cohort_years (universe_id, fiscal_year, ranking_date, as_of_date, source,
            market_source, rules_json, candidate_count, eligible_count, selected_count, excluded_listing,
            excluded_market_cap, excluded_rank, is_completed_year, status, available_at, run_id)
        SELECT ?, y, make_date(y, 12, 31), ?, 'fixture', ?, ?, 3500, 3200, 3000, 300, 0, 200, true, 'complete', ?,
               'fixture'
        FROM range(2015, ?) t(y)
        """,
        [
            DEFAULT_UNIVERSE_ID,
            as_of,
            MARKET_DAILY_STRICT_SOURCE_NAME,
            json.dumps({"listing_universe_id": "us_listed_v1"}),
            at(22),
            as_of.year,
        ],
    )
    con.execute(
        """
        INSERT INTO fundamental_item_coverage (coverage_id, source, universe_id, item_id, canonical_code, basis,
            fiscal_year, n_securities, n_with_value, coverage_pct, as_of_date, cohort_status)
        SELECT 'cov-' || i || '-' || y, ?, ?, i, 'item_' || i, 'annual', y, 3000, 2850, 95.0, ?, 'complete'
        FROM range(1, 111) a(i), range(2015, ?) b(y)
        """,
        [DEFAULT_SOURCE, DEFAULT_UNIVERSE_ID, as_of, as_of.year],
    )


def _gate_status(manifest):
    return {name: gate["status"] for name, gate in manifest["gates"].items()}


def test_publish_release_writes_a_parquet_per_dataset(tmp_store, tmp_path):
    from atx_db.publication import RELEASE_DATASETS, publish_release

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "2026-09-19", tmp_path, created_at=CREATED_AT, run_id="t")
    assert len(result.datasets) == len(RELEASE_DATASETS)
    for dataset in result.datasets:
        assert dataset.parquet_path.exists()
        assert dataset.parquet_path.parent == tmp_path / "2026-09-19"
    assert {d.name: d.row_count for d in result.datasets} == {
        "security_master": 2,
        "universe": 2,
        "delistings": 2,
        "fundamentals_core": 2,
        "derived_metrics": 1,
        "market_daily": 1,
    }


def test_the_universe_parquet_round_trips(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "2026-09-19", tmp_path, created_at=CREATED_AT, run_id="t")
    universe = next(d for d in result.datasets if d.name == "universe")
    table = pq.read_table(universe.parquet_path)
    assert table.num_rows == 2
    assert universe.row_count == 2
    assert "security_id" in table.column_names


def test_the_manifest_carries_every_required_key(tmp_store, tmp_path):
    from atx_db.publication import PUBLICATION_CONTRACT_VERSION, publish_release, read_release_manifest

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "2026-09-19", tmp_path, created_at=CREATED_AT, run_id="t")
    manifest = read_release_manifest(result.manifest_path)
    assert manifest["release_id"] == "2026-09-19"
    assert manifest["contract_version"] == PUBLICATION_CONTRACT_VERSION
    assert manifest["previous_release_id"] is None
    assert manifest["created_at"] == "2026-09-19T00:00:00"
    assert hashlib.sha256(result.manifest_path.read_bytes()).hexdigest() == result.manifest_sha256
    assert isinstance(manifest["activation_stage_run_ids"], dict)
    assert {"source_pins", "evidence", "evidence_floor", "gates", "eligibility", "gates_not_passed"} <= set(manifest)
    assert set(manifest["source_pins"]) == {"companyfacts", "submissions", "ticker_history", "symbol_directory"}
    assert set(manifest["gates"]) == set(ALL_GATES)
    evidence = manifest["evidence"]
    assert set(evidence) == {d.name for d in result.datasets} | {"forward_returns"}
    # Modeled clocks are labeled as such and never as verified historical vintage.
    assert evidence["fundamentals_core"]["availability_basis"] == "conservative_filing_date_46h"
    assert evidence["fundamentals_core"]["clock_policy"] == "sec_filed_date_plus_46h_v1"
    assert evidence["market_daily"]["availability_basis"] == "modeled_trade_date_22h"
    assert evidence["market_daily"]["share_basis"]["vendor_share_run_clock_verified"] is False
    assert evidence["forward_returns"]["expected_calculation_version"] == "forward_return_publication_v2"
    assert not any(block.get("verified_vintage") for block in evidence.values() if isinstance(block, dict))
    for entry in manifest["datasets"]:
        assert set(entry) == {
            "name",
            "object",
            "schema",
            "schema_sha256",
            "record_schema_sha256",
            "query_sha256",
            "query",
            "columns",
            "key_columns",
            "parquet",
            "parquet_sha256",
            "row_count",
            "byte_count",
            "diff",
        }
        assert len(entry["query_sha256"]) == 64
        assert len(entry["parquet_sha256"]) == 64
        assert len(entry["schema_sha256"]) == 64
        assert hashlib.sha256(entry["query"].encode()).hexdigest() == entry["query_sha256"]
        assert hashlib.sha256((result.out_dir / entry["parquet"]).read_bytes()).hexdigest() == entry["parquet_sha256"]
        assert (
            hashlib.sha256(json.dumps(entry["columns"], sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            == entry["schema_sha256"]
        )


def test_two_identical_releases_produce_identical_hashes(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    first = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT, run_id="t")
    second = publish_release(tmp_store, "r2", tmp_path, created_at=CREATED_AT, previous_dir=first.out_dir, run_id="t")
    by_name_first = {d.name: d for d in first.datasets}
    by_name_second = {d.name: d for d in second.datasets}
    for name, dataset in by_name_first.items():
        assert dataset.query_sha256 == by_name_second[name].query_sha256
        assert dataset.schema_sha256 == by_name_second[name].schema_sha256
        assert dataset.row_count == by_name_second[name].row_count
        assert dataset.parquet_sha256 == by_name_second[name].parquet_sha256
        assert (
            by_name_second[name].rows_added,
            by_name_second[name].rows_removed,
            by_name_second[name].rows_changed,
        ) == (0, 0, 0)


def test_the_diff_reports_added_removed_and_changed(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    first = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT, run_id="t")

    tmp_store.con.execute("UPDATE universe_us_listed_membership SET market_cap_decile = 9 WHERE membership_id = 'm-1'")
    tmp_store.con.execute("DELETE FROM universe_us_listed_membership WHERE membership_id = 'm-2'")
    tmp_store.con.execute(
        "INSERT INTO universe_us_listed_membership (membership_id, universe_id, security_id, "
        "symbol, valid_from, valid_to, available_at, security_type, exchange_code, has_cik, cik, "
        "market_cap_decile, reason, rules_json, decision_count, as_of_date, source, run_id) VALUES "
        "('m-3','us_listed_v1','SEC-3','CCC',DATE '2024-01-02',NULL,"
        "TIMESTAMP '2024-01-02 22:00:00','common','XNAS',false,NULL,3,'member_no_cik','{}',1,"
        "DATE '2024-01-02','t','r')"
    )

    second = publish_release(tmp_store, "r2", tmp_path, created_at=CREATED_AT, previous_dir=first.out_dir, run_id="t")
    universe = next(d for d in second.datasets if d.name == "universe")
    assert universe.rows_added == 1
    assert universe.rows_removed == 1
    assert universe.rows_changed == 1
    assert second.previous_release_id == "r1"


def test_the_release_is_recorded_in_the_warehouse(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    publish_release(tmp_store, "2026-09-19", tmp_path, created_at=CREATED_AT, run_id="t")
    header = tmp_store.con.execute(
        "SELECT dataset_count, previous_release_id FROM publication_releases WHERE release_id = ?",
        ["2026-09-19"],
    ).fetchone()
    assert int(header[0]) == 6
    assert header[1] is None
    rows = tmp_store.con.execute(
        "SELECT count(*) FROM publication_release_datasets WHERE release_id = ?",
        ["2026-09-19"],
    ).fetchone()[0]
    assert int(rows) == 6


def test_the_cli_publishes_a_release(tmp_store, tmp_path, built_warehouse, capsys):
    from atx_db import cli

    db_path = built_warehouse("publish_release.duckdb")
    code = cli.main(
        [
            "publish-release",
            "--db-path",
            str(db_path),
            "--release-id",
            "cli-1",
            "--out-dir",
            str(tmp_path),
        ]
    )
    assert code == 0
    payload = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert payload["release_id"] == "cli-1"
    assert payload["dataset_count"] == 6
    assert (tmp_path / "cli-1" / "manifest.json").exists()


def test_diff_preserves_null_positions_separators_and_nullable_keys(tmp_store, tmp_path):
    from atx_db.publication import ReleaseDataset, diff_against

    before = pa.table(
        {
            "id": [None, "same", "null", "separator", "removed"],
            "a": ["before", "same", None, "a\x1fb", "gone"],
            "b": [None, None, "x", "c", None],
        }
    )
    after = pa.table(
        {
            "id": [None, "same", "null", "separator", "added"],
            "a": ["after", "same", "x", "a", "new"],
            "b": [None, None, None, "b\x1fc", None],
        }
    )
    previous, current = tmp_path / "before.parquet", tmp_path / "after.parquet"
    pq.write_table(before, previous)
    pq.write_table(after, current)
    dataset = ReleaseDataset("sample", "sample", ("id",), None, None)
    assert diff_against(tmp_store, dataset, after.column_names, current, previous) == (1, 1, 3)


@pytest.mark.parametrize("problem", ["duplicate", "schema"])
def test_diff_rejects_ambiguous_inputs(tmp_store, tmp_path, problem):
    from atx_db.publication import ReleaseDataset, diff_against

    before = pa.table({"id": ["x"], "value": [1]})
    after = pa.table({"id": ["x", "x"], "value": [1, 2]}) if problem == "duplicate" else pa.table({"id": ["x"]})
    previous, current = tmp_path / "before.parquet", tmp_path / "after.parquet"
    pq.write_table(before, previous)
    pq.write_table(after, current)
    dataset = ReleaseDataset("sample", "sample", ("id",), None, None)
    with pytest.raises(ValueError, match=r"Duplicate|incompatible"):
        diff_against(tmp_store, dataset, after.column_names, current, previous)


@pytest.mark.parametrize("problem", ["missing_manifest", "missing_file", "tampered_file"])
def test_previous_release_must_be_complete_and_verified(tmp_store, tmp_path, problem):
    from atx_db.publication import publish_release

    first = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    if problem == "missing_manifest":
        first.manifest_path.unlink()
    elif problem == "missing_file":
        first.datasets[0].parquet_path.unlink()
    else:
        first.datasets[0].parquet_path.write_bytes(b"tampered")
    with pytest.raises((FileNotFoundError, ValueError)):
        publish_release(tmp_store, "r2", tmp_path, created_at=CREATED_AT, previous_dir=first.out_dir)
    assert not (tmp_path / "r2").exists()
    assert tmp_store.con.execute("SELECT count(*) FROM publication_releases").fetchone()[0] == 1


def test_published_releases_are_immutable(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    first = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    original = first.manifest_path.read_bytes()
    with pytest.raises(FileExistsError):
        publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    with pytest.raises(ValueError, match="already published"):
        publish_release(tmp_store, "r1", tmp_path / "other", created_at=CREATED_AT)
    assert first.manifest_path.read_bytes() == original
    assert not (tmp_path / "other" / "r1").exists()


@pytest.mark.parametrize("release_id", ["../escape", "C:/escape", "..", "", "r1/child", "r1\\child"])
def test_release_id_cannot_escape_output_directory(tmp_store, tmp_path, release_id):
    from atx_db.publication import publish_release

    with pytest.raises(ValueError, match="directory name"):
        publish_release(tmp_store, release_id, tmp_path, created_at=CREATED_AT)


@pytest.mark.parametrize("failure", ["export", "commit"])
def test_failed_publication_leaves_no_release_or_ledger(tmp_store, tmp_path, monkeypatch, failure):
    from atx_db import publication

    output = tmp_path / "releases"
    if failure == "export":
        original_schema = publication._object_schema

        def broken_schema(store, name):
            if name == "universe_us_listed_membership":
                raise RuntimeError("injected failure")
            return original_schema(store, name)

        monkeypatch.setattr(publication, "_object_schema", broken_schema)
    else:
        original_transaction = tmp_store.transaction

        @contextmanager
        def failed_transaction():
            with original_transaction():
                yield
                assert (output / "r1" / "manifest.json").exists()
                raise RuntimeError("injected failure")

        monkeypatch.setattr(tmp_store, "transaction", failed_transaction)
    with pytest.raises(RuntimeError, match="injected failure"):
        publication.publish_release(tmp_store, "r1", output, created_at=CREATED_AT)
    assert list(output.iterdir()) == []
    assert tmp_store.con.execute("SELECT count(*) FROM publication_releases").fetchone()[0] == 0
    assert tmp_store.con.execute("SELECT count(*) FROM publication_release_datasets").fetchone()[0] == 0


def test_exports_and_activation_provenance_share_one_snapshot(tmp_store, tmp_path, monkeypatch):
    from atx_db import publication

    _seed_release_inputs(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO activation_stage_runs (stage,run_id,status,started_at) VALUES "
        "('universe','old','completed',TIMESTAMP '2024-01-01'),"
        "('universe','failed','failed',TIMESTAMP '2024-01-03')"
    )
    original_schema = publication._object_schema
    # A second connection must match the fixture connection's DuckDB config (conftest bounds it).
    with duckdb.connect(str(tmp_store.path), config={"memory_limit": "1GB", "threads": 1}) as writer:

        def change_after_first_export(store, name):
            if name == "universe_us_listed_membership":
                writer.execute("UPDATE universe_us_listed_membership SET market_cap_decile = 9")
                writer.execute(
                    "INSERT INTO activation_stage_runs (stage,run_id,status,started_at) VALUES "
                    "('universe','new','completed',TIMESTAMP '2024-01-02')"
                )
            return original_schema(store, name)

        monkeypatch.setattr(publication, "_object_schema", change_after_first_export)
        release = publication.publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    universe = pq.read_table(release.out_dir / "universe.parquet")
    assert universe["market_cap_decile"].to_pylist() == [5, 5]
    assert publication.read_release_manifest(release.manifest_path)["activation_stage_run_ids"] == {"universe": "old"}
    assert tmp_store.con.execute("SELECT min(market_cap_decile) FROM universe_us_listed_membership").fetchone()[0] == 9


def test_parquet_bytes_stable_across_thread_settings_and_row_groups(tmp_store, tmp_path, monkeypatch):
    from atx_db import publication

    tmp_store.con.execute(
        "CREATE TABLE publication_large AS SELECT i AS id, 'v' || i AS value FROM range(260000) t(i) ORDER BY i DESC"
    )
    tmp_store.con.execute("INSERT INTO publication_large VALUES (NULL, 'null-key')")
    monkeypatch.setattr(
        publication,
        "RELEASE_DATASETS",
        (publication.ReleaseDataset("large", "publication_large", ("id",), None, None),),
    )
    first = publication.publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    tmp_store.con.execute("SET threads = 4")
    tmp_store.con.execute("SET preserve_insertion_order = false")
    tmp_store.con.execute("SET default_order = 'DESC'")
    tmp_store.con.execute("SET default_null_order = 'NULLS_FIRST'")
    second = publication.publish_release(tmp_store, "r2", tmp_path, created_at=CREATED_AT)
    assert first.datasets[0].parquet_sha256 == second.datasets[0].parquet_sha256
    assert first.datasets[0].query_sha256 == second.datasets[0].query_sha256
    assert first.datasets[0].schema_sha256 == second.datasets[0].schema_sha256
    assert second.total_rows == 260001
    ids = pq.read_table(second.datasets[0].parquet_path)["id"].to_pylist()
    assert ids == [*range(260000), None]
    assert tmp_store.con.execute(
        "SELECT current_setting('threads'), current_setting('preserve_insertion_order'), "
        "current_setting('default_order'), current_setting('default_null_order')"
    ).fetchone() == (4, False, "DESC", "NULLS_FIRST")


def test_cli_refuses_pending_migrations_before_opening_writable_store(tmp_path, monkeypatch):
    from atx_db import cli

    db_path = tmp_path / "warehouse.duckdb"
    events = []

    def pending(path):
        assert path == db_path
        events.append("read-only preflight")
        return [308]

    def forbidden_store(*args, **kwargs):
        pytest.fail("Publication must not open a writable store while migrations are pending")

    monkeypatch.setattr(cli, "pending_migrations", pending)
    monkeypatch.setattr(cli, "DuckDBStore", forbidden_store)
    with pytest.raises(RuntimeError, match="warehouse_migrate") as error:
        cli.main(
            ["publish-release", "--db-path", str(db_path), "--release-id", "r1", "--out-dir", str(tmp_path / "out")]
        )
    assert "308" in str(error.value)
    assert events == ["read-only preflight"]
    assert not db_path.exists()
    assert not (tmp_path / "out").exists()


@pytest.mark.parametrize("existing", [False, True])
def test_cli_read_only_preflight_preserves_missing_or_legacy_database(tmp_path, monkeypatch, existing):
    from atx_db import cli

    db_path = tmp_path / "warehouse.duckdb"
    if existing:
        with duckdb.connect(str(db_path)) as con:
            con.execute("CREATE TABLE sentinel AS SELECT 1 AS value")
    original = db_path.read_bytes() if existing else None

    def forbidden_store(*args, **kwargs):
        pytest.fail("Preflight must not bootstrap the warehouse")

    monkeypatch.setattr(cli, "DuckDBStore", forbidden_store)
    with pytest.raises(RuntimeError, match="pending schema migrations"):
        cli.main(
            ["publish-release", "--db-path", str(db_path), "--release-id", "r1", "--out-dir", str(tmp_path / "out")]
        )
    assert (db_path.read_bytes() if db_path.exists() else None) == original
    assert not (tmp_path / "out").exists()


def test_release_query_pins_sort_direction_and_null_order():
    from atx_db.publication import ReleaseDataset, release_query

    dataset = ReleaseDataset("sample", "sample", ("id", "date"), None, None)
    assert release_query(dataset, ["id", "date", "value"]) == (
        'SELECT "id", "date", "value" FROM "sample" ORDER BY "id" ASC NULLS LAST, "date" ASC NULLS LAST'
    )


# --- C4: release gates, eligibility and honest evidence ---------------------------------


def test_eligible_is_impossible_unless_every_required_gate_passed():
    from atx_db.publication import release_eligibility

    passed = {gate: {"status": "passed"} for gate in ALL_GATES}
    assert release_eligibility(passed) == ("eligible", ())
    for gate in ALL_GATES:
        for status in ("failed", "unmeasured", None):
            assert release_eligibility({**passed, gate: {"status": status}}) == ("candidate", (gate,))
        omitted = {name: value for name, value in passed.items() if name != gate}
        assert release_eligibility(omitted) == ("candidate", (gate,))
    with pytest.raises(ValueError, match="unknown release gates"):
        release_eligibility({**passed, "invented": {"status": "passed"}})


def test_all_gates_passing_fixture_is_eligible(tmp_store, tmp_path):
    from atx_db.publication import publish_release, read_release_manifest

    _seed_all_gates_passing(tmp_store.con)
    result = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    manifest = read_release_manifest(result.manifest_path)
    assert _gate_status(manifest) == dict.fromkeys(ALL_GATES, "passed")
    assert manifest["eligibility"] == result.eligibility == "eligible"
    assert manifest["gates_not_passed"] == [] and result.gates_not_passed == ()
    coverage = manifest["gates"]["item_coverage"]
    assert coverage["items_meeting_every_completed_year"] == 110
    assert coverage["completed_fiscal_years"] == [2015, 2025]
    assert [year["fiscal_year"] for year in coverage["cohort_years"] if year["certified"]] == list(range(2015, 2026))
    slos = manifest["gates"]["provider_slos"]
    assert slos["available_slos"] == slos["active_slos"] >= slos["required_slos"] >= 12
    assert manifest["gates"]["critical_dqc"]["outcomes"]["passed"] >= 1
    assert manifest["source_pins"]["companyfacts"]["sha256"] == [
        {
            "sha256": hashlib.sha256(b"companyfacts").hexdigest(),
            "receipts": 1,
            "last_fetched_at": "2026-09-18T07:00:00",
        }
    ]
    assert manifest["evidence_floor"]["floor"] == "2026-09-18T09:00:00"


def _set_strict_builder_labels(con):
    from atx_db.universe_us_listed import UniverseUsListedOptions, _rules

    con.execute(
        "UPDATE universe_us_listed_membership SET rules_json = ? WHERE universe_id = 'us_listed_v1'",
        [json.dumps(_rules(UniverseUsListedOptions(), "strict"))],
    )


_FAILED_DQC = (
    "INSERT INTO data_quality_checks (check_id, dataset_id, table_name, check_name, status, severity, details_json, "
    "checked_at) VALUES (?, ?, ?, ?, 'failed', ?, '{}', TIMESTAMP '2026-09-18 11:00:00')"
)
# scenario -> (break one stored measurement of the all-pass fixture, gates that stop passing)
_BREAKS = {
    # One item misses 90 % in FY2019 only: 109 < 110 items over every completed year.
    "coverage_item_below_target": (
        lambda con: con.execute(
            "UPDATE fundamental_item_coverage SET n_with_value = 2600, coverage_pct = 100.0 * 2600 / 3000 "
            "WHERE item_id = 7 AND fiscal_year = 2019"
        ),
        {"item_coverage": "failed"},
    ),
    # FY2020 cohort ranked by market cap from the reconstructed-identity panel.
    "cohort_ranked_on_reconstructed_panel": (
        lambda con: con.execute(
            "UPDATE item_coverage_cohort_years SET market_source = 'atx-db daily market panel v1' "
            "WHERE fiscal_year = 2020"
        ),
        {"item_coverage": "failed"},
    ),
    "slo_degraded": (
        lambda con: con.execute(
            "UPDATE api_schema_coverage_snapshot SET condition = 'degraded' WHERE schema_code = 'market-daily-1d'"
        ),
        {"provider_slos": "failed"},
    ),
    "critical_dqc_failed": (
        lambda con: con.execute(_FAILED_DQC, ["f1", "fixture", "fixture", "fixture_critical_check", "critical"]),
        {"critical_dqc": "failed"},
    ),
    # market_daily rebuilt after the SLO/DQC evidence was recorded: that evidence is stale.
    "market_rebuilt_after_evidence": (
        lambda con: con.execute(
            "UPDATE activation_stage_runs SET finished_at = TIMESTAMP '2026-09-18 11:00:00' WHERE stage = 'market_daily'"
        ),
        {"provider_slos": "unmeasured", "critical_dqc": "unmeasured"},
    ),
    "no_activation_provenance": (
        lambda con: con.execute("DELETE FROM activation_stage_runs"),
        {"provider_slos": "unmeasured", "critical_dqc": "unmeasured"},
    ),
    "companyfacts_source_incomplete": (
        lambda con: con.execute(
            _FAILED_DQC, ["f2", "sec_company_facts", "sec_company_facts", "source_completeness", "error"]
        ),
        {"source_completeness": "failed"},
    ),
    # A newer TickerHistory file landed after completeness was last checked.
    "ticker_history_receipt_newer_than_completeness": (
        lambda con: con.execute(
            "UPDATE raw_source_files SET fetched_at = TIMESTAMP '2026-09-18 12:00:00' "
            "WHERE dataset_id = 'tbltickerhistory_daily'"
        ),
        {"source_completeness": "unmeasured"},
    ),
    # The strict builder's own labels (PIT directory snapshot carried forward): neither the
    # universe nor the coverage cohort built on it is certified.
    "strict_universe_snapshot_labels": (
        _set_strict_builder_labels,
        {"item_coverage": "failed", "universe_certification": "failed"},
    ),
}


@pytest.mark.parametrize("scenario", sorted(_BREAKS))
def test_any_gate_not_passing_makes_the_release_a_candidate(tmp_store, tmp_path, scenario):
    from atx_db.publication import publish_release, read_release_manifest

    breaker, expected = _BREAKS[scenario]
    _seed_all_gates_passing(tmp_store.con)
    breaker(tmp_store.con)
    result = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    manifest = read_release_manifest(result.manifest_path)
    assert _gate_status(manifest) == {gate: expected.get(gate, "passed") for gate in ALL_GATES}
    assert manifest["eligibility"] == result.eligibility == "candidate"
    assert manifest["gates_not_passed"] == [gate for gate in ALL_GATES if gate in expected]
    assert result.gates_not_passed == tuple(manifest["gates_not_passed"])


def test_coverage_measured_before_the_release_year_is_unmeasured(tmp_store, tmp_path):
    from atx_db.publication import publish_release, read_release_manifest

    _seed_all_gates_passing(tmp_store.con)
    # A 2027 release cannot certify FY2026 from a 2026 measurement (FY2015-FY2025 only).
    result = publish_release(tmp_store, "r1", tmp_path, created_at=dt.datetime(2027, 2, 1, tzinfo=dt.UTC))
    gate = read_release_manifest(result.manifest_path)["gates"]["item_coverage"]
    assert gate["status"] == "unmeasured"
    assert gate["unmeasured_reasons"] == ["measurement_not_in_release_year"]
    assert result.eligibility == "candidate"


def test_identical_rerun_writes_a_byte_identical_manifest(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_all_gates_passing(tmp_store.con)
    first = publish_release(tmp_store, "r1", tmp_path / "a", created_at=CREATED_AT, run_id="t")
    # Rerun the same release on the same warehouse state: only the ledger row blocks the
    # reused release id, and the ledger is not a manifest input.
    tmp_store.con.execute("DELETE FROM publication_release_datasets WHERE release_id = 'r1'")
    tmp_store.con.execute("DELETE FROM publication_releases WHERE release_id = 'r1'")
    # Session defaults must not reorder any manifest list.
    tmp_store.con.execute("SET default_order = 'DESC'")
    tmp_store.con.execute("SET default_null_order = 'NULLS_FIRST'")
    second = publish_release(tmp_store, "r1", tmp_path / "b", created_at=CREATED_AT, run_id="t")
    assert first.manifest_path.read_bytes() == second.manifest_path.read_bytes()
    assert first.manifest_sha256 == second.manifest_sha256
    assert first.eligibility == second.eligibility == "eligible"


def test_evidence_labels_reconstruction_inference_and_policy_honestly(tmp_store, tmp_path):
    from atx_db.publication import publish_release, read_release_manifest
    from atx_db.universe_us_listed import UniverseUsListedOptions, _rules

    con = tmp_store.con
    _seed_release_inputs(tmp_store)
    _set_strict_builder_labels(con)
    con.execute(
        "INSERT INTO universe_us_listed_membership (membership_id, universe_id, security_id, symbol, valid_from, "
        "valid_to, available_at, security_type, exchange_code, has_cik, cik, market_cap_decile, reason, rules_json, "
        "decision_count, as_of_date, source, run_id) VALUES ('m-r', 'us_listed_reconstructed_v1', 'SEC-9', 'ZZZ', "
        "DATE '2020-01-02', NULL, TIMESTAMP '2020-01-02 22:00:00', 'unknown', 'UNKNOWN', false, NULL, 3, "
        "'reconstructed_no_listing_evidence', ?, 1, DATE '2026-09-18', 't', 'r')",
        [json.dumps(_rules(UniverseUsListedOptions(), "reconstructed"))],
    )
    con.execute("UPDATE delisting_events SET inferred_from_absence = true WHERE delisting_event_id = 'e1'")
    con.execute("""
        INSERT INTO market_daily_metrics (market_daily_id, source, security_id, trade_date, available_at, inputs_hash,
            as_of_date, identity_basis, availability_basis, link_method, shares_source) VALUES
        ('p2', 'test', 'SEC-2', DATE '2024-01-02', TIMESTAMP '2024-01-02 22:00:00', 'h', DATE '2024-01-02',
         'current_ticker_unverified', 'modeled', 'current_sec_ticker', 'dei'),
        ('p3', 'test', 'SEC-2', DATE '2024-01-03', TIMESTAMP '2024-01-03 22:00:00', 'h', DATE '2024-01-03',
         'verified_dated', 'verified', 'dated_evidence', 'archive')
    """)
    v2 = "forward_return_publication_v2"
    con.execute(
        """
        INSERT INTO forward_returns_survivorship_safe (forward_return_id, source, security_id, as_of_date,
            horizon_days, forward_return, is_delisted_in_horizon, is_stitched, terminal_return_source,
            available_at, calculation_version) VALUES
        ('fr1', 't', 'SEC-1', DATE '2024-01-02', 21, 0.1, false, false, NULL, TIMESTAMP '2024-02-01', ?),
        ('fr2', 't', 'SEC-1', DATE '2024-01-03', 21, -0.3, true, true, 'observed', TIMESTAMP '2024-02-01', ?),
        ('fr3', 't', 'SEC-2', DATE '2024-01-03', 21, -0.4, true, true, 'policy', TIMESTAMP '2024-02-01', ?),
        ('fr4', 't', 'SEC-2', DATE '2024-01-04', 21, 0.0, false, false, NULL, TIMESTAMP '2024-02-01', NULL)
        """,
        [v2, v2, v2],
    )
    result = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    manifest = read_release_manifest(result.manifest_path)
    evidence = manifest["evidence"]
    universes = evidence["universe"]["universes"]
    assert universes["us_listed_v1"]["certification_status"] == "not_certified"
    assert universes["us_listed_v1"]["labels"][0]["evidence_status"] == "pit_snapshot"
    assert universes["us_listed_reconstructed_v1"]["certification_status"] == "reconstructed"
    assert universes["us_listed_reconstructed_v1"]["verified_rows"] == 0
    assert manifest["gates"]["universe_certification"]["failed_reasons"] == ["strict_universe_not_certified"]
    identity = evidence["market_daily"]["owner_identity"]
    assert (identity["verified_identity_rows"], identity["unlinked_or_unlabeled_rows"]) == (1, 1)
    assert {"source": "test", "shares_source": "dei", "rows": 1} in (
        evidence["market_daily"]["share_basis"]["rows_by_shares_source"]
    )
    delistings = evidence["delistings"]
    assert (delistings["rows"], delistings["inferred_from_absence_rows"], delistings["unresolved_security_rows"]) == (
        2,
        1,
        2,
    )
    assert delistings["verified_vintage"] is False
    forward = evidence["forward_returns"]
    assert (
        forward["rows"],
        forward["current_version_rows"],
        forward["unversioned_legacy_rows"],
        forward["stitched_observed_terminal_rows"],
        forward["stitched_policy_terminal_rows"],
    ) == (4, 3, 1, 1, 1)
    assert result.eligibility == "candidate"


def test_empty_evidence_is_never_a_pass(tmp_store, tmp_path):
    from atx_db.publication import publish_release, read_release_manifest

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    manifest = read_release_manifest(result.manifest_path)
    assert _gate_status(manifest) == {
        "item_coverage": "unmeasured",
        "provider_slos": "unmeasured",
        "critical_dqc": "unmeasured",
        "source_completeness": "unmeasured",
        # Rows exist, but carry no verified labels.
        "universe_certification": "failed",
    }
    assert result.eligibility == "candidate" and result.gates_not_passed == ALL_GATES
    assert manifest["source_pins"]["ticker_history"]["receipts"] == 0


def _load_publish_script():
    path = Path(__file__).resolve().parents[1] / "scripts" / "publish_release.py"
    spec = importlib.util.spec_from_file_location("publish_release_under_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("all_pass", [False, True])
def test_publish_script_exit_code_distinguishes_candidate_from_eligible(built_warehouse, tmp_path, capsys, all_pass):
    from atx_db.publication import CANDIDATE_EXIT_CODE

    db_path = built_warehouse("publish_script.duckdb")
    if all_pass:
        with duckdb.connect(str(db_path)) as con:
            # The CLI stamps the release with the wall clock: measure "today".
            _seed_all_gates_passing(con, as_of=dt.datetime.now(dt.UTC).date())
    out_dir = tmp_path / "out"
    code = _load_publish_script().main(["--db-path", str(db_path), "--release-id", "s1", "--out-dir", str(out_dir)])
    report = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    manifest = json.loads((out_dir / "s1" / "manifest.json").read_text(encoding="utf-8"))
    assert report["eligibility"] == manifest["eligibility"] == ("eligible" if all_pass else "candidate")
    assert report["gates_not_passed"] == manifest["gates_not_passed"] == ([] if all_pass else list(ALL_GATES))
    assert CANDIDATE_EXIT_CODE not in (0, 1, 2)
    assert code == (0 if all_pass else CANDIDATE_EXIT_CODE)


def test_release_producing_stages_run_before_the_gate_evidence_stages():
    """One full activation always leaves SLO and DQC evidence fresh (not stale)."""

    from atx_db.activation import STAGE_ORDER
    from atx_db.publication import RELEASE_DATASET_STAGES, RELEASE_DATASETS

    order = {stage: index for index, stage in enumerate(STAGE_ORDER)}
    assert set(RELEASE_DATASET_STAGES) == {dataset.name for dataset in RELEASE_DATASETS}
    for stage in RELEASE_DATASET_STAGES.values():
        assert order[stage] < order["provider_coverage"] < order["quality"]
