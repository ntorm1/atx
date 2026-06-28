from __future__ import annotations

import datetime as dt
import io
from dataclasses import dataclass

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check, record_source_file


SOURCE_NAME = "FRED graph CSV"
FRED_GRAPH_CSV_URL = "https://fred.stlouisfed.org/graph/fredgraph.csv?id={series_id}"
DEFAULT_SERIES = ("DGS10", "DGS2", "FEDFUNDS", "UNRATE", "CPIAUCSL", "VIXCLS")
DEFAULT_SERIES_METADATA = {
    "DGS10": {
        "title": "Market Yield on U.S. Treasury Securities at 10-Year Constant Maturity",
        "frequency": "daily",
        "units": "percent",
        "seasonal_adjustment": "not seasonally adjusted",
    },
    "DGS2": {
        "title": "Market Yield on U.S. Treasury Securities at 2-Year Constant Maturity",
        "frequency": "daily",
        "units": "percent",
        "seasonal_adjustment": "not seasonally adjusted",
    },
    "FEDFUNDS": {
        "title": "Effective Federal Funds Rate",
        "frequency": "monthly",
        "units": "percent",
        "seasonal_adjustment": "not seasonally adjusted",
    },
    "UNRATE": {
        "title": "Unemployment Rate",
        "frequency": "monthly",
        "units": "percent",
        "seasonal_adjustment": "seasonally adjusted",
    },
    "CPIAUCSL": {
        "title": "Consumer Price Index for All Urban Consumers: All Items in U.S. City Average",
        "frequency": "monthly",
        "units": "index 1982-1984=100",
        "seasonal_adjustment": "seasonally adjusted",
    },
    "VIXCLS": {
        "title": "CBOE Volatility Index: VIX",
        "frequency": "daily",
        "units": "index",
        "seasonal_adjustment": "not seasonally adjusted",
    },
}


@dataclass(frozen=True)
class FredMacroOptions:
    series_ids: tuple[str, ...] = DEFAULT_SERIES
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    request_timeout: int = 60
    user_agent: str = "atx-impl macro loader nathan.tormaschy@gmail.com"
    run_id: str | None = None


def _normalize_series(text: str, series_id: str, source_url: str, options: FredMacroOptions) -> pd.DataFrame:
    frame = pd.read_csv(io.StringIO(text), dtype=str)
    if "observation_date" not in frame.columns or series_id not in frame.columns:
        raise ValueError(f"Unexpected FRED CSV columns for {series_id}: {list(frame.columns)}")
    frame = pd.DataFrame(
        {
            "source": SOURCE_NAME,
            "series_id": series_id,
            "observation_date": pd.to_datetime(frame["observation_date"], errors="coerce").dt.date,
            "as_of_date": pd.to_datetime(frame["observation_date"], errors="coerce").dt.date,
            "available_at": pd.to_datetime(frame["observation_date"], errors="coerce") + pd.Timedelta(hours=23),
            "value": pd.to_numeric(frame[series_id].replace(".", pd.NA), errors="coerce"),
        }
    )
    frame = frame.dropna(subset=["observation_date"])
    if options.start_date is not None:
        frame = frame[frame["observation_date"] >= options.start_date]
    if options.end_date is not None:
        frame = frame[frame["observation_date"] <= options.end_date]
    frame = frame.dropna(subset=["value"])
    return frame.reset_index(drop=True)


class FredMacroDataset(Dataset):
    dataset_id = "fred_macro"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FredMacroOptions) -> DatasetLoadResult:
        session = requests.Session()
        session.headers.update({"User-Agent": options.user_agent, "Accept": "text/csv,*/*"})
        frames: list[pd.DataFrame] = []
        for series_id in options.series_ids:
            series_id = series_id.strip().upper()
            url = FRED_GRAPH_CSV_URL.format(series_id=series_id)
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={"series_id": series_id},
            )
            frames.append(_normalize_series(response.text, series_id, url, options))
        frame = pd.concat([frame for frame in frames if not frame.empty], ignore_index=True) if frames else pd.DataFrame()
        self._replace_series_metadata(store, options)
        rows = self._replace_rows(store, frame, options.series_ids)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="macro_observations",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"series_ids": options.series_ids},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=SOURCE_NAME,
            details={"series_ids": options.series_ids},
        )

    def _replace_series_metadata(self, store: DuckDBStore, options: FredMacroOptions) -> None:
        rows = []
        for raw_series_id in options.series_ids:
            series_id = raw_series_id.strip().upper()
            metadata = DEFAULT_SERIES_METADATA.get(series_id, {})
            rows.append(
                {
                    "source": SOURCE_NAME,
                    "series_id": series_id,
                    "title": metadata.get("title", series_id),
                    "frequency": metadata.get("frequency"),
                    "units": metadata.get("units"),
                    "seasonal_adjustment": metadata.get("seasonal_adjustment"),
                    "notes": "Loaded from FRED graph CSV latest-revision download; use ALFRED vintage data for strict macro revision PIT.",
                    "source_url": FRED_GRAPH_CSV_URL.format(series_id=series_id),
                    "run_id": options.run_id,
                }
            )
        frame = pd.DataFrame(rows)
        if frame.empty:
            return
        ids = pd.DataFrame({"series_id": frame["series_id"].tolist()})
        with store.transaction():
            store.con.register("fred_macro_series_delete", ids)
            store.con.register("fred_macro_series_load", frame)
            try:
                store.con.execute(
                    """
                    DELETE FROM macro_series
                    USING fred_macro_series_delete src
                    WHERE macro_series.source = ?
                      AND macro_series.series_id = src.series_id
                    """,
                    [SOURCE_NAME],
                )
                insert_frame(store, frame, "macro_series", "fred_macro_series_insert")
            finally:
                store.con.unregister("fred_macro_series_delete")
                store.con.unregister("fred_macro_series_load")

    def _replace_rows(self, store: DuckDBStore, frame: pd.DataFrame, series_ids: tuple[str, ...]) -> int:
        if frame.empty:
            return 0
        ids = pd.DataFrame({"series_id": [series_id.strip().upper() for series_id in series_ids]})
        with store.transaction():
            store.con.register("fred_delete_series", ids)
            store.con.register("fred_macro_load", frame)
            try:
                store.con.execute(
                    """
                    DELETE FROM macro_observations
                    USING fred_delete_series src
                    WHERE macro_observations.source = ?
                      AND macro_observations.series_id = src.series_id
                    """,
                    [SOURCE_NAME],
                )
                insert_frame(store, frame, "macro_observations", "fred_macro_insert")
            finally:
                store.con.unregister("fred_delete_series")
                store.con.unregister("fred_macro_load")
        return int(len(frame))
