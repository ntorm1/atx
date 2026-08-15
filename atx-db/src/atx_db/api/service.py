"""Secure read-only query engine behind the customer API."""

from __future__ import annotations

import datetime as dt
import json
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import duckdb
import pyarrow as pa  # type: ignore[import-untyped]

from ..connection import DEFAULT_DB_PATH, open_duckdb_connection
from .catalog import DATASETS, RecordSchema, get_dataset, get_schema, public_catalog, public_schema
from .models import BatchRangeRequest, RangeRequest, SymbologyRequest, SymbolType


class ApiQueryError(ValueError):
    code = "invalid_request"


class DatasetNotFound(ApiQueryError):
    code = "dataset_not_found"


class SchemaNotFound(ApiQueryError):
    code = "schema_not_found"


class FieldNotFound(ApiQueryError):
    code = "field_not_found"


@dataclass(frozen=True)
class QueryResult:
    metadata: dict[str, Any]
    data: list[dict[str, Any]]
    response_bytes: int
    billable_bytes: int


@dataclass(frozen=True)
class RangeEstimate:
    metadata: dict[str, Any]
    record_count: int
    billable_bytes: int


@dataclass
class RangeBatchStream:
    metadata: dict[str, Any]
    reader: pa.RecordBatchReader
    limit: int
    connection: duckdb.DuckDBPyConnection
    record_count: int = 0
    truncated: bool = False

    def batches(self) -> Iterable[pa.RecordBatch]:
        for batch in self.reader:
            remaining = self.limit - self.record_count
            if remaining <= 0:
                self.truncated = batch.num_rows > 0
                break
            if batch.num_rows > remaining:
                batch = batch.slice(0, remaining)
                self.truncated = True
            self.record_count += batch.num_rows
            if batch.num_rows:
                yield batch
            if self.truncated:
                break
        self.metadata["record_count"] = self.record_count
        self.metadata["truncated"] = self.truncated

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> RangeBatchStream:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()


_STYPE_TO_IDENTIFIER = {
    "raw_symbol": "TICKER",
    "cik": "CIK",
    "cusip": "CUSIP",
}


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC)


def _naive_utc(value: dt.datetime) -> dt.datetime:
    if value.tzinfo is None:
        return value
    return value.astimezone(dt.UTC).replace(tzinfo=None)


def _normalize_symbol(value: str, stype: SymbolType) -> str:
    value = value.strip()
    if stype == "cik":
        digits = value.removeprefix("CIK").removeprefix("cik").strip()
        return digits.zfill(10) if digits.isdigit() else value.upper()
    return value if stype == "security_id" else value.upper()


def _placeholders(values: Iterable[object]) -> str:
    return ",".join("?" for _ in values)


def _quote_identifier(value: str) -> str:
    if not value.replace("_", "").isalnum() or value[0].isdigit():
        raise RuntimeError(f"unsafe catalog identifier: {value!r}")
    return f'"{value}"'


class WarehouseReadService:
    """Execute allow-listed, PIT-correct range and symbology queries.

    A fresh read-only DuckDB connection is used per call.  Deployments should point
    this service at an immutable published warehouse snapshot; the ingestion writer
    must not share the serving file.
    """

    def __init__(self, database_path: Path | str = DEFAULT_DB_PATH) -> None:
        self.database_path = Path(database_path)

    def _connect(self) -> duckdb.DuckDBPyConnection:
        return open_duckdb_connection(self.database_path, read_only=True)

    def list_datasets(self) -> list[dict[str, object]]:
        return public_catalog()

    def list_schemas(self, dataset_code: str) -> list[dict[str, object]]:
        try:
            dataset = get_dataset(dataset_code)
        except KeyError as exc:
            raise DatasetNotFound(f"unknown dataset {dataset_code!r}") from exc
        return [public_schema(schema) for schema in dataset.schemas]

    def schema(self, dataset_code: str, schema_code: str) -> dict[str, object]:
        try:
            schema = get_schema(dataset_code, schema_code)
        except KeyError as exc:
            if not any(dataset.code == dataset_code for dataset in DATASETS):
                raise DatasetNotFound(f"unknown dataset {dataset_code!r}") from exc
            raise SchemaNotFound(f"unknown schema {schema_code!r} for dataset {dataset_code!r}") from exc
        return public_schema(schema)

    def _coverage_records(self, dataset_code: str) -> list[dict[str, Any]]:
        try:
            dataset = get_dataset(dataset_code)
        except KeyError as exc:
            raise DatasetNotFound(f"unknown dataset {dataset_code!r}") from exc
        with self._connect() as conn:
            cursor = conn.execute(
                """
                WITH active_slo AS (
                    SELECT * EXCLUDE (slo_rank)
                    FROM (
                        SELECT
                            slo.*,
                            row_number() OVER (
                                PARTITION BY dataset_id,schema_code
                                ORDER BY valid_from DESC,slo_version DESC
                            ) AS slo_rank
                        FROM api_schema_coverage_slo slo
                        WHERE is_active
                          AND valid_from <= now()
                          AND coalesce(valid_to,TIMESTAMP '9999-12-31') > now()
                    )
                    WHERE slo_rank=1
                )
                SELECT
                    slo.dataset_id,
                    slo.schema_code,
                    slo.slo_version,
                    slo.expected_history_start,
                    slo.minimum_history_years,
                    slo.minimum_security_count,
                    slo.minimum_item_count,
                    slo.maximum_freshness_lag_days,
                    slo.citation,
                    slo.description AS slo_description,
                    snapshot.coverage_snapshot_id,
                    snapshot.schema_version,
                    snapshot.source_relation,
                    snapshot.time_column,
                    snapshot.observed_at,
                    snapshot.start_time,
                    snapshot.end_time,
                    snapshot.first_available_at,
                    snapshot.last_available_at,
                    coalesce(snapshot.record_count,0) AS record_count,
                    coalesce(snapshot.security_count,0) AS security_count,
                    snapshot.item_count,
                    snapshot.basis_count,
                    snapshot.history_years,
                    snapshot.freshness_lag_days,
                    coalesce(snapshot.condition,'pending') AS condition,
                    coalesce(snapshot.failed_slos_json,'[]') AS failed_slos_json,
                    snapshot.run_id
                FROM active_slo slo
                LEFT JOIN v_api_schema_coverage_current snapshot
                  ON snapshot.dataset_id=slo.dataset_id
                 AND snapshot.schema_code=slo.schema_code
                WHERE slo.dataset_id=?
                ORDER BY slo.schema_code
                """,
                [dataset_code],
            )
            columns = [str(column[0]) for column in cursor.description]
            records = [dict(zip(columns, row, strict=True)) for row in cursor.fetchall()]
        expected = {schema.code for schema in dataset.schemas}
        actual = {str(record["schema_code"]) for record in records}
        if actual != expected:
            missing = ", ".join(sorted(expected - actual))
            raise RuntimeError(f"coverage SLO catalog is incomplete for {dataset_code}: {missing}")
        for record in records:
            record["failed_slos"] = json.loads(str(record.pop("failed_slos_json")))
        return records

    def dataset_range(self, dataset_code: str) -> dict[str, object]:
        """Return measured inclusive-start/exclusive-end ranges by public schema."""

        records = self._coverage_records(dataset_code)
        starts = [record["start_time"] for record in records if record["start_time"] is not None]
        ends = [record["end_time"] for record in records if record["end_time"] is not None]
        return {
            "dataset": dataset_code,
            "start": min(starts) if starts else None,
            "end": max(ends) if ends else None,
            "schema": {
                str(record["schema_code"]): {
                    "start": record["start_time"],
                    "end": record["end_time"],
                    "condition": record["condition"],
                    "record_count": record["record_count"],
                    "security_count": record["security_count"],
                    "last_modified_date": (
                        None
                        if record["observed_at"] is None
                        else record["observed_at"].date()
                    ),
                }
                for record in records
            },
        }

    def schema_coverage(
        self,
        dataset_code: str,
        schema_code: str | None = None,
    ) -> list[dict[str, Any]]:
        records = self._coverage_records(dataset_code)
        if schema_code is None:
            return records
        try:
            get_schema(dataset_code, schema_code)
        except KeyError as exc:
            raise SchemaNotFound(
                f"unknown schema {schema_code!r} for dataset {dataset_code!r}"
            ) from exc
        return [record for record in records if record["schema_code"] == schema_code]

    def dataset_condition(
        self,
        dataset_code: str,
        *,
        start: dt.date | None = None,
        end: dt.date | None = None,
        schema_code: str | None = None,
    ) -> list[dict[str, object]]:
        if start is not None and end is not None and end <= start:
            raise ApiQueryError("end_date must be later than start_date; the interval is [start, end)")
        records = self.schema_coverage(dataset_code, schema_code)
        result: list[dict[str, object]] = []
        for record in records:
            interval_start = start or (
                record["start_time"].date()
                if record["start_time"] is not None
                else record["expected_history_start"]
            )
            interval_end = end or (
                record["end_time"].date()
                if record["end_time"] is not None
                else dt.date.today() + dt.timedelta(days=1)
            )
            result.append(
                {
                    "dataset": dataset_code,
                    "schema": record["schema_code"],
                    "start": interval_start,
                    "end": interval_end,
                    "condition": record["condition"],
                    "last_modified_date": (
                        None
                        if record["observed_at"] is None
                        else record["observed_at"].date()
                    ),
                    "failed_slos": record["failed_slos"],
                }
            )
        return result

    def _security_ids(
        self,
        conn: duckdb.DuckDBPyConnection,
        *,
        symbols: list[str],
        stype_in: SymbolType,
        start: dt.date,
        end: dt.date,
        as_of: dt.datetime,
    ) -> list[str] | None:
        if symbols == ["ALL_SYMBOLS"]:
            return None
        normalized = [_normalize_symbol(symbol, stype_in) for symbol in symbols]
        if stype_in == "security_id":
            rows = conn.execute(
                f"SELECT security_id FROM securities WHERE security_id IN ({_placeholders(normalized)})",
                normalized,
            ).fetchall()
            return sorted({str(row[0]) for row in rows})

        id_type = _STYPE_TO_IDENTIFIER[stype_in]
        rows = conn.execute(
            f"""
            SELECT DISTINCT security_id
            FROM security_identifier_history
            WHERE id_type = ?
              AND upper(id_value) IN ({_placeholders(normalized)})
              AND valid_from < ?
              AND coalesce(valid_to, DATE '9999-12-31') >= ?
              AND coalesce(available_at, source_loaded_at) <= ?
              AND as_of_date <= CAST(? AS DATE)
            """,
            [id_type, *normalized, end, start, _naive_utc(as_of), _naive_utc(as_of)],
        ).fetchall()
        return sorted({str(row[0]) for row in rows})

    @staticmethod
    def _projected_fields(request: RangeRequest, schema: RecordSchema) -> list[str]:
        requested = request.fields or list(schema.field_names)
        unknown = sorted(set(requested) - set(schema.field_names))
        if unknown:
            raise FieldNotFound(f"unknown fields for {schema.code}: {', '.join(unknown)}")
        if len(requested) != len(set(requested)):
            raise ApiQueryError("fields must not contain duplicates")
        return requested

    def get_range(self, request: RangeRequest) -> QueryResult:
        schema = self._validate_range_request(request, enforce_sync_limit=True)
        as_of = request.as_of or _utc_now()
        fields = self._projected_fields(request, schema)
        with self._connect() as conn:
            security_ids = self._security_ids(
                conn,
                symbols=request.symbols,
                stype_in=request.stype_in,
                start=request.start,
                end=request.end,
                as_of=as_of,
            )
            rows, columns, truncated = self._execute_range(conn, request, schema, as_of, fields, security_ids)
        data = [dict(zip(columns, row, strict=True)) for row in rows]
        return self._result(request, schema, as_of, fields, data, truncated=truncated)

    def stream_range(self, request: BatchRangeRequest, *, rows_per_batch: int = 65_536) -> RangeBatchStream:
        """Return an Arrow stream backed by a bounded, PIT-correct warehouse query."""

        schema = self._validate_range_request(request, enforce_sync_limit=False)
        as_of = request.as_of or _utc_now()
        fields = self._projected_fields(request, schema)
        conn = self._connect()
        try:
            security_ids = self._security_ids(
                conn,
                symbols=request.symbols,
                stype_in=request.stype_in,
                start=request.start,
                end=request.end,
                as_of=as_of,
            )
            cursor = self._range_cursor(conn, request, schema, as_of, fields, security_ids)
            reader = cursor.to_arrow_reader(batch_size=rows_per_batch)
        except BaseException:
            conn.close()
            raise
        metadata = self._metadata(request, schema, as_of, fields, record_count=0, truncated=False)
        return RangeBatchStream(metadata=metadata, reader=reader, limit=request.limit, connection=conn)

    def estimate_range(self, request: BatchRangeRequest) -> RangeEstimate:
        """Scan a request without materializing rows and return its exact Arrow-byte size."""

        billable_bytes = 0
        with self.stream_range(request) as stream:
            for batch in stream.batches():
                billable_bytes += batch.nbytes
            return RangeEstimate(
                metadata=stream.metadata,
                record_count=stream.record_count,
                billable_bytes=billable_bytes,
            )

    @staticmethod
    def _validate_range_request(
        request: RangeRequest,
        *,
        enforce_sync_limit: bool,
    ) -> RecordSchema:
        try:
            dataset = get_dataset(request.dataset)
        except KeyError as exc:
            raise DatasetNotFound(f"unknown dataset {request.dataset!r}") from exc
        try:
            schema = dataset.schema(request.schema_name)
        except KeyError as exc:
            raise SchemaNotFound(f"unknown schema {request.schema_name!r} for dataset {request.dataset!r}") from exc
        if enforce_sync_limit and request.limit > schema.max_sync_rows:
            raise ApiQueryError(f"limit {request.limit} exceeds synchronous maximum {schema.max_sync_rows}")
        if request.items and schema.item_column is None:
            raise ApiQueryError(f"schema {schema.code!r} does not support item filtering")
        if request.basis and schema.basis_column is None:
            raise ApiQueryError(f"schema {schema.code!r} does not support basis filtering")
        return schema

    def _execute_range(
        self,
        conn: duckdb.DuckDBPyConnection,
        request: RangeRequest,
        schema: RecordSchema,
        as_of: dt.datetime,
        fields: list[str],
        security_ids: list[str] | None,
    ) -> tuple[list[tuple[Any, ...]], list[str], bool]:
        cursor = self._range_cursor(conn, request, schema, as_of, fields, security_ids)
        all_rows = cursor.fetchall()
        columns = [str(column[0]) for column in cursor.description]
        return all_rows[: request.limit], columns, len(all_rows) > request.limit

    def _range_cursor(
        self,
        conn: duckdb.DuckDBPyConnection,
        request: RangeRequest,
        schema: RecordSchema,
        as_of: dt.datetime,
        fields: list[str],
        security_ids: list[str] | None,
    ) -> duckdb.DuckDBPyConnection:
        time_column = _quote_identifier(schema.time_column)
        table = _quote_identifier(schema.source_table)
        natural_key = ", ".join(f"b.{_quote_identifier(name)}" for name in schema.natural_key)
        direction = "ASC" if request.vintage == "first_reported" else "DESC"
        conditions = [
            f"b.{time_column} >= ?",
            f"b.{time_column} < ?",
            "coalesce(b.available_at, b.source_loaded_at) <= ?",
            "coalesce(b.as_of_date, CAST(coalesce(b.available_at, b.source_loaded_at) AS DATE)) <= CAST(? AS DATE)",
        ]
        parameters: list[object] = [
            request.start,
            request.end,
            _naive_utc(as_of),
            _naive_utc(as_of),
        ]
        if security_ids == []:
            conditions.append("false")
        elif security_ids is not None:
            conditions.append(f"b.security_id IN ({_placeholders(security_ids)})")
            parameters.extend(security_ids)
        if request.items:
            assert schema.item_column is not None
            conditions.append(f"b.{_quote_identifier(schema.item_column)} IN ({_placeholders(request.items)})")
            parameters.extend(request.items)
        if request.basis:
            assert schema.basis_column is not None
            conditions.append(f"b.{_quote_identifier(schema.basis_column)} IN ({_placeholders(request.basis)})")
            parameters.extend(request.basis)

        select_fields = []
        for name in fields:
            field = schema.field(name)
            select_fields.append(f"v.{_quote_identifier(field.source_column)} AS {_quote_identifier(field.name)}")
        sql = f"""
            WITH visible AS (
                SELECT b.*,
                       row_number() OVER (
                           PARTITION BY {natural_key}
                           ORDER BY coalesce(b.available_at, b.source_loaded_at) {direction},
                                    b.source_loaded_at {direction},
                                    coalesce(b.run_id, '') {direction}
                       ) AS _revision_rank
                FROM {table} AS b
                WHERE {" AND ".join(conditions)}
            )
            SELECT {", ".join(select_fields)}
            FROM visible AS v
            WHERE v._revision_rank = 1
            ORDER BY v.{time_column}, v.security_id
            LIMIT ?
        """
        parameters.append(request.limit + 1)
        return conn.execute(sql, parameters)

    @staticmethod
    def _result(
        request: RangeRequest,
        schema: RecordSchema,
        as_of: dt.datetime,
        fields: list[str],
        data: list[dict[str, Any]],
        *,
        truncated: bool,
    ) -> QueryResult:
        metadata = WarehouseReadService._metadata(
            request,
            schema,
            as_of,
            fields,
            record_count=len(data),
            truncated=truncated,
        )
        response_bytes = len(
            json.dumps({"metadata": metadata, "data": data}, default=str, separators=(",", ":")).encode("utf-8")
        )
        billable_bytes = 0 if not data else pa.Table.from_pylist(data).nbytes
        return QueryResult(
            metadata=metadata,
            data=data,
            response_bytes=response_bytes,
            billable_bytes=billable_bytes,
        )

    @staticmethod
    def _metadata(
        request: RangeRequest,
        schema: RecordSchema,
        as_of: dt.datetime,
        fields: list[str],
        *,
        record_count: int,
        truncated: bool,
    ) -> dict[str, Any]:
        return {
            "dataset": request.dataset,
            "schema": schema.code,
            "schema_version": schema.version,
            "start": request.start,
            "end": request.end,
            "as_of": as_of,
            "vintage": request.vintage,
            "fields": fields,
            "record_count": record_count,
            "truncated": truncated,
        }

    def resolve_symbology(self, request: SymbologyRequest) -> dict[str, Any]:
        as_of = request.as_of or _utc_now()
        with self._connect() as conn:
            inputs = self._input_intervals(conn, request, as_of)
            targets = self._target_intervals(conn, inputs, request.stype_out, as_of)

        result: dict[str, list[dict[str, Any]]] = {symbol: [] for symbol in request.symbols}
        for input_symbol, security_id, input_start, input_end in inputs:
            matching = targets.get(security_id, [])
            for output_symbol, target_start, target_end in matching:
                overlap_start = max(input_start, target_start, request.start)
                overlap_end = min(input_end, target_end, request.end)
                if overlap_start < overlap_end:
                    result[input_symbol].append(
                        {
                            "start": overlap_start,
                            "end": overlap_end,
                            "symbol": output_symbol,
                            "security_id": security_id,
                        }
                    )
        not_found = [symbol for symbol, mappings in result.items() if not mappings]
        partial = [
            symbol
            for symbol, mappings in result.items()
            if mappings and not _covers_interval(mappings, request.start, request.end)
        ]
        status = 2 if len(not_found) == len(request.symbols) else (1 if not_found or partial else 0)
        return {
            "result": result,
            "symbols": request.symbols,
            "stype_in": request.stype_in,
            "stype_out": request.stype_out,
            "start_date": request.start,
            "end_date": request.end,
            "as_of": as_of,
            "partial": partial,
            "not_found": not_found,
            "message": ("Not found" if status == 2 else "Partially resolved" if status == 1 else "OK"),
            "status": status,
        }

    def _input_intervals(
        self,
        conn: duckdb.DuckDBPyConnection,
        request: SymbologyRequest,
        as_of: dt.datetime,
    ) -> list[tuple[str, str, dt.date, dt.date]]:
        normalized_to_original = {_normalize_symbol(symbol, request.stype_in): symbol for symbol in request.symbols}
        normalized = list(normalized_to_original)
        if request.stype_in == "security_id":
            rows = conn.execute(
                f"SELECT security_id FROM securities WHERE security_id IN ({_placeholders(normalized)})",
                normalized,
            ).fetchall()
            return [(normalized_to_original[str(row[0])], str(row[0]), request.start, request.end) for row in rows]
        id_type = _STYPE_TO_IDENTIFIER[request.stype_in]
        rows = conn.execute(
            f"""
            SELECT upper(id_value), security_id, greatest(valid_from, ?),
                   least(coalesce(valid_to, DATE '9999-12-31'), ?)
            FROM security_identifier_history
            WHERE id_type = ?
              AND upper(id_value) IN ({_placeholders(normalized)})
              AND valid_from < ?
              AND coalesce(valid_to, DATE '9999-12-31') >= ?
              AND coalesce(available_at, source_loaded_at) <= ?
              AND as_of_date <= CAST(? AS DATE)
            ORDER BY id_value, valid_from, security_id
            """,
            [
                request.start,
                request.end,
                id_type,
                *normalized,
                request.end,
                request.start,
                _naive_utc(as_of),
                _naive_utc(as_of),
            ],
        ).fetchall()
        return [(normalized_to_original[str(row[0])], str(row[1]), row[2], row[3]) for row in rows]

    def _target_intervals(
        self,
        conn: duckdb.DuckDBPyConnection,
        inputs: list[tuple[str, str, dt.date, dt.date]],
        stype_out: SymbolType,
        as_of: dt.datetime,
    ) -> dict[str, list[tuple[str, dt.date, dt.date]]]:
        security_ids = sorted({row[1] for row in inputs})
        if not security_ids:
            return {}
        bounds: dict[str, tuple[dt.date, dt.date]] = {}
        for _, security_id, start, end in inputs:
            prior = bounds.get(security_id)
            bounds[security_id] = (
                min(prior[0], start) if prior else start,
                max(prior[1], end) if prior else end,
            )
        if stype_out == "security_id":
            return {security_id: [(security_id, start, end)] for security_id, (start, end) in bounds.items()}
        id_type = _STYPE_TO_IDENTIFIER[stype_out]
        rows = conn.execute(
            f"""
            SELECT security_id, id_value, valid_from, coalesce(valid_to, DATE '9999-12-31')
            FROM security_identifier_history
            WHERE id_type = ?
              AND security_id IN ({_placeholders(security_ids)})
              AND coalesce(available_at, source_loaded_at) <= ?
              AND as_of_date <= CAST(? AS DATE)
            ORDER BY security_id, valid_from, id_value
            """,
            [id_type, *security_ids, _naive_utc(as_of), _naive_utc(as_of)],
        ).fetchall()
        targets: dict[str, list[tuple[str, dt.date, dt.date]]] = {}
        for security_id, symbol, start, end in rows:
            targets.setdefault(str(security_id), []).append((str(symbol), start, end))
        return targets


def _covers_interval(mappings: list[dict[str, Any]], start: dt.date, end: dt.date) -> bool:
    intervals = sorted((mapping["start"], mapping["end"]) for mapping in mappings)
    covered_until = start
    for interval_start, interval_end in intervals:
        if interval_start > covered_until:
            return False
        covered_until = max(covered_until, interval_end)
        if covered_until >= end:
            return True
    return covered_until >= end
