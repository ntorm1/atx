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
from .warehouse import (
    insert_frame,
    quality_check,
    record_source_file,
    snake_case,
    symbol_key,
    vendor_security_id,
)


DEFAULT_TICKER_HISTORY_ZIP = Path.home() / "Downloads" / "tbltickerhistory3_10y.zip"
SOURCE_NAME = "tbltickerhistory3_10y"

SOURCE_COLUMNS = [
    "tradingDate",
    "securityID",
    "ticker_tk",
    "todayTicker",
    "dn",
    "open",
    "high",
    "low",
    "close",
    "closePr",
    "volume",
    "shares",
    "earnFlag",
    "ccVar",
    "hlVar",
    "rvVar",
    "expiryCount",
    "hEMove",
    "iEMove",
    "shD1",
    "lnD1",
    "atmCenI_decay",
    "atmCenI_st",
    "atmCenI_lt",
    "atmCenI_5d",
    "atmCenI_21d",
    "atmCenI_42d",
    "atmCenI_63d",
    "atmCenI_84d",
    "atmCenI_105d",
    "atmCenI_126d",
    "atmCenI_189d",
    "atmCenI_252d",
    "atmCenI_378d",
    "atmCenI_504d",
    "atmCenH_st",
    "atmCenH_lt",
    "atmCenH_decay",
    "atmCenH_5d",
    "atmCenH_21d",
    "atmCenH_42d",
    "atmCenH_63d",
    "atmCenH_84d",
    "atmCenH_105d",
    "atmCenH_126d",
    "atmCenH_189d",
    "atmCenH_252d",
    "atmCenH_378d",
    "atmCenH_504d",
    "nEarnCnt",
    "nEarnCnt_5d",
    "nEarnCnt_21d",
    "nEarnCnt_42d",
    "nEarnCnt_63d",
    "nEarnCnt_84d",
    "nEarnCnt_105d",
    "nEarnCnt_126d",
    "nEarnCnt_189d",
    "nEarnCnt_252d",
    "nEarnCnt_378d",
    "nEarnCnt_504d",
    "GICS",
    "closeUnadjPr",
    "returnFactor",
    "totalReturn",
    "cumulReturnFactor",
    "wkD1",
    "atmCenI_10d",
    "atmCenH_10d",
    "nEarnCnt_10d",
    "qtrD1",
]

RENAMES = {column: snake_case(column) for column in SOURCE_COLUMNS}
RENAMES["tradingDate"] = "trading_date"
RENAMES["securityID"] = "vendor_security_id"
RENAMES["todayTicker"] = "today_ticker"
RENAMES["GICS"] = "gics"

TEXT_COLUMNS = {"ticker_tk", "today_ticker", "earn_flag", "gics", "security_id", "source", "run_id"}
DATE_COLUMNS = {"trading_date"}
INT_COLUMNS = {"vendor_security_id", "dn", "volume", "shares", "expiry_count"}
INT_COLUMNS.update({column for column in RENAMES.values() if column.startswith("n_earn_cnt")})
TBLTICKERHISTORY_ID_TYPE = "TBLTICKERHISTORY_SECURITY_ID"


def _vendor_identifier_value(symbol: str, value: object) -> str:
    raw = "" if pd.isna(value) else str(value).strip()
    if raw in {"", "0", "<NA>", "nan", "NaN", "None"}:
        return f"SYMBOL-{symbol_key(symbol)}-VENDOR-{raw or 'MISSING'}"
    return raw


@dataclass(frozen=True)
class TickerHistoryOptions:
    zip_path: Path = DEFAULT_TICKER_HISTORY_ZIP
    symbols: tuple[str, ...] | None = ("AAPL",)
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    chunk_size: int = 200_000
    max_chunks: int | None = None
    source: str = SOURCE_NAME
    compute_source_hash: bool = False
    run_id: str | None = field(default=None, compare=False)


@dataclass(frozen=True)
class TickerHistoryArchiveProfileOptions:
    zip_path: Path = DEFAULT_TICKER_HISTORY_ZIP
    chunk_size: int = 200_000
    max_chunks: int | None = 3
    top_n: int = 25


def profile_ticker_history_archive(options: TickerHistoryArchiveProfileOptions) -> dict[str, object]:
    if not options.zip_path.exists():
        raise FileNotFoundError(options.zip_path)
    if options.chunk_size < 1:
        raise ValueError("chunk_size must be positive")
    if options.top_n < 1:
        raise ValueError("top_n must be positive")

    chunks: list[dict[str, object]] = []
    symbol_counts: dict[str, int] = {}
    total_rows = 0
    min_date: str | None = None
    max_date: str | None = None

    with zipfile.ZipFile(options.zip_path) as archive:
        infos = archive.infolist()
        if len(infos) != 1:
            raise RuntimeError(f"Expected one tbltickerhistory member, found {len(infos)}")
        info = infos[0]
        with archive.open(info.filename) as handle:
            reader = pd.read_csv(
                handle,
                sep="\t",
                dtype=str,
                keep_default_na=False,
                chunksize=options.chunk_size,
                usecols=["tradingDate", "ticker_tk", "todayTicker"],
            )
            for chunk_index, chunk in enumerate(reader, start=1):
                if options.max_chunks is not None and chunk_index > options.max_chunks:
                    break
                symbols = chunk["todayTicker"].where(chunk["todayTicker"].str.len() > 0, chunk["ticker_tk"]).map(symbol_key)
                counts = symbols.value_counts()
                for symbol, count in counts.items():
                    if symbol:
                        symbol_counts[symbol] = symbol_counts.get(symbol, 0) + int(count)
                chunk_min = str(chunk["tradingDate"].min())
                chunk_max = str(chunk["tradingDate"].max())
                min_date = chunk_min if min_date is None else min(min_date, chunk_min)
                max_date = chunk_max if max_date is None else max(max_date, chunk_max)
                total_rows += int(len(chunk))
                chunks.append(
                    {
                        "chunk_index": chunk_index,
                        "rows": int(len(chunk)),
                        "min_trading_date": chunk_min,
                        "max_trading_date": chunk_max,
                        "symbol_count": int(counts.size),
                        "top_symbols": [
                            {"symbol": str(symbol), "rows": int(count)}
                            for symbol, count in counts.head(options.top_n).items()
                        ],
                    }
                )

    top_symbols = sorted(symbol_counts.items(), key=lambda item: (-item[1], item[0]))[: options.top_n]
    return {
        "zip_path": str(options.zip_path),
        "zip_size_bytes": int(options.zip_path.stat().st_size),
        "member_name": info.filename,
        "member_size_bytes": int(info.file_size),
        "member_compressed_size_bytes": int(info.compress_size),
        "chunk_size": options.chunk_size,
        "chunks_profiled": len(chunks),
        "rows_profiled": total_rows,
        "min_trading_date": min_date,
        "max_trading_date": max_date,
        "symbol_count": len(symbol_counts),
        "top_symbols": [{"symbol": symbol, "rows": count} for symbol, count in top_symbols],
        "chunks": chunks,
    }


def _raw_table_columns_sql() -> str:
    fields: list[str] = [
        "source VARCHAR NOT NULL",
        "security_id VARCHAR NOT NULL",
        "run_id VARCHAR",
    ]
    for original in SOURCE_COLUMNS:
        column = RENAMES[original]
        if column in DATE_COLUMNS:
            dtype = "DATE"
        elif column in INT_COLUMNS:
            dtype = "BIGINT"
        elif column in TEXT_COLUMNS:
            dtype = "VARCHAR"
        else:
            dtype = "DOUBLE"
        fields.append(f"{column} {dtype}")
    fields.extend(
        [
            "available_at TIMESTAMP",
            "source_loaded_at TIMESTAMP NOT NULL DEFAULT now()",
        ]
    )
    return ",\n                ".join(fields)


def _iter_raw_chunks(options: TickerHistoryOptions) -> Iterator[pd.DataFrame]:
    with zipfile.ZipFile(options.zip_path) as archive:
        names = archive.namelist()
        if len(names) != 1:
            raise RuntimeError(f"Expected one tbltickerhistory member, found {len(names)}")
        with archive.open(names[0]) as handle:
            reader = pd.read_csv(
                handle,
                sep="\t",
                dtype=str,
                keep_default_na=False,
                chunksize=options.chunk_size,
            )
            for index, chunk in enumerate(reader, start=1):
                if options.max_chunks is not None and index > options.max_chunks:
                    break
                yield chunk


def _filter_chunk(chunk: pd.DataFrame, options: TickerHistoryOptions) -> pd.DataFrame:
    filtered = chunk
    if options.symbols is not None:
        symbols = {symbol_key(symbol) for symbol in options.symbols}
        mask = filtered["ticker_tk"].str.upper().isin(symbols) | filtered["todayTicker"].str.upper().isin(symbols)
        filtered = filtered.loc[mask]
    if options.start_date is not None:
        filtered = filtered.loc[filtered["tradingDate"] >= options.start_date.isoformat()]
    if options.end_date is not None:
        filtered = filtered.loc[filtered["tradingDate"] <= options.end_date.isoformat()]
    return filtered.reset_index(drop=True)


def _normalize_chunk(chunk: pd.DataFrame, options: TickerHistoryOptions) -> pd.DataFrame:
    missing = [column for column in SOURCE_COLUMNS if column not in chunk.columns]
    if missing:
        raise ValueError(f"tbltickerhistory missing expected columns: {missing}")

    frame = chunk[SOURCE_COLUMNS].rename(columns=RENAMES)
    frame["source"] = options.source
    frame["run_id"] = options.run_id
    frame["trading_date"] = pd.to_datetime(frame["trading_date"], errors="coerce").dt.date
    for column in INT_COLUMNS:
        if column in frame.columns:
            frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce").astype("Int64")
    for column in frame.columns:
        if column not in TEXT_COLUMNS and column not in DATE_COLUMNS and column not in INT_COLUMNS:
            frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")
    for column in ("ticker_tk", "today_ticker", "earn_flag", "gics"):
        frame[column] = frame[column].replace("", pd.NA).astype("string")

    symbol_for_mapping = frame["today_ticker"].fillna(frame["ticker_tk"]).fillna("").map(symbol_key)
    frame["_symbol_for_mapping"] = symbol_for_mapping
    return frame


def _apply_security_ids(store: DuckDBStore, frame: pd.DataFrame, options: TickerHistoryOptions) -> pd.DataFrame:
    symbols = sorted(set(frame["_symbol_for_mapping"].dropna().tolist()))
    sec_map = security_ids_for_symbols(store, symbols)

    def resolve(row: pd.Series) -> str:
        symbol = row["_symbol_for_mapping"]
        if symbol in sec_map:
            return sec_map[symbol]
        value = row["vendor_security_id"]
        return vendor_security_id("tbltickerhistory", _vendor_identifier_value(symbol, value))

    frame["security_id"] = frame.apply(resolve, axis=1)
    return frame.drop(columns=["_symbol_for_mapping"])


def _canonical_bars(frame: pd.DataFrame, options: TickerHistoryOptions) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    bars = pd.DataFrame(
        {
            "source": options.source,
            "security_id": frame["security_id"],
            "vendor_security_id": frame["vendor_security_id"].astype("string"),
            "symbol": frame["today_ticker"].fillna(frame["ticker_tk"]).map(symbol_key),
            "trade_date": frame["trading_date"],
            "open": frame["open"],
            "high": frame["high"],
            "low": frame["low"],
            "close": frame["close"],
            "adjusted_close": frame["close_pr"],
            "volume": frame["volume"],
            "vwap": pd.NA,
            "dividend_amount": pd.NA,
            "split_factor": frame["return_factor"],
            "is_adjusted": False,
            "available_at": pd.to_datetime(frame["trading_date"]) + pd.Timedelta(hours=22),
            "run_id": options.run_id,
        }
    )
    bars = bars.dropna(subset=["security_id", "symbol", "trade_date"]).reset_index(drop=True)
    return _drop_invalid_ohlcv(bars)


def _drop_invalid_ohlcv(bars: pd.DataFrame) -> pd.DataFrame:
    """Drop structurally-invalid OHLCV rows from the canonical bar frame.

    The broad vendor archive carries ~0.7% malformed rows (non-positive prices,
    high < max(open, low, close), low > min(open, high, close), negative volume).
    The raw ``tbltickerhistory_daily`` table preserves them for lineage, but the
    canonical ``equity_daily_bars`` surface must be clean — these rows produce
    nonsensical returns/volatility. NaN comparisons are treated as valid (kept),
    matching the SQL ``bad_ohlcv_values`` quality check's null semantics.
    """
    if bars.empty:
        return bars
    o = pd.to_numeric(bars["open"], errors="coerce")
    h = pd.to_numeric(bars["high"], errors="coerce")
    low = pd.to_numeric(bars["low"], errors="coerce")
    c = pd.to_numeric(bars["close"], errors="coerce")
    v = pd.to_numeric(bars["volume"], errors="coerce")
    bad = (
        (v < 0)
        | (o <= 0)
        | (h <= 0)
        | (low <= 0)
        | (c <= 0)
        | (h < pd.concat([o, low, c], axis=1).max(axis=1))
        | (low > pd.concat([o, h, c], axis=1).min(axis=1))
    ).fillna(False)
    return bars.loc[~bad].reset_index(drop=True)


def _security_links(frame: pd.DataFrame, options: TickerHistoryOptions) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    if frame.empty:
        empty = pd.DataFrame()
        return empty, empty, empty
    grouped = (
        frame.assign(symbol=frame["today_ticker"].fillna(frame["ticker_tk"]).map(symbol_key))
        .dropna(subset=["security_id", "vendor_security_id", "symbol", "trading_date"])
        .groupby(["security_id", "vendor_security_id", "symbol"], dropna=False)
        .agg(first_seen_date=("trading_date", "min"), last_seen_date=("trading_date", "max"))
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
            "id_type": TBLTICKERHISTORY_ID_TYPE,
            "id_value": [
                _vendor_identifier_value(symbol, value)
                for symbol, value in zip(grouped["symbol"], grouped["vendor_security_id"], strict=True)
            ],
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


def disambiguate_vendor_collisions(store: DuckDBStore, source: str = SOURCE_NAME) -> int:
    """Split ``security_id``s that collapse multiple distinct tradeable lines.

    The symbol -> canonical-``security_id`` mapping (``security_ids_for_symbols``)
    is many-to-one across the broad universe: a ticker recycled across issuers
    (two unrelated companies that both traded as "ET") or split across share
    classes (LMCA / LMCAV under one vendor) resolves several distinct lines onto
    one ``security_id``, producing duplicate ``(security_id, trade_date)`` bars
    that corrupt cross-sectional stats and break the ``equity_price_metrics``
    primary key.

    The natural permanent line identity in this feed is
    ``(vendor_security_id, symbol)``. For each ``security_id`` that carries more
    than one such line, the line with the most bars keeps the canonical id (it is
    the surviving/primary issuer that carries any cross-surface links); every
    other line is re-keyed to a deterministic per-line synthetic id, and
    first-class ``securities`` / ``security_identifier_history`` /
    ``exchange_listings`` rows are materialized for it. A final safety-net pass
    collapses any residual exact ``(security_id, trade_date)`` duplicates (e.g.
    the same line emitted twice in the source), keeping the higher-volume row.

    Global and idempotent: once split, no ``security_id`` carries multiple lines,
    so a re-run is a no-op. Returns the number of lines re-keyed.
    """
    con = store.con
    lines = con.execute(
        """
        SELECT security_id,
               vendor_security_id,
               symbol,
               COUNT(*) AS n,
               MIN(trade_date) AS first_seen,
               MAX(trade_date) AS last_seen
        FROM equity_daily_bars
        WHERE source = ?
        GROUP BY security_id, vendor_security_id, symbol
        """,
        [source],
    ).df()
    rekeyed = 0
    if not lines.empty:
        line_counts = lines.groupby("security_id")["vendor_security_id"].transform("size")
        multi = lines[line_counts > 1].copy()
        # Within a multi-line security_id: most-bars line is primary (keeps id);
        # the rest are re-keyed. Deterministic tie-break on vendor id then symbol.
        multi = multi.sort_values(
            ["security_id", "n", "vendor_security_id", "symbol"],
            ascending=[True, False, True, True],
        )
        multi["rank"] = multi.groupby("security_id").cumcount()
        nonprimary = multi[multi["rank"] > 0].copy()
    else:
        nonprimary = lines

    if not nonprimary.empty:
        nonprimary["symbol_key"] = [symbol_key(sym) for sym in nonprimary["symbol"]]
        nonprimary["id_value"] = [
            _vendor_identifier_value(sk, vid)
            for sk, vid in zip(nonprimary["symbol_key"], nonprimary["vendor_security_id"])
        ]
        nonprimary["new_security_id"] = [
            vendor_security_id("tbltickerhistory", f"{vid}-{sk}")
            for vid, sk in zip(nonprimary["vendor_security_id"], nonprimary["symbol_key"])
        ]
        mapping = nonprimary[
            [
                "security_id",
                "vendor_security_id",
                "symbol",
                "symbol_key",
                "id_value",
                "new_security_id",
                "first_seen",
                "last_seen",
            ]
        ]
        rekeyed = int(len(mapping))
        store.con.register("vendor_collision_map", mapping)
        try:
            with store.transaction():
                con.execute(
                    """
                    UPDATE equity_daily_bars AS b
                    SET security_id = m.new_security_id
                    FROM vendor_collision_map m
                    WHERE b.source = ?
                      AND b.security_id = m.security_id
                      AND b.vendor_security_id = m.vendor_security_id
                      AND b.symbol = m.symbol
                    """,
                    [source],
                )
                raw_exists = con.execute(
                    "SELECT COUNT(*) FROM duckdb_tables() "
                    "WHERE schema_name = 'main' AND table_name = 'tbltickerhistory_daily'"
                ).fetchone()[0]
                if raw_exists:
                    con.execute(
                        """
                        UPDATE tbltickerhistory_daily AS r
                        SET security_id = m.new_security_id
                        FROM vendor_collision_map m
                        WHERE r.source = ?
                          AND r.security_id = m.security_id
                          AND r.vendor_security_id = m.vendor_security_id
                          AND COALESCE(r.today_ticker, r.ticker_tk) = m.symbol
                        """,
                        [source],
                    )
                con.execute(
                    """
                    INSERT INTO securities (
                        security_id, issuer_id, primary_symbol, name, asset_class,
                        country, currency, active, first_seen_date, last_seen_date, source
                    )
                    SELECT new_security_id, NULL, any_value(symbol_key), any_value(symbol_key),
                           'EQUITY', 'US', 'USD', TRUE, min(first_seen), max(last_seen), ?
                    FROM vendor_collision_map m
                    WHERE NOT EXISTS (
                        SELECT 1 FROM securities s WHERE s.security_id = m.new_security_id
                    )
                    GROUP BY new_security_id
                    """,
                    [source],
                )
                con.execute(
                    """
                    INSERT INTO security_identifier_history (
                        security_id, id_type, id_value, valid_from, valid_to,
                        as_of_date, available_at, source, run_id
                    )
                    SELECT new_security_id, ?, any_value(id_value), min(first_seen), NULL,
                           min(first_seen), min(first_seen) + INTERVAL 22 HOUR, ?, NULL
                    FROM vendor_collision_map m
                    WHERE NOT EXISTS (
                        SELECT 1 FROM security_identifier_history h
                        WHERE h.security_id = m.new_security_id
                          AND h.id_type = ?
                          AND h.source = ?
                    )
                    GROUP BY new_security_id
                    """,
                    [TBLTICKERHISTORY_ID_TYPE, source, TBLTICKERHISTORY_ID_TYPE, source],
                )
                con.execute(
                    """
                    INSERT INTO exchange_listings (
                        security_id, ticker, exchange_code, mic, currency,
                        valid_from, valid_to, as_of_date, available_at, source, run_id
                    )
                    SELECT new_security_id, any_value(symbol_key), NULL, NULL, 'USD',
                           min(first_seen), NULL, min(first_seen),
                           min(first_seen) + INTERVAL 22 HOUR, ?, NULL
                    FROM vendor_collision_map m
                    WHERE NOT EXISTS (
                        SELECT 1 FROM exchange_listings e
                        WHERE e.security_id = m.new_security_id AND e.source = ?
                    )
                    GROUP BY new_security_id
                    """,
                    [source, source],
                )
        finally:
            store.con.unregister("vendor_collision_map")

    # Safety net: drop any residual exact (security_id, trade_date) duplicates
    # (e.g. an identical line emitted twice in the source), keeping the
    # higher-volume row. Distinct real lines now carry distinct ids, so this only
    # removes redundant rows, never a real security.
    with store.transaction():
        con.execute(
            """
            DELETE FROM equity_daily_bars
            WHERE source = ?
              AND rowid IN (
                  SELECT rowid FROM (
                      SELECT rowid,
                             row_number() OVER (
                                 PARTITION BY security_id, trade_date
                                 ORDER BY volume DESC NULLS LAST, vendor_security_id
                             ) AS rn
                      FROM equity_daily_bars
                      WHERE source = ?
                  )
                  WHERE rn > 1
              )
            """,
            [source, source],
        )
    return rekeyed


class TickerHistoryDataset(Dataset):
    dataset_id = "tbltickerhistory_daily"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.con.execute(
            f"""
            CREATE TABLE IF NOT EXISTS tbltickerhistory_daily (
                {_raw_table_columns_sql()}
            )
            """
        )
        store.con.execute(
            "CREATE INDEX IF NOT EXISTS idx_tbltickerhistory_daily_symbol_date ON tbltickerhistory_daily(today_ticker, trading_date)"
        )
        store.con.execute(
            "CREATE INDEX IF NOT EXISTS idx_tbltickerhistory_daily_security_date ON tbltickerhistory_daily(security_id, trading_date)"
        )
        store.con.execute(
            """
            CREATE OR REPLACE VIEW tbltickerhistory AS
            SELECT *
            FROM tbltickerhistory_daily
            """
        )

    def load(self, store: DuckDBStore, options: TickerHistoryOptions) -> DatasetLoadResult:
        if not options.zip_path.exists():
            raise FileNotFoundError(options.zip_path)
        effective_options = TickerHistoryOptions(
            zip_path=options.zip_path,
            symbols=options.symbols,
            start_date=options.start_date,
            end_date=options.end_date,
            chunk_size=options.chunk_size,
            max_chunks=options.max_chunks,
            source=options.source,
            compute_source_hash=options.compute_source_hash,
            run_id=options.run_id,
        )
        record_source_file(
            store,
            dataset_id=self.dataset_id,
            source_url=str(effective_options.zip_path),
            cache_path=effective_options.zip_path,
            status="available",
            metadata={"symbols": effective_options.symbols, "max_chunks": effective_options.max_chunks},
            compute_hash=effective_options.compute_source_hash,
        )

        total_rows = 0
        chunks_seen = 0
        chunks_with_matches = 0
        matched_symbols: set[str] = set()
        min_trading_date: dt.date | None = None
        max_trading_date: dt.date | None = None
        for raw_chunk in _iter_raw_chunks(effective_options):
            chunks_seen += 1
            filtered = _filter_chunk(raw_chunk, effective_options)
            if filtered.empty:
                continue
            chunks_with_matches += 1
            normalized = _normalize_chunk(filtered, effective_options)
            normalized = _apply_security_ids(store, normalized, effective_options)
            matched_symbols.update(
                normalized["today_ticker"].fillna(normalized["ticker_tk"]).dropna().map(symbol_key).tolist()
            )
            chunk_min = normalized["trading_date"].min()
            chunk_max = normalized["trading_date"].max()
            min_trading_date = chunk_min if min_trading_date is None else min(min_trading_date, chunk_min)
            max_trading_date = chunk_max if max_trading_date is None else max(max_trading_date, chunk_max)
            total_rows += self._load_chunk(store, normalized, effective_options)

        # Recycled-ticker / share-class collisions only surface across the broad
        # universe (the per-symbol resolver is many-to-one), so resolve them once
        # globally after all chunks land. Idempotent.
        rekeyed = disambiguate_vendor_collisions(store, effective_options.source)

        details = {
            "chunks_seen": chunks_seen,
            "chunks_with_matches": chunks_with_matches,
            "symbols": effective_options.symbols,
            "matched_symbols": sorted(matched_symbols),
            "matched_symbol_count": len(matched_symbols),
            "min_trading_date": min_trading_date,
            "max_trading_date": max_trading_date,
            "max_chunks": effective_options.max_chunks,
            "chunk_size": effective_options.chunk_size,
            "vendor_collisions_rekeyed": rekeyed,
        }
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="tbltickerhistory_daily",
            check_name="rows_loaded",
            status="passed" if total_rows > 0 else "warning",
            observed_value=float(total_rows),
            threshold_value=1.0,
            details=details,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=total_rows,
            source=str(effective_options.zip_path),
            details=details,
        )

    def _load_chunk(self, store: DuckDBStore, frame: pd.DataFrame, options: TickerHistoryOptions) -> int:
        raw_columns = ["source", "security_id", "run_id"] + [RENAMES[column] for column in SOURCE_COLUMNS] + ["available_at"]
        raw = frame.copy()
        raw["available_at"] = pd.to_datetime(raw["trading_date"]) + pd.Timedelta(hours=22)
        raw = raw[raw_columns]
        bars = _canonical_bars(frame, options)
        securities, identifiers, listings = _security_links(frame, options)

        store.con.register("tbltickerhistory_load", raw)
        store.con.register("equity_daily_bars_load", bars)
        try:
            with store.transaction():
                store.con.execute(
                    """
                    DELETE FROM tbltickerhistory_daily AS dst
                    USING tbltickerhistory_load AS src
                    WHERE dst.source = src.source
                      AND dst.vendor_security_id = src.vendor_security_id
                      AND dst.trading_date = src.trading_date
                    """
                )
                insert_frame(store, raw, "tbltickerhistory_daily", "tbltickerhistory_insert")
                store.con.execute(
                    """
                    DELETE FROM equity_daily_bars AS dst
                    USING equity_daily_bars_load AS src
                    WHERE dst.source = src.source
                      AND dst.security_id = src.security_id
                      AND dst.trade_date = src.trade_date
                    """
                )
                insert_frame(store, bars, "equity_daily_bars", "equity_daily_bars_insert")
                self._upsert_links(store, securities, identifiers, listings)
        finally:
            for relation in ("tbltickerhistory_load", "equity_daily_bars_load"):
                try:
                    store.con.unregister(relation)
                except Exception:
                    pass
        return int(len(raw))

    def _upsert_links(
        self,
        store: DuckDBStore,
        securities: pd.DataFrame,
        identifiers: pd.DataFrame,
        listings: pd.DataFrame,
    ) -> None:
        if not securities.empty:
            store.con.register("ticker_securities_load", securities)
            try:
                store.con.execute(
                    """
                    DELETE FROM securities
                    USING ticker_securities_load src
                    WHERE securities.security_id = src.security_id
                      AND securities.source = src.source
                    """
                )
                store.con.execute(
                    """
                    INSERT INTO securities (
                        security_id,
                        issuer_id,
                        primary_symbol,
                        name,
                        asset_class,
                        country,
                        currency,
                        active,
                        first_seen_date,
                        last_seen_date,
                        source
                    )
                    SELECT
                        src.security_id,
                        src.issuer_id,
                        src.primary_symbol,
                        src.name,
                        src.asset_class,
                        src.country,
                        src.currency,
                        src.active,
                        src.first_seen_date,
                        src.last_seen_date,
                        src.source
                    FROM ticker_securities_load src
                    WHERE NOT EXISTS (
                        SELECT 1
                        FROM securities dst
                        WHERE dst.security_id = src.security_id
                    )
                    """
                )
            finally:
                store.con.unregister("ticker_securities_load")
        if not identifiers.empty:
            store.con.register("ticker_identifiers_load", identifiers)
            try:
                store.con.execute(
                    """
                    CREATE TEMPORARY TABLE ticker_identifier_legacy_zero_cleanup AS
                    SELECT DISTINCT security_id, source
                    FROM ticker_identifiers_load
                    WHERE id_type = ?
                      AND id_value LIKE 'SYMBOL-%-VENDOR-0'
                    """,
                    [TBLTICKERHISTORY_ID_TYPE],
                )
                store.con.execute(
                    """
                    DELETE FROM security_identifier_history
                    USING ticker_identifier_legacy_zero_cleanup src
                    WHERE security_identifier_history.security_id = src.security_id
                      AND security_identifier_history.source = src.source
                      AND security_identifier_history.id_type = ?
                      AND security_identifier_history.id_value = '0'
                    """,
                    [TBLTICKERHISTORY_ID_TYPE],
                )
                store.con.execute(
                    """
                    CREATE TEMPORARY TABLE ticker_identifiers_merged AS
                    WITH keys AS (
                        SELECT DISTINCT security_id, id_type, id_value, source
                        FROM ticker_identifiers_load
                    ),
                    unioned AS (
                        SELECT
                            h.security_id,
                            h.id_type,
                            h.id_value,
                            h.valid_from,
                            h.valid_to,
                            h.as_of_date,
                            h.available_at,
                            h.source,
                            h.run_id
                        FROM security_identifier_history h
                        JOIN keys k
                          ON k.security_id = h.security_id
                         AND k.id_type = h.id_type
                         AND k.id_value = h.id_value
                         AND k.source = h.source
                        UNION ALL
                        SELECT
                            security_id,
                            id_type,
                            id_value,
                            valid_from,
                            valid_to,
                            as_of_date,
                            available_at,
                            source,
                            run_id
                        FROM ticker_identifiers_load
                    )
                    SELECT
                        security_id,
                        id_type,
                        id_value,
                        min(valid_from) AS valid_from,
                        CASE
                            WHEN bool_or(valid_to IS NULL) THEN NULL
                            ELSE max(valid_to)
                        END AS valid_to,
                        min(as_of_date) AS as_of_date,
                        min(available_at) AS available_at,
                        source,
                        any_value(run_id) AS run_id
                    FROM unioned
                    GROUP BY security_id, id_type, id_value, source
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM security_identifier_history
                    USING ticker_identifiers_load src
                    WHERE security_identifier_history.security_id = src.security_id
                      AND security_identifier_history.id_type = src.id_type
                      AND security_identifier_history.id_value = src.id_value
                      AND security_identifier_history.source = src.source
                    """
                )
                store.con.execute(
                    """
                    INSERT INTO security_identifier_history (
                        security_id,
                        id_type,
                        id_value,
                        valid_from,
                        valid_to,
                        as_of_date,
                        available_at,
                        source,
                        run_id
                    )
                    SELECT
                        security_id,
                        id_type,
                        id_value,
                        valid_from,
                        valid_to,
                        as_of_date,
                        available_at,
                        source,
                        run_id
                    FROM ticker_identifiers_merged
                    """
                )
            finally:
                try:
                    store.con.execute("DROP TABLE IF EXISTS ticker_identifiers_merged")
                except Exception:
                    pass
                try:
                    store.con.execute("DROP TABLE IF EXISTS ticker_identifier_legacy_zero_cleanup")
                except Exception:
                    pass
                store.con.unregister("ticker_identifiers_load")
        if not listings.empty:
            store.con.register("ticker_listings_load", listings)
            try:
                store.con.execute(
                    """
                    CREATE TEMPORARY TABLE ticker_listings_merged AS
                    WITH keys AS (
                        SELECT DISTINCT
                            security_id,
                            ticker,
                            coalesce(exchange_code, '') AS exchange_code_key,
                            source
                        FROM ticker_listings_load
                    ),
                    unioned AS (
                        SELECT
                            l.security_id,
                            l.ticker,
                            l.exchange_code,
                            l.mic,
                            l.currency,
                            l.valid_from,
                            l.valid_to,
                            l.as_of_date,
                            l.available_at,
                            l.source,
                            l.run_id
                        FROM exchange_listings l
                        JOIN keys k
                          ON k.security_id = l.security_id
                         AND k.ticker = l.ticker
                         AND k.exchange_code_key = coalesce(l.exchange_code, '')
                         AND k.source = l.source
                        UNION ALL
                        SELECT
                            security_id,
                            ticker,
                            exchange_code,
                            mic,
                            currency,
                            valid_from,
                            valid_to,
                            as_of_date,
                            available_at,
                            source,
                            run_id
                        FROM ticker_listings_load
                    )
                    SELECT
                        security_id,
                        ticker,
                        any_value(exchange_code) AS exchange_code,
                        any_value(mic) AS mic,
                        any_value(currency) AS currency,
                        min(valid_from) AS valid_from,
                        CASE
                            WHEN bool_or(valid_to IS NULL) THEN NULL
                            ELSE max(valid_to)
                        END AS valid_to,
                        min(as_of_date) AS as_of_date,
                        min(available_at) AS available_at,
                        source,
                        any_value(run_id) AS run_id
                    FROM unioned
                    GROUP BY security_id, ticker, coalesce(exchange_code, ''), source
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM exchange_listings
                    USING ticker_listings_load src
                    WHERE exchange_listings.security_id = src.security_id
                      AND exchange_listings.ticker = src.ticker
                      AND coalesce(exchange_listings.exchange_code, '') = coalesce(src.exchange_code, '')
                      AND exchange_listings.source = src.source
                    """
                )
                store.con.execute(
                    """
                    INSERT INTO exchange_listings (
                        security_id,
                        ticker,
                        exchange_code,
                        mic,
                        currency,
                        valid_from,
                        valid_to,
                        as_of_date,
                        available_at,
                        source,
                        run_id
                    )
                    SELECT
                        security_id,
                        ticker,
                        exchange_code,
                        mic,
                        currency,
                        valid_from,
                        valid_to,
                        as_of_date,
                        available_at,
                        source,
                        run_id
                    FROM ticker_listings_merged
                    """
                )
            finally:
                try:
                    store.con.execute("DROP TABLE IF EXISTS ticker_listings_merged")
                except Exception:
                    pass
                store.con.unregister("ticker_listings_load")
