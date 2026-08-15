"""Asynchronous batch-job state and immutable columnar artifact delivery."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import threading
import time
import uuid
from collections.abc import Callable, Iterator
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, ClassVar, Protocol, cast

import pyarrow as pa  # type: ignore[import-untyped]
import pyarrow.csv as pa_csv  # type: ignore[import-untyped]
import pyarrow.ipc as pa_ipc  # type: ignore[import-untyped]
import pyarrow.parquet as pq  # type: ignore[import-untyped]

from ..connection import open_duckdb_connection
from .auth import ApiPrincipal
from .catalog import _record_schema_sha256, get_schema, public_schema
from .commercial import PricingCatalog, calculate_cost, month_start
from .models import BatchCompression, BatchEncoding, BatchRangeRequest, BatchSubmitRequest
from .service import WarehouseReadService
from .usage import UsageEvent, UsageLedger


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC)


def _aware_utc(value: dt.datetime) -> dt.datetime:
    return value.replace(tzinfo=dt.UTC) if value.tzinfo is None else value.astimezone(dt.UTC)


def _optional_aware_utc(value: dt.datetime | None) -> dt.datetime | None:
    return None if value is None else _aware_utc(value)


def _json_default(value: object) -> str:
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return str(value)


class BatchQuotaExceeded(RuntimeError):
    pass


@dataclass(frozen=True)
class BatchJob:
    job_id: str
    account_id: str
    key_id: str
    request: BatchRangeRequest
    encoding: BatchEncoding
    compression: BatchCompression
    request_sha256: str
    query_sha256: str
    schema_version: str
    schema_sha256: str
    idempotency_key: str | None
    state: str
    attempt_count: int
    worker_id: str | None
    lease_expires_at: dt.datetime | None
    next_attempt_at: dt.datetime | None
    monthly_byte_limit: int | None
    record_count: int
    billed_bytes: int
    package_bytes: int
    result_uri: str | None
    result_sha256: str | None
    logical_content_sha256: str | None
    manifest_uri: str | None
    manifest_sha256: str | None
    error_code: str | None
    error_message: str | None
    received_at: dt.datetime
    queued_at: dt.datetime | None
    processing_started_at: dt.datetime | None
    completed_at: dt.datetime | None
    expires_at: dt.datetime
    updated_at: dt.datetime

    def public(self) -> dict[str, Any]:
        return {
            "job_id": self.job_id,
            "dataset": self.request.dataset,
            "schema": self.request.schema_name,
            "schema_version": self.schema_version,
            "schema_sha256": self.schema_sha256,
            "request_sha256": self.request_sha256,
            "query_sha256": self.query_sha256,
            "encoding": self.encoding,
            "compression": self.compression,
            "state": self.state,
            "attempt_count": self.attempt_count,
            "record_count": self.record_count,
            "billed_bytes": self.billed_bytes,
            "package_bytes": self.package_bytes,
            "sha256": self.result_sha256,
            "logical_content_sha256": self.logical_content_sha256,
            "manifest_sha256": self.manifest_sha256,
            "received_at": self.received_at,
            "queued_at": self.queued_at,
            "processing_started_at": self.processing_started_at,
            "completed_at": self.completed_at,
            "expires_at": self.expires_at,
            "error": (
                {"code": self.error_code, "message": self.error_message} if self.error_code is not None else None
            ),
        }


class BatchJobRepository(Protocol):
    def add(self, job: BatchJob) -> None: ...

    def get(self, job_id: str, *, account_id: str | None = None) -> BatchJob | None: ...

    def list(self, account_id: str, *, limit: int) -> list[BatchJob]: ...

    def claim(self, job_id: str, *, worker_id: str, lease_seconds: int) -> BatchJob | None: ...

    def claim_next(self, *, worker_id: str, lease_seconds: int) -> BatchJob | None: ...

    def renew_lease(self, job_id: str, *, worker_id: str, lease_seconds: int) -> bool: ...

    def finish(self, job_id: str, *, worker_id: str, **changes: object) -> BatchJob | None: ...

    def update(self, job_id: str, **changes: object) -> BatchJob: ...


class InMemoryBatchJobRepository:
    def __init__(self) -> None:
        self._jobs: dict[str, BatchJob] = {}
        self._lock = threading.Lock()

    def add(self, job: BatchJob) -> None:
        with self._lock:
            if job.job_id in self._jobs:
                raise ValueError(f"duplicate batch job {job.job_id}")
            self._jobs[job.job_id] = job

    def get(self, job_id: str, *, account_id: str | None = None) -> BatchJob | None:
        with self._lock:
            job = self._jobs.get(job_id)
            return job if job is not None and (account_id is None or job.account_id == account_id) else None

    def list(self, account_id: str, *, limit: int) -> list[BatchJob]:
        with self._lock:
            jobs = [job for job in self._jobs.values() if job.account_id == account_id]
            return sorted(jobs, key=lambda job: job.received_at, reverse=True)[:limit]

    def claim(self, job_id: str, *, worker_id: str, lease_seconds: int) -> BatchJob | None:
        with self._lock:
            job = self._jobs.get(job_id)
            now = _utc_now()
            eligible = job is not None and (
                job.state == "queued"
                or (job.state == "processing" and job.lease_expires_at is not None and job.lease_expires_at <= now)
            )
            if not eligible:
                return None
            assert job is not None
            claimed = replace(
                job,
                state="processing",
                attempt_count=job.attempt_count + 1,
                worker_id=worker_id,
                lease_expires_at=now + dt.timedelta(seconds=lease_seconds),
                processing_started_at=job.processing_started_at or now,
                updated_at=now,
            )
            self._jobs[job_id] = claimed
            return claimed

    def claim_next(self, *, worker_id: str, lease_seconds: int) -> BatchJob | None:
        with self._lock:
            now = _utc_now()
            candidates = sorted(self._jobs.values(), key=lambda job: job.received_at)
            job = next(
                (
                    candidate
                    for candidate in candidates
                    if (candidate.next_attempt_at is None or candidate.next_attempt_at <= now)
                    and (
                        candidate.state == "queued"
                        or (
                            candidate.state == "processing"
                            and candidate.lease_expires_at is not None
                            and candidate.lease_expires_at <= now
                        )
                    )
                ),
                None,
            )
            if job is None:
                return None
            claimed = replace(
                job,
                state="processing",
                attempt_count=job.attempt_count + 1,
                worker_id=worker_id,
                lease_expires_at=now + dt.timedelta(seconds=lease_seconds),
                processing_started_at=job.processing_started_at or now,
                updated_at=now,
            )
            self._jobs[job.job_id] = claimed
            return claimed

    def renew_lease(self, job_id: str, *, worker_id: str, lease_seconds: int) -> bool:
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None or job.state != "processing" or job.worker_id != worker_id:
                return False
            self._jobs[job_id] = replace(
                job,
                lease_expires_at=_utc_now() + dt.timedelta(seconds=lease_seconds),
                updated_at=_utc_now(),
            )
            return True

    def finish(self, job_id: str, *, worker_id: str, **changes: object) -> BatchJob | None:
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None or job.state != "processing" or job.worker_id != worker_id:
                return None
            updated = replace(job, **cast(Any, changes), updated_at=_utc_now())
            self._jobs[job_id] = updated
            return updated

    def update(self, job_id: str, **changes: object) -> BatchJob:
        with self._lock:
            updated = replace(self._jobs[job_id], **cast(Any, changes), updated_at=_utc_now())
            self._jobs[job_id] = updated
            return updated


_JOB_COLUMN_NAMES = (
    "job_id",
    "account_id",
    "key_id",
    "dataset_id",
    "schema_code",
    "request_json",
    "encoding",
    "compression",
    "request_sha256",
    "idempotency_key",
    "state",
    "attempt_count",
    "worker_id",
    "lease_expires_at",
    "next_attempt_at",
    "monthly_byte_limit",
    "record_count",
    "billed_bytes",
    "package_bytes",
    "result_uri",
    "result_sha256",
    "logical_content_sha256",
    "manifest_uri",
    "manifest_sha256",
    "schema_version",
    "schema_sha256",
    "query_sha256",
    "error_code",
    "error_message",
    "received_at",
    "queued_at",
    "processing_started_at",
    "completed_at",
    "expires_at",
    "updated_at",
)
_JOB_COLUMNS = ",".join(_JOB_COLUMN_NAMES)


class DuckDBBatchJobRepository:
    """Durable local job repository backed by ``saas_batch_jobs``."""

    _UPDATABLE: ClassVar[set[str]] = {
        "state",
        "record_count",
        "billed_bytes",
        "package_bytes",
        "result_uri",
        "result_sha256",
        "logical_content_sha256",
        "manifest_uri",
        "manifest_sha256",
        "error_code",
        "error_message",
        "processing_started_at",
        "completed_at",
        "expires_at",
        "attempt_count",
        "worker_id",
        "lease_expires_at",
        "next_attempt_at",
    }

    def __init__(self, database_path: Path | str) -> None:
        self.database_path = Path(database_path)
        self._lock = threading.Lock()

    def add(self, job: BatchJob) -> None:
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            conn.execute(
                f"INSERT INTO saas_batch_jobs ({_JOB_COLUMNS}) VALUES ({','.join('?' for _ in _JOB_COLUMN_NAMES)})",
                _job_values(job),
            )

    def get(self, job_id: str, *, account_id: str | None = None) -> BatchJob | None:
        where = "job_id = ?" if account_id is None else "job_id = ? AND account_id = ?"
        params = [job_id] if account_id is None else [job_id, account_id]
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            row = conn.execute(f"SELECT {_JOB_COLUMNS} FROM saas_batch_jobs WHERE {where}", params).fetchone()
        return _job_from_row(row) if row is not None else None

    def list(self, account_id: str, *, limit: int) -> list[BatchJob]:
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            rows = conn.execute(
                f"SELECT {_JOB_COLUMNS} FROM saas_batch_jobs WHERE account_id = ? ORDER BY received_at DESC LIMIT ?",
                [account_id, limit],
            ).fetchall()
        return [_job_from_row(row) for row in rows]

    def claim(self, job_id: str, *, worker_id: str, lease_seconds: int) -> BatchJob | None:
        now = _utc_now()
        lease_expires_at = now + dt.timedelta(seconds=lease_seconds)
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            row = conn.execute(
                """
                UPDATE saas_batch_jobs
                SET state = 'processing', attempt_count = attempt_count + 1,
                    worker_id = ?, lease_expires_at = ?,
                    processing_started_at = coalesce(processing_started_at, ?), updated_at = ?
                WHERE job_id = ?
                  AND (next_attempt_at IS NULL OR next_attempt_at <= ?)
                  AND (state = 'queued' OR (state = 'processing' AND lease_expires_at <= ?))
                RETURNING job_id
                """,
                [worker_id, lease_expires_at, now, now, job_id, now, now],
            ).fetchone()
            if row is None:
                return None
            result = conn.execute(f"SELECT {_JOB_COLUMNS} FROM saas_batch_jobs WHERE job_id = ?", [job_id]).fetchone()
        assert result is not None
        return _job_from_row(result)

    def claim_next(self, *, worker_id: str, lease_seconds: int) -> BatchJob | None:
        now = _utc_now()
        lease_expires_at = now + dt.timedelta(seconds=lease_seconds)
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            row = conn.execute(
                """
                UPDATE saas_batch_jobs
                SET state = 'processing', attempt_count = attempt_count + 1,
                    worker_id = ?, lease_expires_at = ?,
                    processing_started_at = coalesce(processing_started_at, ?), updated_at = ?
                WHERE job_id = (
                    SELECT job_id FROM saas_batch_jobs
                    WHERE (next_attempt_at IS NULL OR next_attempt_at <= ?)
                      AND (state = 'queued' OR (state = 'processing' AND lease_expires_at <= ?))
                    ORDER BY received_at,job_id LIMIT 1
                )
                RETURNING job_id
                """,
                [worker_id, lease_expires_at, now, now, now, now],
            ).fetchone()
            if row is None:
                return None
            result = conn.execute(f"SELECT {_JOB_COLUMNS} FROM saas_batch_jobs WHERE job_id = ?", [row[0]]).fetchone()
        assert result is not None
        return _job_from_row(result)

    def renew_lease(self, job_id: str, *, worker_id: str, lease_seconds: int) -> bool:
        now = _utc_now()
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            row = conn.execute(
                """
                UPDATE saas_batch_jobs
                SET lease_expires_at = ?, updated_at = ?
                WHERE job_id = ? AND state = 'processing' AND worker_id = ?
                RETURNING job_id
                """,
                [now + dt.timedelta(seconds=lease_seconds), now, job_id, worker_id],
            ).fetchone()
        return row is not None

    def finish(self, job_id: str, *, worker_id: str, **changes: object) -> BatchJob | None:
        unknown = set(changes) - self._UPDATABLE
        if unknown:
            raise ValueError(f"unsupported batch job fields: {sorted(unknown)}")
        now = _utc_now()
        assignments = [f"{name} = ?" for name in changes]
        params = [*changes.values(), now, job_id, worker_id]
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            row = conn.execute(
                f"""
                UPDATE saas_batch_jobs
                SET {", ".join(assignments)}, updated_at = ?
                WHERE job_id = ? AND state = 'processing' AND worker_id = ?
                RETURNING {_JOB_COLUMNS}
                """,
                params,
            ).fetchone()
        return None if row is None else _job_from_row(row)

    def update(self, job_id: str, **changes: object) -> BatchJob:
        unknown = set(changes) - self._UPDATABLE
        if unknown:
            raise ValueError(f"unsupported batch job fields: {sorted(unknown)}")
        now = _utc_now()
        assignments = [f"{name} = ?" for name in changes]
        params = [*changes.values(), now, job_id]
        with self._lock, open_duckdb_connection(self.database_path) as conn:
            conn.execute(
                f"UPDATE saas_batch_jobs SET {', '.join(assignments)}, updated_at = ? WHERE job_id = ?",
                params,
            )
            row = conn.execute(f"SELECT {_JOB_COLUMNS} FROM saas_batch_jobs WHERE job_id = ?", [job_id]).fetchone()
        if row is None:
            raise KeyError(job_id)
        return _job_from_row(row)


def _job_values(job: BatchJob) -> list[object]:
    return [
        job.job_id,
        job.account_id,
        job.key_id,
        job.request.dataset,
        job.request.schema_name,
        job.request.model_dump_json(by_alias=True),
        job.encoding,
        job.compression,
        job.request_sha256,
        job.idempotency_key,
        job.state,
        job.attempt_count,
        job.worker_id,
        job.lease_expires_at,
        job.next_attempt_at,
        job.monthly_byte_limit,
        job.record_count,
        job.billed_bytes,
        job.package_bytes,
        job.result_uri,
        job.result_sha256,
        job.logical_content_sha256,
        job.manifest_uri,
        job.manifest_sha256,
        job.schema_version,
        job.schema_sha256,
        job.query_sha256,
        job.error_code,
        job.error_message,
        job.received_at,
        job.queued_at,
        job.processing_started_at,
        job.completed_at,
        job.expires_at,
        job.updated_at,
    ]


def _job_from_row(row: tuple[Any, ...]) -> BatchJob:
    return BatchJob(
        job_id=str(row[0]),
        account_id=str(row[1]),
        key_id=str(row[2]),
        request=BatchRangeRequest.model_validate_json(str(row[5])),
        encoding=str(row[6]),  # type: ignore[arg-type]
        compression=str(row[7]),  # type: ignore[arg-type]
        request_sha256=str(row[8]),
        idempotency_key=None if row[9] is None else str(row[9]),
        state=str(row[10]),
        attempt_count=int(row[11]),
        worker_id=None if row[12] is None else str(row[12]),
        lease_expires_at=_optional_aware_utc(row[13]),
        next_attempt_at=_optional_aware_utc(row[14]),
        monthly_byte_limit=None if row[15] is None else int(row[15]),
        record_count=int(row[16]),
        billed_bytes=int(row[17]),
        package_bytes=int(row[18]),
        result_uri=None if row[19] is None else str(row[19]),
        result_sha256=None if row[20] is None else str(row[20]),
        logical_content_sha256=None if row[21] is None else str(row[21]),
        manifest_uri=None if row[22] is None else str(row[22]),
        manifest_sha256=None if row[23] is None else str(row[23]),
        schema_version=str(row[24]),
        schema_sha256=str(row[25]),
        query_sha256=str(row[26]),
        error_code=None if row[27] is None else str(row[27]),
        error_message=None if row[28] is None else str(row[28]),
        received_at=_aware_utc(row[29]),
        queued_at=_optional_aware_utc(row[30]),
        processing_started_at=_optional_aware_utc(row[31]),
        completed_at=_optional_aware_utc(row[32]),
        expires_at=_aware_utc(row[33]),
        updated_at=_aware_utc(row[34]),
    )


class LocalBatchManager:
    """Queue and materialize reproducible local batch artifacts."""

    def __init__(
        self,
        service: WarehouseReadService,
        repository: BatchJobRepository,
        artifact_root: Path | str,
        *,
        usage_ledger: UsageLedger | None = None,
        pricing_catalog: PricingCatalog | None = None,
        api_version: str = "1.0.0",
    ) -> None:
        self.service = service
        self.repository = repository
        self.artifact_root = Path(artifact_root).resolve()
        self.usage_ledger = usage_ledger
        self.pricing_catalog = pricing_catalog
        self.api_version = api_version

    def submit(
        self,
        payload: BatchSubmitRequest,
        principal: ApiPrincipal,
        *,
        idempotency_key: str | None = None,
        request_sha256: str | None = None,
    ) -> BatchJob:
        now = _utc_now()
        request = payload.request
        if request.as_of is None:
            request = request.model_copy(update={"as_of": now})
        normalized_payload = payload.model_copy(update={"request": request})
        schema = get_schema(request.dataset, request.schema_name)
        schema_sha256 = _record_schema_sha256(schema)
        query_sha256 = hashlib.sha256(
            request.model_dump_json(by_alias=True).encode("utf-8")
        ).hexdigest()
        digest = (
            request_sha256 or hashlib.sha256(normalized_payload.model_dump_json(by_alias=True).encode()).hexdigest()
        )
        job = BatchJob(
            job_id=str(uuid.uuid4()),
            account_id=principal.account_id,
            key_id=principal.key_id,
            request=request,
            encoding=payload.encoding,
            compression=payload.compression,
            request_sha256=digest,
            query_sha256=query_sha256,
            schema_version=schema.version,
            schema_sha256=schema_sha256,
            idempotency_key=idempotency_key,
            state="queued",
            attempt_count=0,
            worker_id=None,
            lease_expires_at=None,
            next_attempt_at=None,
            monthly_byte_limit=principal.bytes_per_month(request.dataset),
            record_count=0,
            billed_bytes=0,
            package_bytes=0,
            result_uri=None,
            result_sha256=None,
            logical_content_sha256=None,
            manifest_uri=None,
            manifest_sha256=None,
            error_code=None,
            error_message=None,
            received_at=now,
            queued_at=now,
            processing_started_at=None,
            completed_at=None,
            expires_at=now + dt.timedelta(hours=payload.expires_in_hours),
            updated_at=now,
        )
        self.repository.add(job)
        return job

    def process(
        self,
        job_id: str,
        *,
        worker_id: str = "inline",
        lease_seconds: int = 300,
    ) -> BatchJob | None:
        job = self.repository.claim(job_id, worker_id=worker_id, lease_seconds=lease_seconds)
        if job is None:
            return None
        return self._process_claimed(job, lease_seconds=lease_seconds)

    def process_next(self, *, worker_id: str, lease_seconds: int = 300) -> BatchJob | None:
        job = self.repository.claim_next(worker_id=worker_id, lease_seconds=lease_seconds)
        if job is None:
            return None
        return self._process_claimed(job, lease_seconds=lease_seconds)

    def _process_claimed(self, job: BatchJob, *, lease_seconds: int) -> BatchJob:
        started_at = _utc_now()
        temporary: Path | None = None
        worker_id = job.worker_id
        assert worker_id is not None
        last_heartbeat = time.monotonic()

        def heartbeat(*, force: bool = False) -> bool:
            nonlocal last_heartbeat
            now = time.monotonic()
            if not force and now - last_heartbeat < max(1.0, lease_seconds / 3):
                return True
            renewed = self.repository.renew_lease(
                job.job_id,
                worker_id=worker_id,
                lease_seconds=lease_seconds,
            )
            if renewed:
                last_heartbeat = now
            return renewed

        try:
            active_schema = get_schema(job.request.dataset, job.request.schema_name)
            active_schema_sha256 = _record_schema_sha256(active_schema)
            if (
                active_schema.version != job.schema_version
                or active_schema_sha256 != job.schema_sha256
            ):
                raise RuntimeError(
                    "batch schema contract changed after submission; submit a new job"
                )
            remaining_bytes: int | None = None
            if job.monthly_byte_limit is not None and self.usage_ledger is not None:
                used = self.usage_ledger.billable_bytes_since(
                    job.account_id,
                    job.request.dataset,
                    month_start(started_at),
                )
                remaining_bytes = max(0, job.monthly_byte_limit - used)
                if remaining_bytes == 0:
                    raise BatchQuotaExceeded("monthly dataset byte quota is exhausted")
            worker_partition = hashlib.sha256(worker_id.encode()).hexdigest()[:12]
            directory = self._job_directory(job) / f"attempt-{job.attempt_count}-{worker_partition}"
            directory.mkdir(parents=True, exist_ok=True)
            filename = _artifact_filename(job.encoding, job.compression)
            destination = directory / filename
            temporary = directory / f".{filename}.{uuid.uuid4().hex}.tmp"
            metadata, record_count, billed_bytes, logical_content_sha256 = self._write(
                job,
                temporary,
                heartbeat=heartbeat,
                maximum_billable_bytes=remaining_bytes,
            )
            if not heartbeat(force=True):
                raise RuntimeError("batch worker lease was lost before artifact publication")
            os.replace(temporary, destination)
            digest = _sha256(destination)
            package_bytes = destination.stat().st_size
            manifest = {
                "job_id": job.job_id,
                "api_version": self.api_version,
                "schema_version": job.schema_version,
                "schema_sha256": job.schema_sha256,
                "schema_definition": public_schema(active_schema),
                "request_sha256": job.request_sha256,
                "query_sha256": job.query_sha256,
                "request": job.request.model_dump(by_alias=True, mode="json"),
                "encoding": job.encoding,
                "compression": job.compression,
                "metadata": metadata,
                "record_count": record_count,
                "billed_bytes": billed_bytes,
                "package_bytes": package_bytes,
                "sha256": digest,
                "logical_content_sha256": logical_content_sha256,
                "logical_content_hash_algorithm": "atx-arrow-record-batch-v1",
                "created_at": _utc_now(),
                "expires_at": job.expires_at,
            }
            manifest_path = directory / "manifest.json"
            _write_json_atomic(manifest_path, manifest)
            manifest_sha256 = _sha256(manifest_path)
            completed_at = _utc_now()
            relative = destination.relative_to(self.artifact_root).as_posix()
            manifest_relative = manifest_path.relative_to(self.artifact_root).as_posix()
            completed = self.repository.finish(
                job.job_id,
                worker_id=worker_id,
                state="completed",
                record_count=record_count,
                billed_bytes=billed_bytes,
                package_bytes=package_bytes,
                result_uri=relative,
                result_sha256=digest,
                logical_content_sha256=logical_content_sha256,
                manifest_uri=manifest_relative,
                manifest_sha256=manifest_sha256,
                completed_at=completed_at,
                lease_expires_at=None,
            )
            if completed is None:
                raise RuntimeError("batch worker lost ownership while finalizing the job")
            if self.usage_ledger is not None:
                price = (
                    None
                    if self.pricing_catalog is None
                    else self.pricing_catalog.get(job.request.dataset, job.request.schema_name)
                )
                self.usage_ledger.record(
                    UsageEvent(
                        request_id=f"batch:{job.job_id}",
                        account_id=job.account_id,
                        key_id=job.key_id,
                        endpoint="batch.generate",
                        dataset=job.request.dataset,
                        schema=job.request.schema_name,
                        started_at=started_at,
                        finished_at=completed_at,
                        status_code=200,
                        record_count=record_count,
                        response_bytes=billed_bytes,
                        billable_bytes=billed_bytes,
                        cost_usd=calculate_cost(billed_bytes, price),
                        billing_mode="historical_batch",
                    )
                )
            return completed
        except Exception as exc:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            failed = self.repository.finish(
                job.job_id,
                worker_id=worker_id,
                state="failed",
                error_code=(
                    "monthly_byte_quota_exceeded" if isinstance(exc, BatchQuotaExceeded) else "batch_generation_failed"
                ),
                error_message=str(exc)[:2_000],
                completed_at=_utc_now(),
                lease_expires_at=None,
            )
            current = self.repository.get(job.job_id)
            return failed or current or job

    def get(self, job_id: str, account_id: str) -> BatchJob | None:
        return self.repository.get(job_id, account_id=account_id)

    def list(self, account_id: str, *, limit: int = 100) -> list[BatchJob]:
        return self.repository.list(account_id, limit=limit)

    def cancel(self, job_id: str, account_id: str) -> BatchJob | None:
        job = self.get(job_id, account_id)
        if job is None:
            return None
        if job.state != "queued":
            return job
        return self.repository.update(job_id, state="cancelled", completed_at=_utc_now())

    def artifact_path(self, job: BatchJob) -> Path | None:
        if job.state != "completed" or job.result_uri is None or self.is_expired(job):
            return None
        candidate = (self.artifact_root / job.result_uri).resolve()
        try:
            candidate.relative_to(self.artifact_root)
        except ValueError:
            return None
        return candidate if candidate.is_file() else None

    def manifest_path(self, job: BatchJob) -> Path | None:
        # Artifact expiry limits paid data delivery, not the customer's audit
        # trail. Keep a completed job's small manifest addressable while its
        # account-scoped control-plane record and manifest file are retained.
        if job.state != "completed" or job.manifest_uri is None:
            return None
        candidate = (self.artifact_root / job.manifest_uri).resolve()
        try:
            candidate.relative_to(self.artifact_root)
        except ValueError:
            return None
        if not candidate.is_file():
            return None
        if job.manifest_sha256 is None or _sha256(candidate) != job.manifest_sha256:
            return None
        return candidate

    @staticmethod
    def is_expired(job: BatchJob) -> bool:
        return _aware_utc(job.expires_at) <= _utc_now()

    def _job_directory(self, job: BatchJob) -> Path:
        account_partition = hashlib.sha256(job.account_id.encode("utf-8")).hexdigest()[:24]
        return self.artifact_root / account_partition / job.job_id

    def _write(
        self,
        job: BatchJob,
        path: Path,
        *,
        heartbeat: Callable[..., bool],
        maximum_billable_bytes: int | None,
    ) -> tuple[dict[str, Any], int, int, str]:
        with self.service.stream_range(job.request) as stream:
            schema = stream.reader.schema
            billed_bytes = 0
            logical_digest = hashlib.sha256()
            logical_digest.update(b"atx-arrow-record-batch-v1\0")
            logical_digest.update(job.schema_sha256.encode("ascii"))

            def batches() -> Iterator[pa.RecordBatch]:
                nonlocal billed_bytes
                for batch in stream.batches():
                    if not heartbeat():
                        raise RuntimeError("batch worker lease was lost during artifact generation")
                    if maximum_billable_bytes is not None and billed_bytes + batch.nbytes > maximum_billable_bytes:
                        raise BatchQuotaExceeded("batch would exceed the remaining monthly byte quota")
                    billed_bytes += batch.nbytes
                    serialized = batch.serialize().to_pybytes()
                    logical_digest.update(len(serialized).to_bytes(8, "big"))
                    logical_digest.update(serialized)
                    yield batch

            if job.encoding == "parquet":
                compression = None if job.compression == "none" else job.compression
                with pq.ParquetWriter(path, schema, compression=compression) as writer:
                    for batch in batches():
                        writer.write_batch(batch)
            else:
                output_compression = None if job.compression == "none" else job.compression
                with pa.output_stream(str(path), compression=output_compression) as sink:
                    if job.encoding == "arrow":
                        with pa_ipc.new_stream(sink, schema) as writer:
                            for batch in batches():
                                writer.write_batch(batch)
                    elif job.encoding == "csv":
                        with pa_csv.CSVWriter(sink, schema) as writer:
                            for batch in batches():
                                writer.write_batch(batch)
                    else:
                        for batch in batches():
                            for row in batch.to_pylist():
                                payload = json.dumps(
                                    row,
                                    default=_json_default,
                                    separators=(",", ":"),
                                    sort_keys=True,
                                )
                                sink.write(payload.encode("utf-8") + b"\n")
            return stream.metadata, stream.record_count, billed_bytes, logical_digest.hexdigest()


def _artifact_filename(encoding: BatchEncoding, compression: BatchCompression) -> str:
    extension = {"parquet": "parquet", "arrow": "arrow", "csv": "csv", "jsonl": "jsonl"}[encoding]
    if encoding == "parquet" or compression == "none":
        return f"data.{extension}"
    suffix = "gz" if compression == "gzip" else "zst"
    return f"data.{extension}.{suffix}"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    temporary.write_text(
        json.dumps(payload, default=_json_default, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)
