from __future__ import annotations

import datetime as dt
import hashlib
import json
from decimal import Decimal

import duckdb
import pyarrow as pa
import pyarrow.csv as pa_csv
import pyarrow.ipc as pa_ipc
import pyarrow.parquet as pq
from fastapi.testclient import TestClient

from atx_db.api.admin import (
    grant_entitlement,
    issue_api_key,
    revoke_api_key,
    set_unit_price,
    upsert_account,
)
from atx_db.api.app import create_app
from atx_db.api.auth import ApiPrincipal, StaticApiKeyAuthenticator
from atx_db.api.batch import InMemoryBatchJobRepository, LocalBatchManager
from atx_db.api.catalog import DATASETS
from atx_db.api.models import BatchSubmitRequest, RangeRequest
from atx_db.api.service import WarehouseReadService
from atx_db.api.usage import InMemoryUsageLedger, UsageEvent
from atx_db.api.worker import BatchWorker, build_worker

API_KEY = "atx_test_000000000000000000000000"


def _seed_fundamental_revisions(store) -> None:
    store.con.execute(
        """
        INSERT INTO securities (
            security_id,entity_id,issuer_id,primary_symbol,name,asset_class,country,
            currency,active,first_seen_date,last_seen_date,source,source_loaded_at
        ) VALUES (
            'SEC-CIK-0000320193','ENTITY-AAPL','ISSUER-AAPL','AAPL','Apple Inc.',
            'equity','US','USD',true,DATE '1980-12-12',NULL,'test',TIMESTAMP '2020-01-01'
        )
        """
    )
    store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id,id_type,id_value,valid_from,valid_to,as_of_date,available_at,
            source,run_id,source_loaded_at,is_latest_revision
        ) VALUES (
            'SEC-CIK-0000320193','TICKER','AAPL',DATE '1980-12-12',NULL,
            DATE '1980-12-12',TIMESTAMP '2020-01-01','test','seed',TIMESTAMP '2020-01-01',true
        )
        """
    )
    store.con.executemany(
        """
        INSERT INTO fundamental_standardized (
            standardized_id,source,upstream_source,security_id,symbol,cik,item_id,
            canonical_code,basis,period_start,period_end,fiscal_year,fiscal_period,
            value,unit_type,source_accession,filed_date,as_of_date,available_at,
            input_codes_json,input_item_ids_json,rule_id,combination_rule,
            is_latest_revision,run_id,source_loaded_at,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            (
                "std-first",
                "test",
                "sec",
                "SEC-CIK-0000320193",
                "AAPL",
                "0000320193",
                1,
                "revenue",
                "annual",
                dt.date(2023, 1, 1),
                dt.date(2023, 12, 31),
                2023,
                "FY",
                100.0,
                "USD",
                "0001",
                dt.date(2024, 2, 1),
                dt.date(2023, 12, 31),
                dt.datetime(2024, 2, 1, 12),
                '["Revenues"]',
                "[1]",
                "rule-revenue",
                "single",
                False,
                "run-first",
                dt.datetime(2024, 2, 1, 12),
                dt.datetime(2024, 2, 1, 12),
            ),
            (
                "std-restated",
                "test",
                "sec",
                "SEC-CIK-0000320193",
                "AAPL",
                "0000320193",
                1,
                "revenue",
                "annual",
                dt.date(2023, 1, 1),
                dt.date(2023, 12, 31),
                2023,
                "FY",
                110.0,
                "USD",
                "0002",
                dt.date(2024, 3, 1),
                dt.date(2023, 12, 31),
                dt.datetime(2024, 3, 1, 12),
                '["Revenues"]',
                "[1]",
                "rule-revenue",
                "single",
                True,
                "run-restated",
                dt.datetime(2024, 3, 1, 12),
                dt.datetime(2024, 3, 1, 12),
            ),
            (
                "std-end-exclusive",
                "test",
                "sec",
                "SEC-CIK-0000320193",
                "AAPL",
                "0000320193",
                1,
                "revenue",
                "annual",
                dt.date(2024, 1, 1),
                dt.date(2024, 1, 1),
                2024,
                "Q1",
                25.0,
                "USD",
                "0003",
                dt.date(2024, 4, 1),
                dt.date(2024, 1, 1),
                dt.datetime(2024, 4, 1, 12),
                '["Revenues"]',
                "[1]",
                "rule-revenue",
                "single",
                True,
                "run-q1",
                dt.datetime(2024, 4, 1, 12),
                dt.datetime(2024, 4, 1, 12),
            ),
        ],
    )


def _close_store_for_serving(store) -> None:
    store.con.execute("CHECKPOINT")
    store.connection.close()
    store.connection = None


def _request(*, as_of: dt.datetime, vintage: str = "latest") -> RangeRequest:
    return RangeRequest.model_validate(
        {
            "dataset": "ATX.US.FUNDAMENTALS",
            "schema": "standardized",
            "symbols": ["AAPL"],
            "start": "2023-01-01",
            "end": "2024-01-01",
            "as_of": as_of.isoformat(),
            "items": ["revenue"],
            "basis": ["annual"],
            "fields": ["security_id", "period_end", "value", "available_at"],
            "vintage": vintage,
        }
    )


def test_saas_contract_migration_catalogs_public_schemas(tmp_store):
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name FROM duckdb_tables()
            WHERE table_name IN (
                'api_dataset_catalog','api_schema_catalog','api_field_catalog',
                'saas_accounts','saas_api_keys','saas_entitlements',
                'saas_usage_events','saas_batch_jobs','api_unit_price_catalog',
                'saas_idempotency_records'
            )
            """
        ).fetchall()
    }
    assert len(tables) == 10
    catalog = tmp_store.con.execute(
        """
        SELECT dataset_id,schema_code,schema_version
        FROM api_schema_catalog ORDER BY dataset_id,schema_code
        """
    ).fetchall()
    expected = sorted((dataset.code, schema.code, schema.version) for dataset in DATASETS for schema in dataset.schemas)
    assert catalog == expected
    price_rows = tmp_store.con.execute(
        "SELECT dataset_id,schema_code,status FROM api_unit_price_catalog ORDER BY 1,2"
    ).fetchall()
    assert price_rows == sorted(
        (dataset.code, schema.code, "contract_required") for dataset in DATASETS for schema in dataset.schemas
    )
    job_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'saas_batch_jobs'"
        ).fetchall()
    }
    assert {
        "request_sha256",
        "attempt_count",
        "worker_id",
        "lease_expires_at",
        "next_attempt_at",
        "monthly_byte_limit",
    } <= job_columns


def test_range_query_is_point_in_time_revision_correct_and_end_exclusive(tmp_store):
    _seed_fundamental_revisions(tmp_store)
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    service = WarehouseReadService(path)

    before_restatement = service.get_range(_request(as_of=dt.datetime(2024, 2, 15, tzinfo=dt.UTC)))
    after_restatement = service.get_range(_request(as_of=dt.datetime(2024, 4, 15, tzinfo=dt.UTC)))
    first_reported = service.get_range(
        _request(as_of=dt.datetime(2024, 4, 15, tzinfo=dt.UTC), vintage="first_reported")
    )

    assert [row["value"] for row in before_restatement.data] == [100.0]
    assert [row["value"] for row in after_restatement.data] == [110.0]
    assert [row["value"] for row in first_reported.data] == [100.0]
    assert all(row["period_end"] < dt.date(2024, 1, 1) for row in after_restatement.data)


def test_industry_standardized_schema_emits_pit_classification_updates(tmp_store):
    _seed_fundamental_revisions(tmp_store)
    tmp_store.con.execute(
        """
        UPDATE fundamental_standardized
        SET revision_group_id='revenue-2023',
            revision_sequence=CASE standardized_id WHEN 'std-first' THEN 1 ELSE 2 END,
            revision_count=2,
            update_type=CASE standardized_id WHEN 'std-first' THEN 'original' ELSE 'restated' END
        WHERE standardized_id IN ('std-first','std-restated')
        """
    )
    tmp_store.con.executemany(
        """
        INSERT INTO entity_industry_template (
            route_id,source,security_id,symbol,industry_template,matched_taxonomy,
            matched_node_code,match_reason,valid_from,valid_to,as_of_date,available_at,
            is_latest_revision,run_id,source_loaded_at,route_revision_group_id,
            revision_sequence,revision_count,previous_industry_template,update_type,
            knowledge_valid_to
        ) VALUES (?,?,?,?,?,?,?,?,?,NULL,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            (
                "route-bank",
                "test-routes",
                "SEC-CIK-0000320193",
                "AAPL",
                "BK",
                "SIC",
                "6022",
                "fixture_bank",
                dt.date(2020, 1, 1),
                dt.date(2020, 1, 1),
                dt.datetime(2024, 1, 1, 12),
                False,
                "route-test",
                dt.datetime(2024, 1, 1, 12),
                "route-group",
                1,
                2,
                None,
                "original",
                dt.datetime(2024, 4, 1, 12),
            ),
            (
                "route-insurance",
                "test-routes",
                "SEC-CIK-0000320193",
                "AAPL",
                "IS",
                "SIC",
                "6311",
                "fixture_insurance_revision",
                dt.date(2020, 1, 1),
                dt.date(2020, 1, 1),
                dt.datetime(2024, 4, 1, 12),
                True,
                "route-test",
                dt.datetime(2024, 4, 1, 12),
                "route-group",
                2,
                2,
                "BK",
                "restated",
                None,
            ),
        ],
    )
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    service = WarehouseReadService(path)

    def request(as_of: dt.datetime, *, vintage: str = "latest") -> RangeRequest:
        return RangeRequest.model_validate(
            {
                "dataset": "ATX.US.FUNDAMENTALS",
                "schema": "industry-standardized",
                "symbols": ["AAPL"],
                "start": "2023-01-01",
                "end": "2024-01-01",
                "as_of": as_of.isoformat(),
                "items": ["revenue"],
                "basis": ["annual"],
                "fields": [
                    "value",
                    "industry_template",
                    "requirement_level",
                    "update_type",
                    "available_at",
                ],
                "vintage": vintage,
            }
        )

    bank = service.get_range(request(dt.datetime(2024, 2, 15, tzinfo=dt.UTC))).data
    insurance = service.get_range(request(dt.datetime(2024, 4, 15, tzinfo=dt.UTC))).data
    first = service.get_range(
        request(dt.datetime(2024, 4, 15, tzinfo=dt.UTC), vintage="first_reported")
    ).data

    assert [(row["value"], row["industry_template"], row["update_type"]) for row in bank] == [
        (100.0, "BK", "original")
    ]
    assert [
        (row["value"], row["industry_template"], row["requirement_level"], row["update_type"])
        for row in insurance
    ] == [(110.0, "IS", "supplemental", "classification_update")]
    assert [(row["value"], row["industry_template"]) for row in first] == [(100.0, "BK")]


def test_http_api_auth_entitlement_symbology_and_usage(tmp_store):
    _seed_fundamental_revisions(tmp_store)
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    principal = ApiPrincipal(
        account_id="account-test",
        key_id="key-test",
        scopes=frozenset({"data:read"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
    )
    ledger = InMemoryUsageLedger()
    app = create_app(
        database_path=path,
        authenticator=StaticApiKeyAuthenticator({API_KEY: principal}),
        usage_ledger=ledger,
    )
    client = TestClient(app)

    assert client.get("/v1/health").status_code == 200
    unauthorized = client.get("/v1/metadata.list_datasets")
    assert unauthorized.status_code == 401
    assert unauthorized.json()["error"]["code"] == "missing_api_key"

    headers = {"Authorization": f"Bearer {API_KEY}"}
    metadata = client.get("/v1/metadata.list_datasets", headers=headers)
    assert metadata.status_code == 200
    assert [row["dataset"] for row in metadata.json()["data"]] == ["ATX.US.FUNDAMENTALS"]

    forbidden = client.post(
        "/v1/timeseries.get_range",
        headers=headers,
        json={
            "dataset": "ATX.US.EQUITIES",
            "schema": "ohlcv-1d",
            "symbols": ["AAPL"],
            "start": "2023-01-01",
            "end": "2024-01-01",
        },
    )
    assert forbidden.status_code == 403
    assert forbidden.json()["error"]["code"] == "dataset_not_entitled"

    response = client.post(
        "/v1/timeseries.get_range",
        headers=headers,
        json=_request(as_of=dt.datetime(2024, 4, 15, tzinfo=dt.UTC)).model_dump(by_alias=True, mode="json"),
    )
    assert response.status_code == 200
    assert response.headers["X-ATX-Records"] == "1"
    assert response.json()["data"][0]["value"] == 110.0
    assert len(ledger.events()) == 1
    assert ledger.events()[0].record_count == 1

    resolved = client.post(
        "/v1/symbology.resolve",
        headers=headers,
        json={
            "symbols": ["AAPL"],
            "stype_in": "raw_symbol",
            "stype_out": "security_id",
            "start": "2023-01-01",
            "end": "2024-01-01",
            "as_of": "2024-04-15T00:00:00Z",
        },
    )
    assert resolved.status_code == 200
    assert resolved.json()["status"] == 0
    assert resolved.json()["result"]["AAPL"][0]["symbol"] == "SEC-CIK-0000320193"


def test_durable_batch_job_is_pit_pinned_checksummed_and_entitled(tmp_store, tmp_path):
    _seed_fundamental_revisions(tmp_store)
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    upsert_account(
        path,
        account_id="account-durable",
        account_name="Durable Test",
        plan_code="institutional",
    )
    issued = issue_api_key(path, account_id="account-durable")
    grant_entitlement(
        path,
        account_id="account-durable",
        dataset="ATX.US.FUNDAMENTALS",
        schemas=("standardized",),
        max_sync_rows=50_000,
        requests_per_minute=1_000,
    )
    set_unit_price(
        path,
        dataset="ATX.US.FUNDAMENTALS",
        schema="standardized",
        unit_price_per_gb=Decimal("2.50"),
    )
    app = create_app(
        database_path=path,
        control_database_path=path,
        artifact_root=tmp_path / "artifacts",
        process_batches_inline=False,
    )
    client = TestClient(app)
    headers = {"X-API-Key": issued.api_key}

    forbidden_schema = client.get(
        "/v1/metadata.get_schema",
        params={"dataset": "ATX.US.FUNDAMENTALS", "schema": "ratios"},
        headers=headers,
    )
    assert forbidden_schema.status_code == 403

    batch_payload = {
        "request": {
            "dataset": "ATX.US.FUNDAMENTALS",
            "schema": "standardized",
            "symbols": ["AAPL"],
            "start": "2023-01-01",
            "end": "2024-01-01",
            "items": ["revenue"],
            "basis": ["annual"],
            "fields": ["security_id", "period_end", "value", "available_at"],
            "vintage": "latest",
        },
        "encoding": "parquet",
        "compression": "zstd",
    }
    estimate = client.post(
        "/v1/metadata.get_cost",
        headers=headers,
        json=batch_payload["request"],
    )
    assert estimate.status_code == 200
    assert estimate.json()["billable_bytes"] > 0
    assert estimate.json()["unit_price"]["unit_price_per_gb"] == 2.5
    assert estimate.json()["cost_usd"] == float(
        (Decimal(estimate.json()["billable_bytes"]) / Decimal(1_000_000_000) * Decimal("2.50")).quantize(
            Decimal("0.000000001")
        )
    )

    idempotent_headers = {**headers, "Idempotency-Key": "batch-retry-0001"}
    submitted = client.post(
        "/v1/batch.submit_job",
        headers=idempotent_headers,
        json=batch_payload,
    )
    assert submitted.status_code == 202
    job_id = submitted.json()["data"]["job_id"]

    replayed = client.post(
        "/v1/batch.submit_job",
        headers=idempotent_headers,
        json=batch_payload,
    )
    assert replayed.status_code == 202
    assert replayed.headers["X-ATX-Idempotent-Replayed"] == "true"
    assert replayed.json()["data"]["job_id"] == job_id
    conflicting_payload = {**batch_payload, "compression": "gzip"}
    conflict = client.post(
        "/v1/batch.submit_job",
        headers=idempotent_headers,
        json=conflicting_payload,
    )
    assert conflict.status_code == 409
    assert conflict.json()["error"]["code"] == "idempotency_key_conflict"

    worker = build_worker(
        warehouse_path=path,
        control_path=path,
        artifact_root=tmp_path / "artifacts",
        worker_id="test-worker",
    )
    completed = worker.run_once()
    assert completed is not None
    assert completed.job_id == job_id

    status = client.get("/v1/batch.get_job", params={"job_id": job_id}, headers=headers)
    assert status.status_code == 200
    job = status.json()["data"]
    assert job["state"] == "completed"
    assert job["record_count"] == 1
    assert job["attempt_count"] == 1
    assert job["download_url"].endswith(job_id)

    downloaded = client.get("/v1/batch.download", params={"job_id": job_id}, headers=headers)
    assert downloaded.status_code == 200
    assert downloaded.headers["X-ATX-SHA256"] == hashlib.sha256(downloaded.content).hexdigest()
    table = pq.read_table(pa.BufferReader(downloaded.content))
    assert table.to_pylist()[0]["value"] == 110.0

    artifact = next((tmp_path / "artifacts").rglob("data.parquet"))
    manifest = json.loads((artifact.parent / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["request"]["as_of"] is not None
    assert manifest["schema_version"] == "2.0.0"
    assert manifest["sha256"] == job["sha256"]

    with duckdb.connect(str(path), read_only=True) as conn:
        states = conn.execute(
            "SELECT state,attempt_count,worker_id FROM saas_batch_jobs WHERE job_id = ?", [job_id]
        ).fetchone()
        endpoints = {
            row[0]
            for row in conn.execute(
                "SELECT endpoint FROM saas_usage_events WHERE account_id = 'account-durable'"
            ).fetchall()
        }
        last_used_at = conn.execute(
            "SELECT last_used_at FROM saas_api_keys WHERE key_id = ?", [issued.key_id]
        ).fetchone()[0]
        billing = conn.execute(
            """
            SELECT billable_bytes,cost_usd,billing_mode
            FROM saas_usage_events WHERE endpoint = 'batch.generate'
            """
        ).fetchone()
        job_count = conn.execute(
            "SELECT count(*) FROM saas_batch_jobs WHERE account_id = 'account-durable'"
        ).fetchone()[0]
    assert states == ("completed", 1, "test-worker")
    assert billing[0] == estimate.json()["billable_bytes"]
    assert billing[1] is not None
    assert billing[2] == "historical_batch"
    assert job_count == 1
    assert {"batch.submit_job", "batch.generate", "batch.download"} <= endpoints
    assert last_used_at is not None
    assert revoke_api_key(path, issued.key_id)
    assert client.get("/v1/batch.list_jobs", headers=headers).status_code == 401


def test_batch_artifact_contract_supports_all_encodings(tmp_store, tmp_path):
    _seed_fundamental_revisions(tmp_store)
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    service = WarehouseReadService(path)
    principal = ApiPrincipal(
        account_id="account-formats",
        key_id="key-formats",
        scopes=frozenset({"data:read", "batch:read", "batch:write"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
    )

    for encoding, compression in (
        ("parquet", "zstd"),
        ("arrow", "zstd"),
        ("csv", "gzip"),
        ("jsonl", "zstd"),
    ):
        root = tmp_path / encoding
        manager = LocalBatchManager(service, InMemoryBatchJobRepository(), root)
        payload = BatchSubmitRequest.model_validate(
            {
                "request": {
                    "dataset": "ATX.US.FUNDAMENTALS",
                    "schema": "standardized",
                    "symbols": ["AAPL"],
                    "start": "2023-01-01",
                    "end": "2024-01-01",
                    "as_of": "2024-04-15T00:00:00Z",
                    "items": ["revenue"],
                    "basis": ["annual"],
                    "fields": ["security_id", "period_end", "value", "available_at"],
                },
                "encoding": encoding,
                "compression": compression,
            }
        )
        submitted = manager.submit(payload, principal)
        completed = manager.process(submitted.job_id)
        assert completed is not None
        assert completed.state == "completed"
        artifact = manager.artifact_path(completed)
        assert artifact is not None
        assert _read_batch_values(artifact, encoding, compression) == [110.0]


def test_rate_and_monthly_byte_limits_return_explicit_429(tmp_store, tmp_path):
    _seed_fundamental_revisions(tmp_store)
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    payload = _request(as_of=dt.datetime(2024, 4, 15, tzinfo=dt.UTC)).model_dump(by_alias=True, mode="json")
    rate_principal = ApiPrincipal(
        account_id="account-rate",
        key_id="key-rate",
        scopes=frozenset({"data:read"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
        requests_per_minute_by_dataset={"ATX.US.FUNDAMENTALS": 1},
    )
    rate_client = TestClient(
        create_app(
            database_path=path,
            authenticator=StaticApiKeyAuthenticator({API_KEY: rate_principal}),
        )
    )
    headers = {"X-API-Key": API_KEY}
    first = rate_client.post("/v1/timeseries.get_range", headers=headers, json=payload)
    second = rate_client.post("/v1/timeseries.get_range", headers=headers, json=payload)
    assert first.status_code == 200
    assert first.headers["RateLimit-Remaining"] == "0"
    assert second.status_code == 429
    assert second.headers["X-ATX-Rate-Limited-Reason"] == "account-dataset-rate"
    assert second.json()["error"]["code"] == "rate_limit_exceeded"

    ledger = InMemoryUsageLedger()
    now = dt.datetime.now(dt.UTC)
    ledger.record(
        UsageEvent(
            request_id="already-billed",
            account_id="account-quota",
            key_id="key-quota",
            endpoint="batch.generate",
            dataset="ATX.US.FUNDAMENTALS",
            schema="standardized",
            started_at=now,
            finished_at=now,
            status_code=200,
            record_count=1,
            response_bytes=10,
            billable_bytes=10,
            billing_mode="historical_batch",
        )
    )
    quota_principal = ApiPrincipal(
        account_id="account-quota",
        key_id="key-quota",
        scopes=frozenset({"data:read"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
        bytes_per_month_by_dataset={"ATX.US.FUNDAMENTALS": 10},
    )
    quota_client = TestClient(
        create_app(
            database_path=path,
            authenticator=StaticApiKeyAuthenticator({API_KEY: quota_principal}),
            usage_ledger=ledger,
        )
    )
    exhausted = quota_client.post("/v1/timeseries.get_range", headers=headers, json=payload)
    assert exhausted.status_code == 429
    assert exhausted.headers["X-ATX-Monthly-Bytes-Remaining"] == "0"
    assert exhausted.json()["error"]["code"] == "monthly_byte_quota_exceeded"

    batch_principal = ApiPrincipal(
        account_id="account-batch-quota",
        key_id="key-batch-quota",
        scopes=frozenset({"data:read", "batch:write"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
        bytes_per_month_by_dataset={"ATX.US.FUNDAMENTALS": 1},
    )
    batch_manager = LocalBatchManager(
        WarehouseReadService(path),
        InMemoryBatchJobRepository(),
        tmp_path / "quota-artifacts",
        usage_ledger=InMemoryUsageLedger(),
    )
    batch_job = batch_manager.submit(
        BatchSubmitRequest.model_validate({"request": payload}),
        batch_principal,
    )
    failed = batch_manager.process(batch_job.job_id)
    assert failed is not None
    assert failed.state == "failed"
    assert failed.error_code == "monthly_byte_quota_exceeded"


def test_expired_batch_lease_is_recovered_by_another_worker(tmp_store, tmp_path):
    _seed_fundamental_revisions(tmp_store)
    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    repository = InMemoryBatchJobRepository()
    manager = LocalBatchManager(WarehouseReadService(path), repository, tmp_path / "lease-artifacts")
    principal = ApiPrincipal(
        account_id="account-lease",
        key_id="key-lease",
        scopes=frozenset({"data:read", "batch:write"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
    )
    payload = BatchSubmitRequest.model_validate(
        {
            "request": {
                "dataset": "ATX.US.FUNDAMENTALS",
                "schema": "standardized",
                "symbols": ["AAPL"],
                "start": "2023-01-01",
                "end": "2024-01-01",
                "as_of": "2024-04-15T00:00:00Z",
                "items": ["revenue"],
                "fields": ["value"],
            }
        }
    )
    submitted = manager.submit(payload, principal)
    abandoned = repository.claim(submitted.job_id, worker_id="crashed-worker", lease_seconds=300)
    assert abandoned is not None
    repository.update(
        submitted.job_id,
        lease_expires_at=dt.datetime.now(dt.UTC) - dt.timedelta(seconds=1),
    )

    recovered = BatchWorker(manager, worker_id="recovery-worker").run_once()
    assert recovered is not None
    assert recovered.state == "completed"
    assert recovered.attempt_count == 2
    assert recovered.worker_id == "recovery-worker"


def _read_batch_values(path, encoding: str, compression: str) -> list[float]:
    if encoding == "parquet":
        return pq.read_table(path).column("value").to_pylist()
    input_compression = None if compression == "none" else compression
    with pa.input_stream(str(path), compression=input_compression) as stream:
        if encoding == "arrow":
            table = pa_ipc.open_stream(stream).read_all()
            return table.column("value").to_pylist()
        if encoding == "csv":
            table = pa_csv.read_csv(stream)
            return table.column("value").to_pylist()
        return [json.loads(line)["value"] for line in stream.read().decode("utf-8").splitlines()]
