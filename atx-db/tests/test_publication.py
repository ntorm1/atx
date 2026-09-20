"""Tier1-S4 T8: scheduled full-universe publication with a manifest and a diff."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from contextlib import contextmanager

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

CREATED_AT = dt.datetime(2026, 9, 19, tzinfo=dt.UTC)


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
    with duckdb.connect(str(tmp_store.path)) as writer:

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
    monkeypatch.setattr(
        publication,
        "RELEASE_DATASETS",
        (publication.ReleaseDataset("large", "publication_large", ("id",), None, None),),
    )
    first = publication.publish_release(tmp_store, "r1", tmp_path, created_at=CREATED_AT)
    tmp_store.con.execute("SET threads = 4")
    tmp_store.con.execute("SET preserve_insertion_order = false")
    second = publication.publish_release(tmp_store, "r2", tmp_path, created_at=CREATED_AT)
    assert first.datasets[0].parquet_sha256 == second.datasets[0].parquet_sha256
    assert second.total_rows == 260000
    assert tmp_store.con.execute(
        "SELECT current_setting('threads'), current_setting('preserve_insertion_order')"
    ).fetchone() == (4, False)
