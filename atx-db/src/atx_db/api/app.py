"""FastAPI adapter for the versioned ATX historical data service."""

from __future__ import annotations

import datetime as dt
import hashlib
import os
import uuid
from collections.abc import Awaitable, Callable, Mapping
from decimal import Decimal
from pathlib import Path
from typing import Annotated

from fastapi import BackgroundTasks, Depends, FastAPI, Header, HTTPException, Query, Request, Response
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse

from ..connection import DEFAULT_DB_PATH, resolve_data_dir
from .auth import ApiPrincipal, Authenticator, DuckDBApiKeyAuthenticator, StaticApiKeyAuthenticator
from .batch import BatchJob, DuckDBBatchJobRepository, InMemoryBatchJobRepository, LocalBatchManager
from .commercial import (
    DuckDBIdempotencyStore,
    DuckDBPricingCatalog,
    IdempotencyConflict,
    IdempotencyInProgress,
    IdempotencyStore,
    InMemoryIdempotencyStore,
    InMemoryPricingCatalog,
    InMemoryRateLimiter,
    PricingCatalog,
    RateLimiter,
    UnitPrice,
    calculate_cost,
    month_start,
)
from .models import BatchRangeRequest, BatchSubmitRequest, RangeRequest, SymbologyRequest
from .service import ApiQueryError, RangeEstimate, WarehouseReadService
from .usage import DuckDBUsageLedger, InMemoryUsageLedger, UsageEvent, UsageLedger

API_VERSION = "1.0.0"
CONTROL_PATH_ENV = "ATX_DB_CONTROL_PATH"
ARTIFACT_ROOT_ENV = "ATX_DB_BATCH_ARTIFACT_DIR"
INLINE_BATCH_ENV = "ATX_DB_BATCH_INLINE"


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC)


def create_app(
    *,
    database_path: Path | str = DEFAULT_DB_PATH,
    control_database_path: Path | str | None = None,
    artifact_root: Path | str | None = None,
    authenticator: Authenticator | None = None,
    usage_ledger: UsageLedger | None = None,
    batch_manager: LocalBatchManager | None = None,
    pricing_catalog: PricingCatalog | None = None,
    rate_limiter: RateLimiter | None = None,
    idempotency_store: IdempotencyStore | None = None,
    process_batches_inline: bool | None = None,
) -> FastAPI:
    configured_control = control_database_path or os.environ.get(CONTROL_PATH_ENV)
    control_path = Path(configured_control) if configured_control is not None else None
    configured_artifacts = artifact_root or os.environ.get(ARTIFACT_ROOT_ENV)
    inline_batches = (
        os.environ.get(INLINE_BATCH_ENV, "true").strip().lower() in {"1", "true", "yes", "on"}
        if process_batches_inline is None
        else process_batches_inline
    )
    auth = authenticator or (
        DuckDBApiKeyAuthenticator(control_path)
        if control_path is not None
        else StaticApiKeyAuthenticator.from_environment()
    )
    ledger = usage_ledger or (DuckDBUsageLedger(control_path) if control_path is not None else InMemoryUsageLedger())
    pricing = pricing_catalog or (
        DuckDBPricingCatalog(control_path) if control_path is not None else InMemoryPricingCatalog()
    )
    limiter = rate_limiter or InMemoryRateLimiter()
    idempotency = idempotency_store or (
        DuckDBIdempotencyStore(control_path) if control_path is not None else InMemoryIdempotencyStore()
    )
    service = WarehouseReadService(database_path)
    jobs = batch_manager or LocalBatchManager(
        service,
        DuckDBBatchJobRepository(control_path) if control_path is not None else InMemoryBatchJobRepository(),
        configured_artifacts or (resolve_data_dir() / "api-batch-artifacts"),
        usage_ledger=ledger,
        pricing_catalog=pricing,
        api_version=API_VERSION,
    )
    app = FastAPI(
        title="ATX Historical Data API",
        version=API_VERSION,
        description="Point-in-time US equity fundamentals and market data.",
    )
    app.state.service = service
    app.state.usage_ledger = ledger
    app.state.authenticator = auth
    app.state.batch_manager = jobs
    app.state.pricing_catalog = pricing
    app.state.rate_limiter = limiter
    app.state.idempotency_store = idempotency

    @app.middleware("http")
    async def request_identity(request: Request, call_next: Callable[[Request], Awaitable[Response]]) -> Response:
        request_id = request.headers.get("x-request-id") or str(uuid.uuid4())
        request.state.request_id = request_id
        response = await call_next(request)
        response.headers["X-ATX-Request-ID"] = request_id
        response.headers["X-ATX-API-Version"] = API_VERSION
        return response

    @app.exception_handler(ApiQueryError)
    async def query_error(request: Request, exc: ApiQueryError) -> JSONResponse:
        return _error_response(request, 400, exc.code, str(exc))

    @app.exception_handler(HTTPException)
    async def http_error(request: Request, exc: HTTPException) -> JSONResponse:
        detail: object = exc.detail
        if isinstance(detail, dict):
            code = str(detail.get("code", "http_error"))
            message = str(detail.get("message", "request failed"))
        else:
            code = "http_error"
            message = str(detail)
        return _error_response(request, exc.status_code, code, message, headers=exc.headers)

    @app.exception_handler(RequestValidationError)
    async def validation_error(request: Request, exc: RequestValidationError) -> JSONResponse:
        return _error_response(request, 422, "validation_error", str(exc))

    @app.get("/v1/health", tags=["operations"])
    def health() -> dict[str, object]:
        return {"status": "ok", "api_version": API_VERSION}

    @app.get("/v1/metadata.list_datasets", tags=["metadata"])
    def list_datasets(user: RequestPrincipal) -> dict[str, object]:
        datasets = [row for row in service.list_datasets() if user.can_read(str(row["dataset"]))]
        return {"data": datasets}

    @app.get("/v1/metadata.list_schemas", tags=["metadata"])
    def list_schemas(dataset: Annotated[str, Query()], user: RequestPrincipal) -> dict[str, object]:
        _require_dataset(user, dataset)
        return {"data": service.list_schemas(dataset)}

    @app.get("/v1/metadata.get_schema", tags=["metadata"])
    def get_schema(
        dataset: Annotated[str, Query()],
        schema: Annotated[str, Query()],
        user: RequestPrincipal,
    ) -> dict[str, object]:
        _require_dataset(user, dataset, schema)
        return {"data": service.schema(dataset, schema)}

    @app.get("/v1/metadata.get_dataset_range", tags=["metadata"])
    def get_dataset_range(
        dataset: Annotated[str, Query()],
        user: RequestPrincipal,
    ) -> dict[str, object]:
        _require_dataset(user, dataset)
        return {"data": service.dataset_range(dataset)}

    @app.get("/v1/metadata.get_dataset_condition", tags=["metadata"])
    def get_dataset_condition(
        dataset: Annotated[str, Query()],
        user: RequestPrincipal,
        start_date: Annotated[dt.date | None, Query()] = None,
        end_date: Annotated[dt.date | None, Query()] = None,
        schema: Annotated[str | None, Query()] = None,
    ) -> dict[str, object]:
        _require_dataset(user, dataset, schema)
        return {
            "data": service.dataset_condition(
                dataset,
                start=start_date,
                end=end_date,
                schema_code=schema,
            )
        }

    @app.get("/v1/metadata.get_schema_coverage", tags=["metadata"])
    def get_schema_coverage(
        dataset: Annotated[str, Query()],
        user: RequestPrincipal,
        schema: Annotated[str | None, Query()] = None,
    ) -> dict[str, object]:
        _require_dataset(user, dataset, schema)
        return {"data": service.schema_coverage(dataset, schema)}

    @app.get("/v1/metadata.list_unit_prices", tags=["metadata"])
    def list_unit_prices(
        user: RequestPrincipal,
        dataset: Annotated[str | None, Query()] = None,
    ) -> dict[str, object]:
        if dataset is not None:
            _require_dataset(user, dataset)
        rows = [price.public() for price in pricing.list(dataset=dataset) if user.can_read(price.dataset, price.schema)]
        return {"data": rows}

    def estimate(
        payload: BatchRangeRequest,
        response: Response,
        user: ApiPrincipal,
    ) -> tuple[RangeEstimate, UnitPrice | None]:
        _require_dataset(user, payload.dataset, payload.schema_name)
        _enforce_rate_limit(limiter, user, payload.dataset, response)
        result = service.estimate_range(payload)
        response.headers["X-ATX-Records"] = str(result.record_count)
        response.headers["X-ATX-Usage-Bytes"] = str(result.billable_bytes)
        return result, pricing.get(payload.dataset, payload.schema_name)

    @app.post("/v1/metadata.get_record_count", tags=["metadata"])
    def get_record_count(
        payload: BatchRangeRequest,
        response: Response,
        user: RequestPrincipal,
    ) -> dict[str, object]:
        result, _ = estimate(payload, response, user)
        return {
            "record_count": result.record_count,
            "truncated": result.metadata["truncated"],
        }

    @app.post("/v1/metadata.get_billable_size", tags=["metadata"])
    def get_billable_size(
        payload: BatchRangeRequest,
        response: Response,
        user: RequestPrincipal,
    ) -> dict[str, object]:
        result, _ = estimate(payload, response, user)
        return {
            "billable_bytes": result.billable_bytes,
            "billing_unit": "uncompressed_arrow_bytes",
            "record_count": result.record_count,
            "truncated": result.metadata["truncated"],
        }

    @app.post("/v1/metadata.get_cost", tags=["metadata"])
    def get_cost(
        payload: BatchRangeRequest,
        response: Response,
        user: RequestPrincipal,
    ) -> dict[str, object]:
        result, unit_price = estimate(payload, response, user)
        cost = calculate_cost(result.billable_bytes, unit_price)
        return {
            "cost_usd": None if cost is None else float(cost),
            "billable_bytes": result.billable_bytes,
            "record_count": result.record_count,
            "unit_price": None if unit_price is None else unit_price.public(),
            "is_estimate": True,
        }

    @app.post("/v1/symbology.resolve", tags=["symbology"])
    def resolve_symbology(
        payload: SymbologyRequest,
        request: Request,
        response: Response,
        user: RequestPrincipal,
    ) -> dict[str, object]:
        started_at = _now()
        result = service.resolve_symbology(payload)
        record_count = sum(len(rows) for rows in result["result"].values())
        response.headers["X-ATX-Records"] = str(record_count)
        _record(
            ledger,
            request,
            user,
            endpoint="symbology.resolve",
            dataset=None,
            schema=None,
            started_at=started_at,
            record_count=record_count,
            response_bytes=0,
        )
        return result

    @app.post("/v1/timeseries.get_range", tags=["timeseries"])
    def get_range(
        payload: RangeRequest,
        request: Request,
        response: Response,
        user: RequestPrincipal,
    ) -> dict[str, object]:
        _require_dataset(user, payload.dataset, payload.schema_name)
        _enforce_rate_limit(limiter, user, payload.dataset, response)
        remaining_bytes = _enforce_monthly_quota(ledger, user, payload.dataset, response)
        maximum = user.max_sync_rows(payload.dataset, payload.limit)
        if payload.limit > maximum:
            raise HTTPException(
                status_code=403,
                detail={
                    "code": "row_limit_exceeded",
                    "message": f"entitlement limits synchronous requests to {maximum} rows",
                },
            )
        started_at = _now()
        result = service.get_range(payload)
        if remaining_bytes is not None and result.billable_bytes > remaining_bytes:
            raise HTTPException(
                status_code=429,
                detail={
                    "code": "request_would_exceed_monthly_byte_quota",
                    "message": "request exceeds the remaining monthly dataset byte quota",
                },
                headers={
                    "X-ATX-Rate-Limited-Reason": "account-dataset-monthly-bytes",
                    "X-ATX-Monthly-Bytes-Remaining": str(remaining_bytes),
                },
            )
        record_count = int(result.metadata["record_count"])
        response.headers["X-ATX-Records"] = str(record_count)
        response.headers["X-ATX-Usage-Bytes"] = str(result.billable_bytes)
        response.headers["X-ATX-Response-Bytes"] = str(result.response_bytes)
        unit_price = pricing.get(payload.dataset, payload.schema_name)
        _record(
            ledger,
            request,
            user,
            endpoint="timeseries.get_range",
            dataset=payload.dataset,
            schema=payload.schema_name,
            started_at=started_at,
            record_count=record_count,
            response_bytes=result.response_bytes,
            billable_bytes=result.billable_bytes,
            cost_usd=calculate_cost(result.billable_bytes, unit_price),
            billing_mode="historical_stream",
        )
        return {"metadata": result.metadata, "data": result.data}

    @app.post("/v1/batch.submit_job", tags=["batch"], status_code=202)
    def submit_batch(
        payload: BatchSubmitRequest,
        background_tasks: BackgroundTasks,
        request: Request,
        response: Response,
        user: RequestPrincipal,
        idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key", max_length=255)] = None,
    ) -> dict[str, object]:
        _require_batch_scope(user, write=True)
        _require_dataset(user, payload.request.dataset, payload.request.schema_name)
        _enforce_rate_limit(limiter, user, payload.request.dataset, response)
        _enforce_monthly_quota(ledger, user, payload.request.dataset, response)
        started_at = _now()
        request_sha256 = hashlib.sha256(payload.model_dump_json(by_alias=True).encode()).hexdigest()
        if idempotency_key is not None:
            if not idempotency_key.strip():
                raise HTTPException(
                    status_code=400,
                    detail={"code": "invalid_idempotency_key", "message": "Idempotency-Key cannot be blank"},
                )
            try:
                replay = idempotency.begin(
                    user.account_id,
                    "batch.submit_job",
                    idempotency_key,
                    request_sha256,
                    ttl=dt.timedelta(hours=24),
                )
            except IdempotencyConflict as exc:
                raise HTTPException(
                    status_code=409,
                    detail={"code": "idempotency_key_conflict", "message": str(exc)},
                ) from exc
            except IdempotencyInProgress as exc:
                raise HTTPException(
                    status_code=409,
                    detail={"code": "idempotency_request_in_progress", "message": str(exc)},
                    headers={"Retry-After": "1"},
                ) from exc
            if replay is not None:
                job = jobs.get(replay.resource_id or "", user.account_id)
                if job is None:
                    raise HTTPException(
                        status_code=409,
                        detail={
                            "code": "idempotency_resource_missing",
                            "message": "the original idempotent resource is unavailable",
                        },
                    )
                response.headers["X-ATX-Idempotent-Replayed"] = "true"
                return {"data": _public_job(job)}
        try:
            job = jobs.submit(
                payload,
                user,
                idempotency_key=idempotency_key,
                request_sha256=request_sha256,
            )
            if idempotency_key is not None:
                idempotency.complete(
                    user.account_id,
                    "batch.submit_job",
                    idempotency_key,
                    resource_type="batch_job",
                    resource_id=job.job_id,
                    response_status=202,
                    response={"job_id": job.job_id},
                )
        except Exception:
            if idempotency_key is not None:
                idempotency.abort(user.account_id, "batch.submit_job", idempotency_key)
            raise
        if inline_batches:
            background_tasks.add_task(jobs.process, job.job_id)
        _record(
            ledger,
            request,
            user,
            endpoint="batch.submit_job",
            dataset=payload.request.dataset,
            schema=payload.request.schema_name,
            started_at=started_at,
            record_count=0,
            response_bytes=0,
            request_id=f"{request.state.request_id}:submit",
        )
        return {"data": _public_job(job)}

    @app.get("/v1/batch.list_jobs", tags=["batch"])
    def list_batch_jobs(
        user: RequestPrincipal,
        limit: Annotated[int, Query(ge=1, le=1_000)] = 100,
    ) -> dict[str, object]:
        _require_batch_scope(user)
        return {"data": [_public_job(job) for job in jobs.list(user.account_id, limit=limit)]}

    @app.get("/v1/batch.get_job", tags=["batch"])
    def get_batch_job(job_id: Annotated[str, Query()], user: RequestPrincipal) -> dict[str, object]:
        _require_batch_scope(user)
        job = jobs.get(job_id, user.account_id)
        if job is None:
            raise HTTPException(status_code=404, detail={"code": "job_not_found", "message": "batch job not found"})
        return {"data": _public_job(job)}

    @app.post("/v1/batch.cancel_job", tags=["batch"])
    def cancel_batch_job(job_id: Annotated[str, Query()], user: RequestPrincipal) -> dict[str, object]:
        _require_batch_scope(user, write=True)
        job = jobs.cancel(job_id, user.account_id)
        if job is None:
            raise HTTPException(status_code=404, detail={"code": "job_not_found", "message": "batch job not found"})
        if job.state not in {"queued", "cancelled"}:
            raise HTTPException(
                status_code=409,
                detail={"code": "job_not_cancellable", "message": f"job is already {job.state}"},
            )
        return {"data": _public_job(job)}

    @app.get("/v1/batch.download", tags=["batch"])
    def download_batch(
        job_id: Annotated[str, Query()],
        request: Request,
        user: RequestPrincipal,
    ) -> FileResponse:
        _require_batch_scope(user)
        started_at = _now()
        job = jobs.get(job_id, user.account_id)
        if job is None:
            raise HTTPException(status_code=404, detail={"code": "job_not_found", "message": "batch job not found"})
        path = jobs.artifact_path(job)
        if path is None:
            code = "artifact_expired" if jobs.is_expired(job) else "artifact_not_ready"
            raise HTTPException(
                status_code=410 if code == "artifact_expired" else 409,
                detail={"code": code, "message": code.replace("_", " ")},
            )
        _record(
            ledger,
            request,
            user,
            endpoint="batch.download",
            dataset=job.request.dataset,
            schema=job.request.schema_name,
            started_at=started_at,
            record_count=job.record_count,
            response_bytes=job.package_bytes,
            request_id=f"{request.state.request_id}:download",
        )
        return FileResponse(
            path,
            media_type=_batch_media_type(job.encoding),
            filename=path.name,
            headers={
                "X-ATX-Records": str(job.record_count),
                "X-ATX-Usage-Bytes": str(job.billed_bytes),
                "X-ATX-SHA256": job.result_sha256 or "",
            },
        )

    return app


def _authenticate_request(request: Request) -> ApiPrincipal:
    api_key = request.headers.get("x-api-key")
    authorization = request.headers.get("authorization")
    if api_key is None and authorization:
        scheme, _, credentials = authorization.partition(" ")
        if scheme.lower() == "bearer" and credentials:
            api_key = credentials.strip()
    if not api_key:
        raise HTTPException(
            status_code=401,
            detail={
                "code": "missing_api_key",
                "message": "provide a Bearer or X-API-Key credential",
            },
            headers={"WWW-Authenticate": "Bearer"},
        )
    authenticator: Authenticator = request.app.state.authenticator
    authenticated = authenticator.authenticate(api_key)
    if authenticated is None:
        raise HTTPException(
            status_code=401,
            detail={"code": "invalid_api_key", "message": "API key is invalid or inactive"},
            headers={"WWW-Authenticate": "Bearer"},
        )
    return authenticated


RequestPrincipal = Annotated[ApiPrincipal, Depends(_authenticate_request)]


def _require_dataset(principal: ApiPrincipal, dataset: str, schema: str | None = None) -> None:
    if not principal.can_read(dataset, schema):
        raise HTTPException(
            status_code=403,
            detail={
                "code": "dataset_not_entitled",
                "message": f"account is not entitled to dataset {dataset!r}",
            },
        )


def _require_batch_scope(principal: ApiPrincipal, *, write: bool = False) -> None:
    required = "batch:write" if write else "batch:read"
    if not principal.has_scope(required) and not (not write and principal.has_scope("batch:write")):
        raise HTTPException(
            status_code=403,
            detail={"code": "scope_forbidden", "message": f"API key requires {required!r} scope"},
        )


def _enforce_rate_limit(
    limiter: RateLimiter,
    principal: ApiPrincipal,
    dataset: str,
    response: Response,
) -> None:
    decision = limiter.consume(
        principal.account_id,
        principal.key_id,
        dataset,
        principal.requests_per_minute(dataset),
    )
    if decision.limit is None:
        return
    headers = {
        "RateLimit-Limit": str(decision.limit),
        "RateLimit-Remaining": str(decision.remaining or 0),
    }
    if decision.retry_after_seconds is not None:
        headers["Retry-After"] = str(decision.retry_after_seconds)
    if not decision.allowed:
        headers["X-ATX-Rate-Limited-Reason"] = "account-dataset-rate"
        raise HTTPException(
            status_code=429,
            detail={"code": "rate_limit_exceeded", "message": "dataset request rate exceeded"},
            headers=headers,
        )
    for name, value in headers.items():
        response.headers[name] = value


def _enforce_monthly_quota(
    ledger: UsageLedger,
    principal: ApiPrincipal,
    dataset: str,
    response: Response,
) -> int | None:
    limit = principal.bytes_per_month(dataset)
    if limit is None:
        return None
    start = month_start()
    used = ledger.billable_bytes_since(principal.account_id, dataset, start)
    remaining = max(0, limit - used)
    response.headers["X-ATX-Monthly-Byte-Limit"] = str(limit)
    response.headers["X-ATX-Monthly-Bytes-Remaining"] = str(remaining)
    if remaining > 0:
        return remaining
    next_month = (
        start.replace(year=start.year + 1, month=1) if start.month == 12 else start.replace(month=start.month + 1)
    )
    retry_after = max(1, int((next_month - _now()).total_seconds()))
    raise HTTPException(
        status_code=429,
        detail={"code": "monthly_byte_quota_exceeded", "message": "monthly dataset byte quota exhausted"},
        headers={
            "Retry-After": str(retry_after),
            "X-ATX-Rate-Limited-Reason": "account-dataset-monthly-bytes",
            "X-ATX-Monthly-Byte-Limit": str(limit),
            "X-ATX-Monthly-Bytes-Remaining": "0",
        },
    )


def _public_job(job: BatchJob) -> dict[str, object]:
    payload = job.public()
    if payload["state"] == "completed":
        payload["download_url"] = f"/v1/batch.download?job_id={payload['job_id']}"
    return payload


def _batch_media_type(encoding: str) -> str:
    return {
        "parquet": "application/vnd.apache.parquet",
        "arrow": "application/vnd.apache.arrow.stream",
        "csv": "text/csv",
        "jsonl": "application/x-ndjson",
    }[encoding]


def _record(
    ledger: UsageLedger,
    request: Request,
    principal: ApiPrincipal,
    *,
    endpoint: str,
    dataset: str | None,
    schema: str | None,
    started_at: dt.datetime,
    record_count: int,
    response_bytes: int,
    request_id: str | None = None,
    billable_bytes: int = 0,
    cost_usd: Decimal | None = None,
    billing_mode: str | None = None,
) -> None:
    ledger.record(
        UsageEvent(
            request_id=request_id or str(request.state.request_id),
            account_id=principal.account_id,
            key_id=principal.key_id,
            endpoint=endpoint,
            dataset=dataset,
            schema=schema,
            started_at=started_at,
            finished_at=_now(),
            status_code=200,
            record_count=record_count,
            response_bytes=response_bytes,
            billable_bytes=billable_bytes,
            cost_usd=cost_usd,
            billing_mode=billing_mode,
        )
    )


def _error_response(
    request: Request,
    status_code: int,
    code: str,
    message: str,
    *,
    headers: Mapping[str, str] | None = None,
) -> JSONResponse:
    return JSONResponse(
        status_code=status_code,
        content={
            "error": {
                "code": code,
                "message": message,
                "request_id": getattr(request.state, "request_id", None),
            }
        },
        headers=headers,
    )


def main() -> None:
    import uvicorn

    host = os.environ.get("ATX_DB_API_HOST", "127.0.0.1")
    port = int(os.environ.get("ATX_DB_API_PORT", "8080"))
    uvicorn.run("atx_db.api.app:create_app", factory=True, host=host, port=port)


if __name__ == "__main__":
    main()
