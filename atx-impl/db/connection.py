from __future__ import annotations

import contextlib
from pathlib import Path
from typing import Iterator

import duckdb


DEFAULT_DB_PATH = Path(__file__).resolve().with_name("atx_impl.duckdb")


class DuckDBStore:
    """Small persistence wrapper used by atx-impl dataset loaders."""

    def __init__(self, path: Path | str = DEFAULT_DB_PATH, *, read_only: bool = False) -> None:
        self.path = Path(path)
        self.read_only = read_only
        self.connection: duckdb.DuckDBPyConnection | None = None

    def __enter__(self) -> "DuckDBStore":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = duckdb.connect(str(self.path), read_only=self.read_only)
        if not self.read_only:
            self.initialize()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self.connection is None:
            raise RuntimeError("DuckDBStore is not open; use it as a context manager")
        return self.connection

    def initialize(self) -> None:
        con = self.con
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS dataset_runs (
                run_id VARCHAR PRIMARY KEY,
                dataset_id VARCHAR NOT NULL,
                status VARCHAR NOT NULL,
                started_at TIMESTAMP NOT NULL,
                finished_at TIMESTAMP,
                rows_loaded BIGINT,
                source VARCHAR,
                params_json VARCHAR,
                error_message VARCHAR
            )
            """
        )
        from .schema import ensure_quant_schema

        ensure_quant_schema(self)
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS dataset_watermarks (
                dataset_id VARCHAR NOT NULL,
                watermark_name VARCHAR NOT NULL,
                watermark_value VARCHAR NOT NULL,
                updated_at TIMESTAMP NOT NULL DEFAULT now(),
                PRIMARY KEY (dataset_id, watermark_name)
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS security_identifiers (
                symbol VARCHAR NOT NULL,
                id_type VARCHAR NOT NULL,
                id_value VARCHAR NOT NULL,
                source VARCHAR NOT NULL,
                updated_at TIMESTAMP NOT NULL DEFAULT now(),
                PRIMARY KEY (symbol, id_type, id_value)
            )
            """
        )

    @contextlib.contextmanager
    def transaction(self) -> Iterator[duckdb.DuckDBPyConnection]:
        con = self.con
        con.execute("BEGIN TRANSACTION")
        try:
            yield con
        except Exception:
            con.execute("ROLLBACK")
            raise
        else:
            con.execute("COMMIT")


@contextlib.contextmanager
def connect(path: Path | str = DEFAULT_DB_PATH, *, read_only: bool = False) -> Iterator[DuckDBStore]:
    with DuckDBStore(path, read_only=read_only) as store:
        yield store
