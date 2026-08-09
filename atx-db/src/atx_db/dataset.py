from __future__ import annotations

import datetime as dt
import json
import traceback
import uuid
from abc import ABC, abstractmethod
from dataclasses import asdict, dataclass, fields, is_dataclass, replace
from typing import Any

from .connection import DuckDBStore


@dataclass(frozen=True)
class DatasetLoadResult:
    dataset_id: str
    rows_loaded: int
    source: str
    details: dict[str, Any]
    run_id: str | None = None


def _json_default(value: Any) -> str:
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return str(value)


def encode_params(params: Any) -> str:
    if is_dataclass(params):
        payload = asdict(params)
    elif isinstance(params, dict):
        payload = params
    else:
        payload = {"value": params}
    return json.dumps(payload, default=_json_default, sort_keys=True)


def with_run_id(options: Any, run_id: str) -> Any:
    if is_dataclass(options):
        names = {field.name for field in fields(options)}
        if "run_id" in names:
            return replace(options, run_id=run_id)
    if isinstance(options, dict):
        updated = dict(options)
        updated.setdefault("run_id", run_id)
        return updated
    return options


class Dataset(ABC):
    dataset_id: str
    source_name: str
    depends_on: tuple[str, ...] = ()

    @abstractmethod
    def ensure_schema(self, store: DuckDBStore) -> None:
        raise NotImplementedError

    @abstractmethod
    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        raise NotImplementedError

    def run(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        self.ensure_schema(store)
        run_id = str(uuid.uuid4())
        started_at = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
        params_json = encode_params(options)
        store.con.execute(
            """
            INSERT INTO dataset_runs (
                run_id, dataset_id, status, started_at, source, params_json
            )
            VALUES (?, ?, 'running', ?, ?, ?)
            """,
            [run_id, self.dataset_id, started_at, self.source_name, params_json],
        )
        try:
            result = self.load(store, with_run_id(options, run_id))
        except Exception as exc:
            finished_at = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
            store.con.execute(
                """
                UPDATE dataset_runs
                SET status = 'failed', finished_at = ?, error_message = ?
                WHERE run_id = ?
                """,
                [finished_at, f"{exc}\n{traceback.format_exc(limit=20)}", run_id],
            )
            raise

        finished_at = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
        store.con.execute(
            """
            UPDATE dataset_runs
            SET status = 'succeeded', finished_at = ?, rows_loaded = ?
            WHERE run_id = ?
            """,
            [finished_at, result.rows_loaded, run_id],
        )
        return replace(result, run_id=run_id)
