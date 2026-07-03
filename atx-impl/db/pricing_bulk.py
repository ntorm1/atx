from __future__ import annotations

import datetime as dt
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import security_ids_for_symbols
from .ticker_history import _drop_invalid_ohlcv, disambiguate_vendor_collisions
from .warehouse import (
    file_sha256,
    insert_frame,
    quality_check,
    record_source_file,
    snake_case,
    symbol_key,
    vendor_security_id,
)


SOURCE_NAME = "bulk_bars_2015plus"
BULK_BAR_IDENTIFIER_TYPE = "BULK_BAR_SYMBOL"

COLUMN_ALIASES = {
    "symbol": ("symbol", "ticker", "ticker_tk", "today_ticker"),
    "trade_date": ("trade_date", "trading_date", "date", "pricedate", "price_date"),
    "open": ("open", "open_price"),
    "high": ("high", "high_price"),
    "low": ("low", "low_price"),
    "close": ("close", "close_price"),
    "volume": ("volume", "vol"),
    "adjusted_close": ("adjusted_close", "adj_close", "adjclose", "close_adj", "close_adjusted"),
    "available_at": ("available_at", "availability", "availability_at", "source_available_at"),
    "vendor_security_id": ("vendor_security_id", "vendor_id", "security_id_vendor"),
    "security_id": ("security_id",),
}

REQUIRED_COLUMNS = ("symbol", "trade_date", "open", "high", "low", "close", "volume")


@dataclass(frozen=True)
class BulkBarsOptions:
    source_file: Path | None = None
    source_zip: Path | None = None
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = dt.date(2015, 1, 1)
    end_date: dt.date | None = None
    chunk_size: int = 200_000
    max_chunks: int | None = None
    source: str = SOURCE_NAME
    compute_source_hash: bool = True
    run_id: str | None = field(default=None, compare=False)


def _source_path(options: BulkBarsOptions) -> Path:
    paths = [path for path in (options.source_file, options.source_zip) if path is not None]
    if len(paths) != 1:
        raise ValueError("Provide exactly one of source_file or source_zip")
    path = Path(paths[0])
    if not path.exists():
        raise FileNotFoundError(path)
    if options.chunk_size < 1:
        raise ValueError("chunk_size must be positive")
    return path


def _canonical_column_map(columns: list[str]) -> dict[str, str]:
    normalized = {snake_case(column).lower(): column for column in columns}
    mapping: dict[str, str] = {}
    for canonical, aliases in COLUMN_ALIASES.items():
        for alias in aliases:
            found = normalized.get(snake_case(alias).lower())
            if found is not None:
                mapping[canonical] = found
                break
    missing = [column for column in REQUIRED_COLUMNS if column not in mapping]
    if missing:
        raise ValueError(f"bulk bars missing required columns: {missing}")
    return mapping


def _iter_csv_chunks(options: BulkBarsOptions) -> Iterator[tuple[str, pd.DataFrame]]:
    path = _source_path(options)
    chunks_seen = 0
    if options.source_file is not None:
        for chunk in pd.read_csv(path, dtype=str, keep_default_na=False, chunksize=options.chunk_size):
            chunks_seen += 1
            if options.max_chunks is not None and chunks_seen > options.max_chunks:
                break
            yield path.name, chunk
        return

    with zipfile.ZipFile(path) as archive:
        members = [
            name
            for name in archive.namelist()
            if not name.endswith("/") and name.lower().endswith((".csv", ".txt"))
        ]
        if not members:
            raise RuntimeError(f"No CSV members found in {path}")
        for member in members:
            with archive.open(member) as handle:
                reader = pd.read_csv(handle, dtype=str, keep_default_na=False, chunksize=options.chunk_size)
                for chunk in reader:
                    chunks_seen += 1
                    if options.max_chunks is not None and chunks_seen > options.max_chunks:
                        return
                    yield member, chunk


def _availability(series: pd.Series, trade_dates: pd.Series) -> pd.Series:
    provided = pd.to_datetime(series.replace("", pd.NA), errors="coerce")
    default = pd.to_datetime(trade_dates, errors="coerce") + pd.Timedelta(hours=22)
    return provided.where(provided.notna(), default)


def _normalize_chunk(chunk: pd.DataFrame, options: BulkBarsOptions) -> pd.DataFrame:
    mapping = _canonical_column_map(list(chunk.columns))
    frame = pd.DataFrame()
    for canonical, source_column in mapping.items():
        frame[canonical] = chunk[source_column]

    frame["symbol"] = frame["symbol"].map(symbol_key)
    frame["trade_date"] = pd.to_datetime(frame["trade_date"], errors="coerce").dt.date
    if options.symbols is not None:
        symbols = {symbol_key(symbol) for symbol in options.symbols}
        frame = frame[frame["symbol"].isin(symbols)]
    if options.start_date is not None:
        frame = frame[frame["trade_date"] >= options.start_date]
    if options.end_date is not None:
        frame = frame[frame["trade_date"] <= options.end_date]
    if frame.empty:
        return frame.reset_index(drop=True)

    for column in ("open", "high", "low", "close", "adjusted_close"):
        if column in frame.columns:
            frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")
    frame["volume"] = pd.to_numeric(frame["volume"].replace("", pd.NA), errors="coerce").astype("Int64")
    if "adjusted_close" not in frame.columns:
        frame["adjusted_close"] = frame["close"]
    else:
        frame["adjusted_close"] = frame["adjusted_close"].where(frame["adjusted_close"].notna(), frame["close"])
    if "vendor_security_id" not in frame.columns:
        frame["vendor_security_id"] = frame["symbol"].map(lambda symbol: f"SYMBOL-{symbol}")
    else:
        frame["vendor_security_id"] = frame["vendor_security_id"].replace("", pd.NA).astype("string")
        frame["vendor_security_id"] = frame["vendor_security_id"].fillna(frame["symbol"].map(lambda s: f"SYMBOL-{s}"))
    if "available_at" not in frame.columns:
        frame["available_at"] = pd.to_datetime(frame["trade_date"], errors="coerce") + pd.Timedelta(hours=22)
    else:
        frame["available_at"] = _availability(frame["available_at"], frame["trade_date"])
    if "security_id" in frame.columns:
        frame["input_security_id"] = frame["security_id"].replace("", pd.NA).astype("string")
    else:
        frame["input_security_id"] = pd.NA
    return frame.dropna(subset=["symbol", "trade_date"]).reset_index(drop=True)


def _fallback_security_id(source: str, symbol: str, vendor_id: object) -> str:
    raw = "" if pd.isna(vendor_id) else str(vendor_id).strip()
    value = raw if raw else f"SYMBOL-{symbol}"
    return vendor_security_id(source, value)


def _apply_security_ids(store: DuckDBStore, frame: pd.DataFrame, options: BulkBarsOptions) -> pd.DataFrame:
    if frame.empty:
        return frame
    symbols = sorted({symbol for symbol in frame["symbol"].dropna().map(symbol_key) if symbol})
    resolved = security_ids_for_symbols(store, symbols)
    out = frame.copy()

    def resolve(row: pd.Series) -> str:
        symbol = symbol_key(row["symbol"])
        if symbol in resolved:
            return resolved[symbol]
        input_security_id = row.get("input_security_id")
        if pd.notna(input_security_id) and str(input_security_id).strip():
            return str(input_security_id).strip()
        return _fallback_security_id(options.source, symbol, row.get("vendor_security_id"))

    out["security_id"] = out.apply(resolve, axis=1)
    return out


def _canonical_bars(frame: pd.DataFrame, options: BulkBarsOptions) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    bars = pd.DataFrame(
        {
            "source": options.source,
            "security_id": frame["security_id"],
            "vendor_security_id": frame["vendor_security_id"].astype("string"),
            "symbol": frame["symbol"].map(symbol_key),
            "trade_date": frame["trade_date"],
            "open": frame["open"],
            "high": frame["high"],
            "low": frame["low"],
            "close": frame["close"],
            "adjusted_close": frame["adjusted_close"],
            "volume": frame["volume"],
            "vwap": pd.NA,
            "dividend_amount": pd.NA,
            "split_factor": pd.NA,
            "is_adjusted": frame["adjusted_close"].notna() & (frame["adjusted_close"] != frame["close"]),
            "available_at": frame["available_at"],
            "run_id": options.run_id,
        }
    )
    bars = bars.dropna(subset=["security_id", "symbol", "trade_date"]).reset_index(drop=True)
    return _drop_invalid_ohlcv(bars)


def _security_links(frame: pd.DataFrame, options: BulkBarsOptions) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    if frame.empty:
        empty = pd.DataFrame()
        return empty, empty, empty
    grouped = (
        frame.dropna(subset=["security_id", "symbol", "trade_date"])
        .groupby(["security_id", "symbol"], dropna=False)
        .agg(first_seen_date=("trade_date", "min"), last_seen_date=("trade_date", "max"))
        .reset_index()
    )
    securities = pd.DataFrame(
        {
            "security_id": grouped["security_id"],
            "issuer_id": pd.NA,
            "primary_symbol": grouped["symbol"],
            "name": grouped["symbol"],
            "asset_class": "EQUITY",
            "country": "US",
            "currency": "USD",
            "active": True,
            "first_seen_date": grouped["first_seen_date"],
            "last_seen_date": grouped["last_seen_date"],
            "source": options.source,
        }
    ).drop_duplicates(subset=["security_id"])
    identifiers = pd.DataFrame(
        {
            "security_id": grouped["security_id"],
            "id_type": BULK_BAR_IDENTIFIER_TYPE,
            "id_value": grouped["symbol"],
            "valid_from": grouped["first_seen_date"],
            "valid_to": pd.NaT,
            "as_of_date": grouped["first_seen_date"],
            "available_at": pd.to_datetime(grouped["first_seen_date"]) + pd.Timedelta(hours=22),
            "source": options.source,
            "run_id": options.run_id,
        }
    )
    listings = pd.DataFrame(
        {
            "security_id": grouped["security_id"],
            "ticker": grouped["symbol"],
            "exchange_code": pd.NA,
            "mic": pd.NA,
            "currency": "USD",
            "valid_from": grouped["first_seen_date"],
            "valid_to": pd.NaT,
            "as_of_date": grouped["first_seen_date"],
            "available_at": pd.to_datetime(grouped["first_seen_date"]) + pd.Timedelta(hours=22),
            "source": options.source,
            "run_id": options.run_id,
        }
    )
    return securities, identifiers, listings


def _upsert_links(
    store: DuckDBStore,
    securities: pd.DataFrame,
    identifiers: pd.DataFrame,
    listings: pd.DataFrame,
) -> None:
    if not securities.empty:
        store.con.register("bulk_bar_securities_load", securities)
        try:
            store.con.execute(
                """
                INSERT INTO securities (
                    security_id, issuer_id, primary_symbol, name, asset_class, country,
                    currency, active, first_seen_date, last_seen_date, source
                )
                SELECT
                    src.security_id, src.issuer_id, src.primary_symbol, src.name,
                    src.asset_class, src.country, src.currency, src.active,
                    src.first_seen_date, src.last_seen_date, src.source
                FROM bulk_bar_securities_load src
                WHERE NOT EXISTS (
                    SELECT 1 FROM securities dst WHERE dst.security_id = src.security_id
                )
                """
            )
        finally:
            store.con.unregister("bulk_bar_securities_load")
    if not identifiers.empty:
        store.con.register("bulk_bar_identifiers_load", identifiers)
        try:
            store.con.execute(
                """
                DELETE FROM security_identifier_history
                USING bulk_bar_identifiers_load src
                WHERE security_identifier_history.security_id = src.security_id
                  AND security_identifier_history.id_type = src.id_type
                  AND security_identifier_history.id_value = src.id_value
                  AND security_identifier_history.source = src.source
                """
            )
            insert_frame(store, identifiers, "security_identifier_history", "bulk_bar_identifiers_insert")
        finally:
            store.con.unregister("bulk_bar_identifiers_load")
    if not listings.empty:
        store.con.register("bulk_bar_listings_load", listings)
        try:
            store.con.execute(
                """
                DELETE FROM exchange_listings
                USING bulk_bar_listings_load src
                WHERE exchange_listings.security_id = src.security_id
                  AND exchange_listings.ticker = src.ticker
                  AND exchange_listings.source = src.source
                """
            )
            insert_frame(store, listings, "exchange_listings", "bulk_bar_listings_insert")
        finally:
            store.con.unregister("bulk_bar_listings_load")


class BulkBarsDataset(Dataset):
    """Offline generic OHLCV loader for modern bars.

    Symbol resolution first uses ``security_ids_for_symbols`` (SEC ticker,
    identifier history, then listings). If a supplied symbol is not known to the
    warehouse, the loader falls back to a deterministic source/vendor id and
    creates minimal security/listing rows; this preserves offline ingest but
    cannot disambiguate recycled tickers without a vendor id or crosswalk.
    """

    dataset_id = "bulk_daily_bars"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: BulkBarsOptions) -> DatasetLoadResult:
        path = _source_path(options)
        source_hash = file_sha256(path) if options.compute_source_hash else None
        record_source_file(
            store,
            dataset_id=self.dataset_id,
            source_url=str(path),
            cache_path=path,
            status="available",
            metadata={
                "source": options.source,
                "symbols": options.symbols,
                "start_date": options.start_date,
                "end_date": options.end_date,
                "archive_type": "zip" if options.source_zip is not None else "csv",
            },
            sha256=source_hash,
            compute_hash=False,
        )

        rows_loaded = 0
        chunks_seen = 0
        chunks_with_rows = 0
        members: set[str] = set()
        matched_symbols: set[str] = set()
        min_trade_date: dt.date | None = None
        max_trade_date: dt.date | None = None
        for member_name, raw_chunk in _iter_csv_chunks(options):
            chunks_seen += 1
            members.add(member_name)
            normalized = _normalize_chunk(raw_chunk, options)
            if normalized.empty:
                continue
            normalized = _apply_security_ids(store, normalized, options)
            bars = _canonical_bars(normalized, options)
            if bars.empty:
                continue
            chunks_with_rows += 1
            matched_symbols.update(bars["symbol"].dropna().map(symbol_key).tolist())
            chunk_min = bars["trade_date"].min()
            chunk_max = bars["trade_date"].max()
            min_trade_date = chunk_min if min_trade_date is None else min(min_trade_date, chunk_min)
            max_trade_date = chunk_max if max_trade_date is None else max(max_trade_date, chunk_max)
            rows_loaded += self._load_chunk(store, bars, normalized, options)

        rekeyed = disambiguate_vendor_collisions(store, options.source)
        details = {
            "chunks_seen": chunks_seen,
            "chunks_with_rows": chunks_with_rows,
            "members": sorted(members),
            "matched_symbols": sorted(matched_symbols),
            "matched_symbol_count": len(matched_symbols),
            "min_trade_date": min_trade_date,
            "max_trade_date": max_trade_date,
            "source_file_sha256": source_hash,
            "vendor_collisions_rekeyed": rekeyed,
        }
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="equity_daily_bars",
            check_name="rows_loaded",
            status="passed" if rows_loaded > 0 else "warning",
            observed_value=float(rows_loaded),
            threshold_value=1.0,
            details=details,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=str(path),
            details=details,
        )

    def _load_chunk(
        self,
        store: DuckDBStore,
        bars: pd.DataFrame,
        normalized: pd.DataFrame,
        options: BulkBarsOptions,
    ) -> int:
        securities, identifiers, listings = _security_links(normalized, options)
        store.con.register("bulk_equity_daily_bars_load", bars)
        try:
            with store.transaction():
                store.con.execute(
                    """
                    DELETE FROM equity_daily_bars AS dst
                    USING bulk_equity_daily_bars_load AS src
                    WHERE dst.source = src.source
                      AND dst.trade_date = src.trade_date
                      AND (
                          dst.security_id = src.security_id
                          OR (
                              coalesce(dst.vendor_security_id, '') = coalesce(src.vendor_security_id, '')
                              AND dst.symbol = src.symbol
                          )
                      )
                    """
                )
                insert_frame(store, bars, "equity_daily_bars", "bulk_equity_daily_bars_insert")
                _upsert_links(store, securities, identifiers, listings)
        finally:
            store.con.unregister("bulk_equity_daily_bars_load")
        return int(len(bars))
