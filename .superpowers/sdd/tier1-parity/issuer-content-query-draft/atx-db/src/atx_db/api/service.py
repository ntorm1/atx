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
    def _normalized_cik(value: str) -> str:
        candidate = value.strip().upper().removeprefix("CIK").strip()
        if not candidate.isdigit():
            raise ApiQueryError("cik must contain only digits")
        if len(candidate) > 10:
            raise ApiQueryError("cik must contain at most 10 digits")
        return candidate.zfill(10)

    @staticmethod
    def _normalized_cik_sql(column: str) -> str:
        """Normalize only one-to-ten digit source values; never truncate a CIK."""
        return (
            f"CASE WHEN regexp_full_match(trim({column}), '^[0-9]{{1,10}}$') "
            f"THEN lpad(trim({column}), 10, '0') END"
        )

    def resolve_issuer_ticker(
        self,
        *,
        ticker: str,
        issuer_lookup_as_of: dt.datetime,
    ) -> dict[str, Any]:
        """Resolve a current-as-of ticker to a CIK without changing security APIs.

        The historical identifier rows are the authority.  ``sec_company_tickers``
        is reported only as a current-directory cross-check, so a changed current
        directory cannot suppress co-visible historical evidence or backdate it.
        """
        requested_ticker = ticker.strip().upper()
        if not requested_ticker:
            raise ApiQueryError("ticker must not be empty")
        lookup_at = _naive_utc(issuer_lookup_as_of)
        with self._connect() as conn:
            rows = conn.execute(
                """
                WITH co_visible AS (
                    SELECT
                        ticker.security_id AS directory_security_id,
                        CASE WHEN regexp_full_match(trim(cik.id_value), '^[0-9]{1,10}$')
                             THEN lpad(trim(cik.id_value), 10, '0') END AS lookup_cik,
                        greatest(ticker.valid_from, cik.valid_from) AS directory_valid_from,
                        least(coalesce(ticker.valid_to, DATE '9999-12-31'),
                              coalesce(cik.valid_to, DATE '9999-12-31')) AS directory_valid_to,
                        greatest(coalesce(ticker.available_at, ticker.source_loaded_at),
                                 coalesce(cik.available_at, cik.source_loaded_at)) AS directory_available_at,
                        greatest(ticker.as_of_date, cik.as_of_date) AS directory_as_of_date,
                        concat(ticker.source, '|', cik.source) AS directory_source,
                        EXISTS (
                            SELECT 1 FROM sec_company_tickers current_directory
                            WHERE current_directory.security_id = ticker.security_id
                              AND CASE WHEN regexp_full_match(trim(current_directory.cik), '^[0-9]{1,10}$')
                                       THEN lpad(trim(current_directory.cik), 10, '0') END
                                  = CASE WHEN regexp_full_match(trim(cik.id_value), '^[0-9]{1,10}$')
                                       THEN lpad(trim(cik.id_value), 10, '0') END
                        ) AS current_directory_cross_check
                    FROM security_identifier_history ticker
                    JOIN security_identifier_history cik
                      ON cik.security_id = ticker.security_id AND cik.id_type = 'CIK'
                    WHERE ticker.id_type = 'TICKER'
                      AND upper(ticker.id_value) = ?
                      AND ticker.valid_from <= CAST(? AS DATE)
                      AND coalesce(ticker.valid_to, DATE '9999-12-31') > CAST(? AS DATE)
                      AND cik.valid_from <= CAST(? AS DATE)
                      AND coalesce(cik.valid_to, DATE '9999-12-31') > CAST(? AS DATE)
                      AND ticker.as_of_date <= CAST(? AS DATE)
                      AND cik.as_of_date <= CAST(? AS DATE)
                      AND coalesce(ticker.available_at, ticker.source_loaded_at) <= ?
                      AND coalesce(cik.available_at, cik.source_loaded_at) <= ?
                      AND regexp_full_match(trim(cik.id_value), '^[0-9]{1,10}$')
                )
                SELECT * FROM co_visible
                ORDER BY lookup_cik, directory_security_id, directory_available_at DESC
                """,
                [requested_ticker, lookup_at, lookup_at, lookup_at, lookup_at, lookup_at, lookup_at, lookup_at, lookup_at],
            ).fetchall()
            columns = [str(column[0]) for column in conn.description]
        candidates = [dict(zip(columns, row, strict=True)) for row in rows]
        ciks = sorted({str(row["lookup_cik"]) for row in candidates})
        if not candidates:
            return {
                "requested_ticker": requested_ticker, "issuer_lookup_as_of": issuer_lookup_as_of,
                "status": "unresolved", "lookup_method": "ticker_cik_history_co_visible",
                "unavailable_reason": "no_co_visible_ticker_cik_identifier_history",
            }
        if len(ciks) != 1:
            return {
                "requested_ticker": requested_ticker, "issuer_lookup_as_of": issuer_lookup_as_of,
                "status": "ambiguous", "lookup_method": "ticker_cik_history_co_visible",
                "lookup_cik_candidates": ciks, "candidates": candidates,
                "unavailable_reason": "multiple_co_visible_ciks",
            }
        # Preserve every co-visible directory row.  A CIK can have more than one
        # current-as-of directory candidate, and collapsing those rows then
        # choosing evidence[0] would turn an unresolved market association into
        # an arbitrary security choice.  These rows establish only a CIK lookup;
        # no one of them is historical-security-qualified.
        evidence = candidates
        association_ambiguous = len(evidence) != 1
        association = {
            "market_security_id": None if association_ambiguous else evidence[0]["directory_security_id"],
            "association_method": "ticker_cik_history_co_visible",
            "association_scope": "query_asof_current_cik_directory",
            "historical_security_qualified": False,
            "unavailable_reason": (
                "multiple_current_directory_security_candidates"
                if association_ambiguous else "current_asof_candidate_not_historical_security_qualified"
            ),
        }
        return {
            "requested_ticker": requested_ticker, "issuer_lookup_as_of": issuer_lookup_as_of,
            "status": "resolved", "lookup_cik": ciks[0],
            "directory_candidate_status": "ambiguous_multiple_candidates" if association_ambiguous else "single_candidate",
            "directory_candidate_count": len(evidence),
            "directory_security_id": None if association_ambiguous else evidence[0]["directory_security_id"],
            "directory_valid_from": None if association_ambiguous else evidence[0]["directory_valid_from"],
            "directory_valid_to": None if association_ambiguous else evidence[0]["directory_valid_to"],
            "directory_available_at": None if association_ambiguous else evidence[0]["directory_available_at"],
            "directory_as_of_date": None if association_ambiguous else evidence[0]["directory_as_of_date"],
            "directory_source": None if association_ambiguous else evidence[0]["directory_source"],
            "current_directory_cross_check": None if association_ambiguous else evidence[0]["current_directory_cross_check"],
            "lookup_method": "ticker_cik_history_co_visible", "candidates": evidence,
            "issuer_market_association": association,
        }

    @staticmethod
    def _issuer_owner_ids(
        conn: duckdb.DuckDBPyConnection, *, cik: str, content_as_of: dt.datetime
    ) -> dict[str, tuple[str, ...]]:
        normalized_cik = WarehouseReadService._normalized_cik_sql("cik")
        rows = conn.execute(
            f"""
            WITH visible AS (
                SELECT security_id, {normalized_cik} AS normalized_cik
                FROM fundamental_fact_revisions
                WHERE coalesce(available_at, source_loaded_at) <= ?
                  AND coalesce(as_of_date, CAST(coalesce(available_at, source_loaded_at) AS DATE))
                      <= CAST(? AS DATE)
            ), selected AS (
                SELECT DISTINCT security_id FROM visible WHERE normalized_cik = ?
            )
            SELECT DISTINCT visible.security_id, visible.normalized_cik
            FROM visible JOIN selected USING (security_id)
            WHERE visible.normalized_cik IS NOT NULL
            ORDER BY visible.security_id, visible.normalized_cik
            """,
            [_naive_utc(content_as_of), _naive_utc(content_as_of), cik],
        ).fetchall()
        by_owner: dict[str, list[str]] = {}
        for owner, owner_cik in rows:
            by_owner.setdefault(str(owner), []).append(str(owner_cik))
        return {owner: tuple(sorted(set(owner_ciks))) for owner, owner_ciks in by_owner.items()}

    def issuer_content_range(
        self,
        *,
        schema_name: str,
        start: dt.date,
        end: dt.date,
        content_as_of: dt.datetime,
        cik: str | None = None,
        ticker: str | None = None,
        issuer_lookup_as_of: dt.datetime | None = None,
        items: list[str] | None = None,
        basis: list[str] | None = None,
        fields: list[str] | None = None,
        vintage: str = "latest",
        limit: int = 10_000,
    ) -> QueryResult:
        """Return bounded CIK-owned accounting content and separate lookup provenance."""
        if end <= start:
            raise ApiQueryError("end must be later than start; the interval is [start, end)")
        if bool(cik) == bool(ticker):
            raise ApiQueryError("supply exactly one of cik or ticker")
        if limit < 1 or limit > 50_000:
            raise ApiQueryError("issuer content limit must be between 1 and 50000")
        if vintage not in {"latest", "first_reported"}:
            raise ApiQueryError("issuer content vintage must be latest or first_reported")
        try:
            schema = get_schema("ATX.US.ISSUER_CONTENT", schema_name)
        except KeyError as exc:
            raise SchemaNotFound(f"unknown issuer-content schema {schema_name!r}") from exc
        requested = fields or list(schema.field_names)
        unknown = sorted(set(requested) - set(schema.field_names))
        if unknown:
            raise FieldNotFound(f"unknown issuer fields: {', '.join(unknown)}")
        if len(requested) != len(set(requested)):
            raise ApiQueryError("issuer fields must not contain duplicates")
        if items and schema.item_column is None:
            raise ApiQueryError(f"schema {schema.code!r} does not support item filtering")
        if basis and schema.basis_column is None:
            raise ApiQueryError(f"schema {schema.code!r} does not support basis filtering")
        lookup: dict[str, Any] | None = None
        if ticker is not None:
            if issuer_lookup_as_of is None:
                raise ApiQueryError("issuer_lookup_as_of is required with ticker")
            lookup = self.resolve_issuer_ticker(ticker=ticker, issuer_lookup_as_of=issuer_lookup_as_of)
            if lookup["status"] != "resolved":
                metadata = self._issuer_metadata(
                    schema=schema, content_as_of=content_as_of, lookup_cik=None, owner_map={},
                    derived_owners=[], excluded_derived_owners=[], issuer_lookup=lookup, fields=requested,
                    record_count=0, truncated=False,
                )
                return QueryResult(metadata=metadata, data=[], response_bytes=len(json.dumps(metadata, default=str).encode()), billable_bytes=0)
            cik = str(lookup["lookup_cik"])
        assert cik is not None
        normalized_cik = self._normalized_cik(cik)

        with self._connect() as conn:
            owner_map = self._issuer_owner_ids(conn, cik=normalized_cik, content_as_of=content_as_of)
            owners = sorted(owner_map)
            derived_owners = [owner for owner in owners if owner_map[owner] == (normalized_cik,)]
            excluded_derived_owners = [owner for owner in owners if owner not in derived_owners]
            if owners:
                rows, columns, truncated = self._issuer_content_rows(
                    conn, schema=schema, cik=normalized_cik, owners=owners, start=start, end=end,
                    content_as_of=content_as_of, items=items or [], basis=basis or [], fields=requested,
                    vintage=vintage, limit=limit,
                    derived_owners=derived_owners,
                )
            else:
                rows, columns, truncated = [], requested, False
        data = [dict(zip(columns, row, strict=True)) for row in rows]
        metadata = self._issuer_metadata(
            schema=schema, content_as_of=content_as_of, lookup_cik=normalized_cik, owner_map=owner_map,
            derived_owners=derived_owners, excluded_derived_owners=excluded_derived_owners,
            issuer_lookup=lookup, fields=requested, record_count=len(data), truncated=truncated,
        )
        response_bytes = len(json.dumps({"metadata": metadata, "data": data}, default=str, separators=(",", ":")).encode())
        return QueryResult(metadata=metadata, data=data, response_bytes=response_bytes,
                           billable_bytes=0 if not data else pa.Table.from_pylist(data).nbytes)

    def _issuer_content_rows(
        self, conn: duckdb.DuckDBPyConnection, *, schema: RecordSchema, cik: str, owners: list[str],
        start: dt.date, end: dt.date, content_as_of: dt.datetime, items: list[str], basis: list[str],
        fields: list[str], vintage: str, limit: int, derived_owners: list[str],
    ) -> tuple[list[tuple[Any, ...]], list[str], bool]:
        owner_marks = _placeholders(owners)
        direction = "ASC" if vintage == "first_reported" else "DESC"
        is_derived = schema.source_table == "derived_metric_values"
        if is_derived:
            owners = derived_owners
            if not owners:
                return [], fields, False
            owner_marks = _placeholders(owners)
        natural_key = "coalesce(b.revision_group_id, b.derived_value_id)" if is_derived else ", ".join(
            f"b.{_quote_identifier(name)}" for name in schema.natural_key
        )
        revision_order = f"coalesce(b.available_at, b.source_loaded_at) {direction}, b.source_loaded_at {direction}"
        if schema.source_table == "fundamental_statement_points":
            revision_order = f"b.as_of_date {direction}, {revision_order}, b.statement_point_id {direction}"
        elif schema.source_table == "fundamental_ttm_points":
            revision_order = f"b.as_of_date {direction}, {revision_order}, b.ttm_point_id {direction}"
        elif schema.source_table == "shares_outstanding_history":
            share_direction = "ASC" if vintage == "first_reported" else "DESC"
            revision_order = (
                f"b.effective_date {share_direction}, b.as_of_date {share_direction}, "
                f"coalesce(b.available_at, b.source_loaded_at) {share_direction}, "
                f"b.source_loaded_at {share_direction}, b.share_history_id {share_direction}"
            )
        elif is_derived:
            revision_order = f"coalesce(b.available_at, b.source_loaded_at) {direction}, b.derived_value_id {direction}"
        cik_condition = "" if is_derived else f"AND {self._normalized_cik_sql('b.cik')} = ?"
        params: list[object] = [*owners]
        if not is_derived:
            params.append(cik)
        params.extend([_naive_utc(content_as_of), _naive_utc(content_as_of)])
        conditions = [f"b.security_id IN ({owner_marks})", cik_condition.removeprefix("AND "),
                      "coalesce(b.available_at, b.source_loaded_at) <= ?",
                      "coalesce(b.as_of_date, CAST(coalesce(b.available_at, b.source_loaded_at) AS DATE)) <= CAST(? AS DATE)"]
        conditions = [condition for condition in conditions if condition]
        if not is_derived:
            conditions[:0] = [f"b.{_quote_identifier(schema.time_column)} >= ?", f"b.{_quote_identifier(schema.time_column)} < ?"]
            params = [start, end, *params]
        if items:
            conditions.append(f"b.{_quote_identifier(schema.item_column or '')} IN ({_placeholders(items)})")
            params.extend(items)
        if basis:
            conditions.append(f"b.{_quote_identifier(schema.basis_column or '')} IN ({_placeholders(basis)})")
            params.extend(basis)
        selected: list[str] = []
        for name in fields:
            if name == "issuer_owner_id":
                selected.append('v.security_id AS "issuer_owner_id"')
            elif name == "cik" and is_derived:
                selected.append('? AS "cik"')
            else:
                selected.append(f"v.{_quote_identifier(schema.field(name).source_column)} AS {_quote_identifier(name)}")
        if is_derived and "cik" in fields:
            params.append(cik)
        output_range = ""
        if is_derived:
            output_range = f"AND v.{_quote_identifier(schema.time_column)} >= ? AND v.{_quote_identifier(schema.time_column)} < ?"
            params.extend([start, end])
        sql = f"""
            WITH visible AS (
                SELECT b.*, row_number() OVER (
                    PARTITION BY {natural_key}
                    ORDER BY {revision_order}
                ) AS _revision_rank
                FROM {_quote_identifier(schema.source_table)} b
                WHERE {' AND '.join(conditions)}
            )
            SELECT {', '.join(selected)} FROM visible v
            WHERE v._revision_rank = 1 {output_range}
            ORDER BY v.{_quote_identifier(schema.time_column)}, v.security_id
            LIMIT ?
        """
        params.append(limit + 1)
        cursor = conn.execute(sql, params)
        rows = cursor.fetchall()
        columns = [str(column[0]) for column in cursor.description]
        return rows[:limit], columns, len(rows) > limit

    @staticmethod
    def _issuer_metadata(
        *, schema: RecordSchema, content_as_of: dt.datetime, lookup_cik: str | None,
        owner_map: dict[str, tuple[str, ...]], derived_owners: list[str], excluded_derived_owners: list[str],
        issuer_lookup: dict[str, Any] | None, fields: list[str], record_count: int, truncated: bool,
    ) -> dict[str, Any]:
        owners = sorted(owner_map)
        owner_status = "resolved_single_owner" if len(owners) == 1 else ("ambiguous_multiple_visible_owners" if owners else "unresolved_no_visible_source_owner")
        if excluded_derived_owners:
            owner_status = "ambiguous_owner_cik_collision"
        association = None if issuer_lookup is None else issuer_lookup.get("issuer_market_association")
        if association is None:
            association = {
                "market_security_id": None, "association_method": None, "association_scope": None,
                "historical_security_qualified": False, "unavailable_reason": "no_ticker_directory_candidate",
            }
        return {
            "dataset": schema.dataset, "schema": schema.code, "schema_version": schema.version,
            "content_as_of": content_as_of, "lookup_cik": lookup_cik, "issuer_owner_ids": owners,
            "issuer_owner_ciks": owner_map, "issuer_owner_status": owner_status,
            "derived_issuer_owner_ids": derived_owners, "excluded_derived_owner_ids": excluded_derived_owners,
            "issuer_lookup": issuer_lookup, "issuer_market_association": association,
            "record_count": record_count, "truncated": truncated, "fields": fields,
            "owner_semantics": "Company Facts source owner; not a tradable security or historical market association.",
        }

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
        revision_ties = f"b.source_loaded_at {direction}, coalesce(b.run_id, '') {direction}"
        if schema.source_table == "derived_metric_values":
            # Legacy rows have no reconstructed revision group; preserve their
            # separate identities. NULL invalid states participate in ranking.
            natural_key = "coalesce(b.revision_group_id, b.derived_value_id)"
            revision_ties = f"b.derived_value_id {direction}"
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
        output_time_predicate = ""
        if schema.source_table == "derived_metric_values":
            # A later observed stub period may rename the same bucket. Rank
            # its visible group before filtering the requested period range,
            # otherwise a range containing only the old date resurrects it.
            conditions = conditions[2:]
            parameters = parameters[2:]
            output_time_predicate = f"AND v.{time_column} >= ? AND v.{time_column} < ?"
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
        if output_time_predicate:
            parameters.extend([request.start, request.end])

        select_fields = []
        for name in fields:
            field = schema.field(name)
            select_fields.append(f"v.{_quote_identifier(field.source_column)} AS {_quote_identifier(field.name)}")
        output_order = f"v.{time_column}, v.security_id"
        if schema.source_table == "derived_metric_values":
            output_order += ", v.metric_code, v.derived_value_id"
        sql = f"""
            WITH visible AS (
                SELECT b.*,
                       row_number() OVER (
                           PARTITION BY {natural_key}
                           ORDER BY coalesce(b.available_at, b.source_loaded_at) {direction},
                                    {revision_ties}
                       ) AS _revision_rank
                FROM {table} AS b
                WHERE {" AND ".join(conditions)}
            )
            SELECT {", ".join(select_fields)}
            FROM visible AS v
            WHERE v._revision_rank = 1 {output_time_predicate}
            ORDER BY {output_order}
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
