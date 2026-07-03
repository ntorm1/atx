#!/usr/bin/env python
"""Download FINRA consolidated short interest into Hive-partitioned Parquet."""

from __future__ import annotations

import argparse
import calendar
import csv
import datetime as dt
import io
import json
import logging
import os
import tempfile
import time
from dataclasses import asdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import requests
from selenium import webdriver
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.common.by import By


API_URL = "https://api.finra.org/data/group/otcMarket/name/consolidatedShortInterest"
DEFAULT_OUTPUT_ROOT = Path("data") / "short_interest"
DEFAULT_CHROME_BINARY = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
DEFAULT_EDGE_BINARY = Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")

RAW_COLUMNS = [
    "accountingYearMonthNumber",
    "symbolCode",
    "issueName",
    "issuerServicesGroupExchangeCode",
    "marketClassCode",
    "currentShortPositionQuantity",
    "previousShortPositionQuantity",
    "stockSplitFlag",
    "averageDailyVolumeQuantity",
    "daysToCoverQuantity",
    "revisionFlag",
    "changePercent",
    "changePreviousNumber",
    "settlementDate",
]

COLUMN_RENAMES = {
    "accountingYearMonthNumber": "accounting_year_month_number",
    "symbolCode": "symbol",
    "issueName": "issue_name",
    "issuerServicesGroupExchangeCode": "issuer_services_group_exchange_code",
    "marketClassCode": "market_class_code",
    "currentShortPositionQuantity": "current_short_position_quantity",
    "previousShortPositionQuantity": "previous_short_position_quantity",
    "stockSplitFlag": "stock_split_flag",
    "averageDailyVolumeQuantity": "average_daily_volume_quantity",
    "daysToCoverQuantity": "days_to_cover_quantity",
    "revisionFlag": "revision_flag",
    "changePercent": "change_percent",
    "changePreviousNumber": "change_previous_number",
    "settlementDate": "settlement_date",
}

OUTPUT_SCHEMA = pa.schema(
    [
        ("settlement_date", pa.date32()),
        ("accounting_year_month_number", pa.int32()),
        ("symbol", pa.string()),
        ("issue_name", pa.string()),
        ("issuer_services_group_exchange_code", pa.string()),
        ("market_class_code", pa.string()),
        ("current_short_position_quantity", pa.int64()),
        ("previous_short_position_quantity", pa.int64()),
        ("stock_split_flag", pa.string()),
        ("average_daily_volume_quantity", pa.int64()),
        ("days_to_cover_quantity", pa.float64()),
        ("revision_flag", pa.string()),
        ("change_percent", pa.float64()),
        ("change_previous_number", pa.int64()),
    ]
)


@dataclass(frozen=True)
class DateDownload:
    settlement_date: str
    record_total: int
    rows: int
    batches: int
    output_file: str
    skipped: bool = False


def parse_args() -> argparse.Namespace:
    default_start = subtract_years(dt.date.today(), 5)
    parser = argparse.ArgumentParser(
        description=(
            "Use headless Selenium to bootstrap FINRA's public API, then download "
            "consolidated short-interest records into Hive Parquet partitions."
        )
    )
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--start-date", default=default_start.isoformat())
    parser.add_argument("--end-date", default=dt.date.today().isoformat())
    parser.add_argument("--api-url", default=API_URL)
    parser.add_argument("--limit", type=int, default=5000, help="FINRA page size; max is 5000.")
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--compression", default="zstd", choices=["zstd", "snappy", "gzip", "brotli", "none"])
    parser.add_argument("--chrome-binary", type=Path, default=None)
    parser.add_argument("--selenium-timeout", type=int, default=90)
    parser.add_argument("--request-timeout", type=int, default=120)
    parser.add_argument("--max-retries", type=int, default=5)
    parser.add_argument("--retry-sleep", type=float, default=1.0)
    parser.add_argument("--limit-dates", type=int, help="Optional smoke-test cap on discovered settlement dates.")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    return parser.parse_args()


def subtract_years(value: dt.date, years: int) -> dt.date:
    try:
        return value.replace(year=value.year - years)
    except ValueError:
        return value.replace(year=value.year - years, day=28)


def parse_date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def month_iter(start_date: dt.date, end_date: dt.date) -> list[tuple[int, int]]:
    year = start_date.year
    month = start_date.month
    result: list[tuple[int, int]] = []
    while (year, month) <= (end_date.year, end_date.month):
        result.append((year, month))
        if month == 12:
            year += 1
            month = 1
        else:
            month += 1
    return result


def period_candidate_windows(start_date: dt.date, end_date: dt.date) -> list[list[dt.date]]:
    windows: list[list[dt.date]] = []
    for year, month in month_iter(start_date, end_date):
        _, last_day = calendar.monthrange(year, month)
        for anchor_day in (15, last_day):
            anchor = dt.date(year, month, anchor_day)
            window = [anchor - dt.timedelta(days=offset) for offset in range(8)]
            window = [value for value in window if start_date <= value <= end_date]
            if window:
                windows.append(window)
    return windows


def resolve_chrome_binary(explicit_path: Path | None) -> Path | None:
    candidates = [
        explicit_path,
        Path(os.environ["FINRA_CHROME_BINARY"]) if os.environ.get("FINRA_CHROME_BINARY") else None,
        DEFAULT_CHROME_BINARY,
        DEFAULT_EDGE_BINARY,
    ]
    for candidate in candidates:
        if candidate and candidate.exists():
            return candidate
    return None


def selenium_bootstrap_session(api_url: str, chrome_binary: Path | None, timeout: int) -> requests.Session:
    options = Options()
    if chrome_binary:
        options.binary_location = str(chrome_binary)
    options.add_argument("--headless=new")
    options.add_argument("--disable-gpu")
    options.add_argument("--no-sandbox")
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument("--window-size=1280,900")

    probe_url = f"{api_url}?limit=1"
    driver = webdriver.Chrome(options=options)
    try:
        driver.set_page_load_timeout(timeout)
        driver.get(probe_url)
        body_text = driver.find_element(By.TAG_NAME, "body").text
        if "accountingYearMonthNumber" not in body_text:
            raise RuntimeError(f"FINRA API Selenium probe returned unexpected body: {body_text[:500]}")
        user_agent = driver.execute_script("return navigator.userAgent")
        cookies = driver.get_cookies()
    finally:
        driver.quit()

    session = requests.Session()
    session.headers.update(
        {
            "Accept": "text/plain",
            "Content-Type": "application/json",
            "User-Agent": user_agent,
        }
    )
    for cookie in cookies:
        session.cookies.set(cookie["name"], cookie["value"], domain=cookie.get("domain"))
    return session


def post_finra(
    session: requests.Session,
    api_url: str,
    payload: dict[str, Any],
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> requests.Response:
    last_exc: Exception | None = None
    for attempt in range(1, max_retries + 1):
        try:
            response = session.post(api_url, json=payload, timeout=timeout)
            if response.status_code in (429, 500, 502, 503, 504):
                raise requests.HTTPError(f"retryable HTTP {response.status_code}", response=response)
            response.raise_for_status()
            return response
        except Exception as exc:  # noqa: BLE001 - retries should cover requests and HTTP errors.
            last_exc = exc
            if attempt == max_retries:
                break
            sleep_for = retry_sleep * (2 ** (attempt - 1))
            logging.warning("FINRA request failed on attempt %d/%d: %s; sleeping %.1fs", attempt, max_retries, exc, sleep_for)
            time.sleep(sleep_for)
    raise RuntimeError(f"FINRA request failed after {max_retries} attempts: {last_exc}") from last_exc


def date_payload(settlement_date: dt.date, limit: int, offset: int = 0) -> dict[str, Any]:
    return {
        "compareFilters": [
            {
                "compareType": "EQUAL",
                "fieldName": "settlementDate",
                "fieldValue": settlement_date.isoformat(),
            }
        ],
        "limit": limit,
        "offset": offset,
    }


def record_total_for_date(
    session: requests.Session,
    api_url: str,
    settlement_date: dt.date,
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> int:
    response = post_finra(
        session=session,
        api_url=api_url,
        payload=date_payload(settlement_date, limit=1),
        timeout=timeout,
        max_retries=max_retries,
        retry_sleep=retry_sleep,
    )
    return int(response.headers.get("record-total", "0"))


def discover_settlement_dates(
    session: requests.Session,
    api_url: str,
    start_date: dt.date,
    end_date: dt.date,
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> dict[dt.date, int]:
    discovered: dict[dt.date, int] = {}
    for window in period_candidate_windows(start_date, end_date):
        for candidate in window:
            if candidate in discovered:
                break
            total = record_total_for_date(
                session=session,
                api_url=api_url,
                settlement_date=candidate,
                timeout=timeout,
                max_retries=max_retries,
                retry_sleep=retry_sleep,
            )
            if total > 0:
                discovered[candidate] = total
                break
    return dict(sorted(discovered.items()))


def parse_csv_frame(text: str) -> pd.DataFrame:
    if not text.strip():
        return pd.DataFrame(columns=RAW_COLUMNS)
    frame = pd.read_csv(
        io.StringIO(text),
        dtype=str,
        keep_default_na=False,
        na_values=[],
        quoting=csv.QUOTE_MINIMAL,
    )
    return frame


def normalize_frame(frame: pd.DataFrame, settlement_date: dt.date) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame(columns=OUTPUT_SCHEMA.names)

    missing = [column for column in RAW_COLUMNS if column not in frame.columns]
    if missing:
        raise ValueError(f"FINRA CSV missing expected columns: {missing}")

    frame = frame[RAW_COLUMNS].rename(columns=COLUMN_RENAMES)
    frame["settlement_date"] = pd.to_datetime(frame["settlement_date"], errors="coerce").dt.date
    frame.loc[frame["settlement_date"].isna(), "settlement_date"] = settlement_date

    int_columns = [
        "accounting_year_month_number",
        "current_short_position_quantity",
        "previous_short_position_quantity",
        "average_daily_volume_quantity",
        "change_previous_number",
    ]
    for column in int_columns:
        frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce").astype("Int64")

    for column in ("days_to_cover_quantity", "change_percent"):
        frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")

    for column in ("symbol", "issue_name", "issuer_services_group_exchange_code", "market_class_code", "stock_split_flag", "revision_flag"):
        frame[column] = frame[column].replace("", pd.NA).astype("string")

    return frame[OUTPUT_SCHEMA.names]


def output_file_for_date(output_root: Path, settlement_date: dt.date) -> Path:
    return output_root / f"date={settlement_date.isoformat()}" / "part-00000.parquet"


def write_parquet_atomic(frame: pd.DataFrame, destination: Path, compression: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    codec = None if compression == "none" else compression
    table = pa.Table.from_pandas(frame, schema=OUTPUT_SCHEMA, preserve_index=False)
    with tempfile.TemporaryDirectory(prefix=".tmp-", dir=destination.parent) as tmp_dir:
        tmp_path = Path(tmp_dir) / destination.name
        pq.write_table(table, tmp_path, compression=codec, use_dictionary=True)
        os.replace(tmp_path, destination)


def download_date(
    session: requests.Session,
    api_url: str,
    output_root: Path,
    settlement_date: dt.date,
    record_total: int,
    limit: int,
    overwrite: bool,
    compression: str,
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> DateDownload:
    output_file = output_file_for_date(output_root, settlement_date)
    if output_file.exists() and output_file.stat().st_size > 0 and not overwrite:
        return DateDownload(
            settlement_date=settlement_date.isoformat(),
            record_total=record_total,
            rows=0,
            batches=0,
            output_file=str(output_file),
            skipped=True,
        )

    frames: list[pd.DataFrame] = []
    offset = 0
    batches = 0
    while offset < record_total:
        response = post_finra(
            session=session,
            api_url=api_url,
            payload=date_payload(settlement_date, limit=limit, offset=offset),
            timeout=timeout,
            max_retries=max_retries,
            retry_sleep=retry_sleep,
        )
        frame = parse_csv_frame(response.text)
        if frame.empty:
            break
        frames.append(frame)
        batches += 1
        offset += len(frame)

    if not frames:
        combined = pd.DataFrame(columns=RAW_COLUMNS)
    else:
        combined = pd.concat(frames, ignore_index=True)

    normalized = normalize_frame(combined, settlement_date)
    if len(normalized) != record_total:
        raise RuntimeError(
            f"{settlement_date}: downloaded {len(normalized)} rows, expected {record_total}"
        )

    write_parquet_atomic(normalized, output_file, compression)
    return DateDownload(
        settlement_date=settlement_date.isoformat(),
        record_total=record_total,
        rows=len(normalized),
        batches=batches,
        output_file=str(output_file),
    )


def write_manifest(
    output_root: Path,
    api_url: str,
    start_date: dt.date,
    end_date: dt.date,
    results: list[DateDownload],
    chrome_binary: Path | None,
) -> Path:
    built = [result for result in results if not result.skipped]
    skipped = [result for result in results if result.skipped]
    manifest = {
        "created_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source": "FINRA consolidatedShortInterest",
        "api_url": api_url,
        "selenium": {
            "used": True,
            "mode": "headless Chrome API bootstrap",
            "browser_binary": str(chrome_binary) if chrome_binary else None,
        },
        "output_root": str(output_root),
        "partitioning": "hive/date=YYYY-MM-DD with one part-00000.parquet per settlement date",
        "requested_start_date": start_date.isoformat(),
        "requested_end_date": end_date.isoformat(),
        "partitions": len(results),
        "built_partitions": len(built),
        "skipped_partitions": len(skipped),
        "rows_built": sum(result.rows for result in built),
        "rows_total_in_scope": sum(result.record_total for result in results),
        "min_settlement_date": min((result.settlement_date for result in results), default=None),
        "max_settlement_date": max((result.settlement_date for result in results), default=None),
        "schema": [field.name for field in OUTPUT_SCHEMA],
        "results": [asdict(result) for result in results],
    }
    metadata_root = output_root / "_metadata"
    metadata_root.mkdir(parents=True, exist_ok=True)
    manifest_path = metadata_root / "download_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    return manifest_path


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level), format="%(asctime)s %(levelname)s %(message)s")

    if args.limit < 1 or args.limit > 5000:
        raise ValueError("--limit must be between 1 and FINRA's max page size of 5000")

    start_date = parse_date(args.start_date)
    end_date = parse_date(args.end_date)
    if start_date > end_date:
        raise ValueError("--start-date must be on or before --end-date")

    output_root = args.output_root.resolve()
    chrome_binary = resolve_chrome_binary(args.chrome_binary)
    logging.info("output root: %s", output_root)
    logging.info("date range: %s through %s", start_date, end_date)
    logging.info("headless browser: %s", chrome_binary or "Selenium default")

    session = selenium_bootstrap_session(args.api_url, chrome_binary, args.selenium_timeout)
    logging.info("Selenium bootstrap succeeded")

    settlement_totals = discover_settlement_dates(
        session=session,
        api_url=args.api_url,
        start_date=start_date,
        end_date=end_date,
        timeout=args.request_timeout,
        max_retries=args.max_retries,
        retry_sleep=args.retry_sleep,
    )
    if args.limit_dates:
        settlement_totals = dict(list(settlement_totals.items())[: args.limit_dates])
    if not settlement_totals:
        raise RuntimeError(f"No FINRA short-interest settlement dates found from {start_date} through {end_date}")

    logging.info(
        "discovered %d settlement dates (%s through %s)",
        len(settlement_totals),
        min(settlement_totals),
        max(settlement_totals),
    )

    results: list[DateDownload] = []
    for index, (settlement_date, record_total) in enumerate(settlement_totals.items(), start=1):
        result = download_date(
            session=session,
            api_url=args.api_url,
            output_root=output_root,
            settlement_date=settlement_date,
            record_total=record_total,
            limit=args.limit,
            overwrite=args.overwrite,
            compression=args.compression,
            timeout=args.request_timeout,
            max_retries=args.max_retries,
            retry_sleep=args.retry_sleep,
        )
        results.append(result)
        status = "skipped" if result.skipped else f"wrote {result.rows} rows in {result.batches} batches"
        logging.info("%d/%d %s %s", index, len(settlement_totals), settlement_date, status)

    manifest_path = write_manifest(output_root, args.api_url, start_date, end_date, results, chrome_binary)
    print(
        f"downloaded {sum(1 for result in results if not result.skipped)} partitions "
        f"({sum(result.rows for result in results if not result.skipped)} rows built, "
        f"{sum(1 for result in results if result.skipped)} skipped) to {output_root}"
    )
    print(f"manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
