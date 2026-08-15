from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from typing import Any, cast

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT, sec_session
from .warehouse import cik_security_id, insert_frame, quality_check, record_source_file, symbol_key

SOURCE_NAME = "SEC submissions API"
SUBMISSIONS_URL = "https://data.sec.gov/submissions/CIK{cik}.json"


@dataclass(frozen=True)
class SecSubmissionsOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    ciks: tuple[str, ...] = ()
    forms: tuple[str, ...] | None = ("10-K", "10-Q", "8-K")
    include_history_files: bool = False
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None


def _parse_date(value: Any) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def _parse_acceptance(value: Any) -> pd.Timestamp | None:
    if not value:
        return None
    parsed = pd.to_datetime(value, errors="coerce", utc=True)
    if pd.isna(parsed):
        return None
    return cast(pd.Timestamp, parsed.tz_convert(None))


def _columnar_filings(payload: dict[str, Any], key: str = "recent") -> pd.DataFrame:
    recent = payload.get("filings", {}).get(key, {})
    if not recent:
        return pd.DataFrame()
    keys = list(recent.keys())
    if not keys:
        return pd.DataFrame()
    length = len(recent[keys[0]])
    rows = [{column: recent[column][index] for column in keys} for index in range(length)]
    return pd.DataFrame(rows)


def _normalize(
    frame: pd.DataFrame,
    *,
    security_id: str,
    cik: str,
    source_url: str,
    run_id: str | None,
    forms: set[str] | None,
) -> pd.DataFrame:
    if frame.empty:
        return frame
    if forms is not None:
        frame = frame[frame["form"].isin(forms)].reset_index(drop=True)
    if frame.empty:
        return frame
    return pd.DataFrame(
        {
            "security_id": security_id,
            "cik": cik,
            "accession_number": frame["accessionNumber"].str.strip(),
            "filing_date": frame["filingDate"].map(_parse_date),
            "report_date": frame["reportDate"].map(_parse_date),
            "acceptance_datetime": frame["acceptanceDateTime"].map(_parse_acceptance),
            "form": frame["form"].str.strip(),
            "primary_document": frame["primaryDocument"].str.strip(),
            "primary_doc_description": frame["primaryDocDescription"].str.strip(),
            "file_number": frame.get("fileNumber", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "film_number": frame.get("filmNumber", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "items": frame.get("items", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "size": pd.to_numeric(frame.get("size", pd.Series([None] * len(frame))), errors="coerce").astype("Int64"),
            "is_xbrl": frame.get("isXBRL", pd.Series([None] * len(frame))).map(lambda value: None if value in (None, "") else bool(int(value))),
            "is_inline_xbrl": frame.get("isInlineXBRL", pd.Series([None] * len(frame))).map(lambda value: None if value in (None, "") else bool(int(value))),
            "act": frame.get("act", pd.Series([None] * len(frame))).astype("string").str.strip(),
            "source_url": source_url,
            "run_id": run_id,
        }
    )


def _normalized_cik(value: str | int) -> str:
    return f"{int(str(value).strip()):010d}"


def _targets(
    store: DuckDBStore,
    symbols: tuple[str, ...],
    ciks: tuple[str, ...] = (),
) -> list[tuple[str, str, str]]:
    frame = pd.DataFrame({"ticker": sorted({symbol_key(symbol) for symbol in symbols})})
    store.con.register("submission_symbol_lookup", frame)
    try:
        symbol_rows = store.con.execute(
            """
            SELECT l.ticker, t.cik, t.security_id
            FROM submission_symbol_lookup l
            JOIN sec_company_tickers t ON t.ticker = l.ticker
            QUALIFY row_number() OVER (
                PARTITION BY l.ticker
                ORDER BY t.source_loaded_at DESC, t.cik
            ) = 1
            """
        ).fetchall()
    finally:
        store.con.unregister("submission_symbol_lookup")

    cik_frame = pd.DataFrame({"cik": sorted({_normalized_cik(cik) for cik in ciks})})
    store.con.register("submission_cik_lookup", cik_frame)
    try:
        cik_rows = store.con.execute(
            """
            SELECT l.cik,t.ticker,t.security_id
            FROM submission_cik_lookup l
            LEFT JOIN sec_company_tickers t ON t.cik=l.cik
            QUALIFY row_number() OVER (
                PARTITION BY l.cik
                ORDER BY t.source_loaded_at DESC,t.ticker
            )=1
            """
        ).fetchall()
    finally:
        store.con.unregister("submission_cik_lookup")

    targets = {
        (cik, security_id): (ticker, cik, security_id)
        for ticker, cik, security_id in symbol_rows
    }
    for cik, ticker, security_id in cik_rows:
        resolved_security_id = security_id or cik_security_id(cik)
        targets[(cik, resolved_security_id)] = (
            ticker or f"CIK-{cik}",
            cik,
            resolved_security_id,
        )
    return sorted(targets.values(), key=lambda row: (row[0], row[1]))


class SecSubmissionsDataset(Dataset):
    dataset_id = "sec_submissions"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecSubmissionsOptions) -> DatasetLoadResult:
        session = sec_session(options.user_agent)
        forms = None if options.forms is None else set(options.forms)
        rows: list[pd.DataFrame] = []
        for symbol, cik, security_id in _targets(store, options.symbols, options.ciks):
            url = SUBMISSIONS_URL.format(cik=cik)
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            payload = response.json()
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={"symbol": symbol, "cik": cik},
            )
            rows.append(_normalize(_columnar_filings(payload), security_id=security_id, cik=cik, source_url=url, run_id=options.run_id, forms=forms))
            if options.include_history_files:
                for item in payload.get("filings", {}).get("files", []):
                    name = item.get("name")
                    if not name:
                        continue
                    history_url = f"https://data.sec.gov/submissions/{name}"
                    history_response = session.get(history_url, timeout=options.request_timeout)
                    history_response.raise_for_status()
                    rows.append(
                        _normalize(
                            _columnar_filings({"filings": {"recent": history_response.json()}}),
                            security_id=security_id,
                            cik=cik,
                            source_url=history_url,
                            run_id=options.run_id,
                            forms=forms,
                        )
                    )
        frame = pd.concat([frame for frame in rows if not frame.empty], ignore_index=True) if rows else pd.DataFrame()
        loaded = self._replace_rows(store, frame)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="sec_submissions",
            check_name="rows_loaded",
            status="passed" if loaded > 0 else "warning",
            observed_value=float(loaded),
            threshold_value=1.0,
            details={"symbols": options.symbols, "ciks": options.ciks, "forms": options.forms},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=loaded,
            source="SEC submissions API",
            run_id=options.run_id,
            details={"symbols": options.symbols, "ciks": options.ciks, "forms": options.forms},
        )

    def _replace_rows(self, store: DuckDBStore, frame: pd.DataFrame) -> int:
        if frame.empty:
            return 0
        with store.transaction():
            store.con.register("sec_submissions_load", frame)
            try:
                store.con.execute(
                    """
                    DELETE FROM sec_submissions AS dst
                    USING sec_submissions_load AS src
                    WHERE dst.security_id = src.security_id
                      AND dst.accession_number = src.accession_number
                    """
                )
                insert_frame(store, frame, "sec_submissions", "sec_submissions_insert")
            finally:
                store.con.unregister("sec_submissions_load")
        return len(frame)
