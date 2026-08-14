"""Pydantic wire models for the ATX historical data API."""

from __future__ import annotations

import datetime as dt
from typing import Annotated, Literal

from pydantic import BaseModel, Field, model_validator

SymbolType = Literal["raw_symbol", "security_id", "cik", "cusip"]
Vintage = Literal["latest", "first_reported"]
BatchEncoding = Literal["parquet", "arrow", "csv", "jsonl"]
BatchCompression = Literal["none", "gzip", "zstd"]


class RangeRequest(BaseModel):
    dataset: str
    schema_name: str = Field(alias="schema")
    symbols: Annotated[list[str], Field(min_length=1, max_length=2_000)]
    stype_in: SymbolType = "raw_symbol"
    start: dt.date
    end: dt.date
    as_of: dt.datetime | None = None
    items: Annotated[list[str], Field(max_length=2_000)] = Field(default_factory=list)
    basis: Annotated[list[str], Field(max_length=20)] = Field(default_factory=list)
    fields: Annotated[list[str], Field(max_length=100)] = Field(default_factory=list)
    vintage: Vintage = "latest"
    limit: int = Field(default=10_000, ge=1, le=50_000)

    @model_validator(mode="after")
    def validate_range(self) -> RangeRequest:
        if self.end <= self.start:
            raise ValueError("end must be later than start; the interval is [start, end)")
        if "ALL_SYMBOLS" in self.symbols and self.symbols != ["ALL_SYMBOLS"]:
            raise ValueError("ALL_SYMBOLS cannot be combined with explicit symbols")
        return self


class BatchRangeRequest(RangeRequest):
    limit: int = Field(default=1_000_000, ge=1, le=10_000_000)


class BatchSubmitRequest(BaseModel):
    request: BatchRangeRequest
    encoding: BatchEncoding = "parquet"
    compression: BatchCompression = "zstd"
    expires_in_hours: int = Field(default=24, ge=1, le=168)


class SymbologyRequest(BaseModel):
    symbols: Annotated[list[str], Field(min_length=1, max_length=2_000)]
    stype_in: SymbolType
    stype_out: SymbolType = "security_id"
    start: dt.date
    end: dt.date
    as_of: dt.datetime | None = None

    @model_validator(mode="after")
    def validate_range(self) -> SymbologyRequest:
        if self.end <= self.start:
            raise ValueError("end must be later than start; the interval is [start, end)")
        return self


class ErrorDetail(BaseModel):
    code: str
    message: str
    request_id: str | None = None


class ErrorResponse(BaseModel):
    error: ErrorDetail
