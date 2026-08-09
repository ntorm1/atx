from __future__ import annotations

import datetime as dt
import os
import re
import zipfile
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd
import requests

from .connection import DuckDBStore, resolve_data_dir
from .dataset import Dataset, DatasetLoadResult
from .warehouse import cik_security_id, insert_frame, record_source_file, security_id_for_cusip

SEC_13F_DATASETS_PAGE = "https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets"
SEC_USER_AGENT = os.getenv(
    "ATX_SEC_USER_AGENT",
    "atx-db/0.2 SEC filings pipeline nathan.tormaschy@gmail.com",
)
AAPL_CUSIP = "037833100"
AAPL_CIK = "0000320193"
CUSIP_FALLBACK_SOURCE = "SEC 13F CUSIP fallback security seed"


@dataclass(frozen=True)
class ThirteenFOptions:
    dataset_url: str | None = None
    cache_dir: Path = field(default_factory=lambda: resolve_data_dir() / "cache")
    cusips: tuple[str, ...] | None = (AAPL_CUSIP,)
    chunk_size: int = 200_000
    request_timeout: int = 180
    user_agent: str = SEC_USER_AGENT
    compute_source_hash: bool = False
    run_id: str | None = None


def normalize_cusip(value: str) -> str:
    return value.replace(" ", "").replace("-", "").upper()


def requests_session(user_agent: str) -> requests.Session:
    session = requests.Session()
    session.headers.update(
        {
            "User-Agent": user_agent,
            "Accept-Encoding": "gzip, deflate",
            "Accept": "text/html,application/zip,application/octet-stream,*/*",
        }
    )
    return session


def discover_latest_dataset_url(session: requests.Session, timeout: int) -> str:
    response = session.get(SEC_13F_DATASETS_PAGE, timeout=timeout)
    response.raise_for_status()
    urls = sorted(
        set(re.findall(r"https://www\.sec\.gov/files/structureddata/data/form-13f-data-sets/[^\"']+?_form13f\.zip", response.text))
    )
    if not urls:
        hrefs = re.findall(r"href=[\"']([^\"']+?_form13f\.zip)[\"']", response.text)
        urls = [
            href if href.startswith("http") else f"https://www.sec.gov{href}"
            for href in hrefs
        ]
    if not urls:
        raise RuntimeError("Could not discover a 13F ZIP URL from SEC data sets page")

    def sort_key(url: str) -> tuple[int, int, int, str]:
        name = Path(url).name
        match = re.search(r"(\d{2})([a-z]{3})(\d{4})-(\d{2})([a-z]{3})(\d{4})", name.lower())
        if not match:
            return (0, 0, 0, name)
        day = int(match.group(4))
        month = {
            "jan": 1,
            "feb": 2,
            "mar": 3,
            "apr": 4,
            "may": 5,
            "jun": 6,
            "jul": 7,
            "aug": 8,
            "sep": 9,
            "oct": 10,
            "nov": 11,
            "dec": 12,
        }[match.group(5)]
        year = int(match.group(6))
        return (year, month, day, name)

    return sorted(urls, key=sort_key)[-1]


def download_dataset_zip(
    session: requests.Session,
    url: str,
    cache_dir: Path,
    timeout: int,
) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    destination = cache_dir / Path(url).name
    if destination.exists() and destination.stat().st_size > 0:
        return destination

    tmp = destination.with_suffix(destination.suffix + ".tmp")
    with session.get(url, timeout=timeout, stream=True) as response:
        response.raise_for_status()
        with tmp.open("wb") as handle:
            for chunk in response.iter_content(chunk_size=1024 * 1024):
                if chunk:
                    handle.write(chunk)
    tmp.replace(destination)
    return destination


def source_period_from_path(path: Path) -> str:
    return path.stem


def read_tsv_member(zip_path: Path, member: str) -> pd.DataFrame:
    with zipfile.ZipFile(zip_path) as archive, archive.open(member) as handle:
        return pd.read_csv(handle, sep="\t", dtype=str, keep_default_na=False)


def date_series(values: pd.Series) -> pd.Series:
    cleaned = values.replace("", pd.NA)
    parsed = pd.to_datetime(cleaned, format="%d-%b-%Y", errors="coerce")
    missing = parsed.isna() & cleaned.notna()
    if missing.any():
        parsed.loc[missing] = pd.to_datetime(cleaned.loc[missing], errors="coerce")
    return parsed.dt.date


def int_series(values: pd.Series) -> pd.Series:
    return pd.to_numeric(values.replace("", pd.NA), errors="coerce").astype("Int64")


def float_series(values: pd.Series) -> pd.Series:
    return pd.to_numeric(values.replace("", pd.NA), errors="coerce")


# SEC Form 13F amended rules require the information-table VALUE column to be
# reported in whole dollars for filings whose periodOfReport is on/after
# 2023-01-01 (effective for filings made on/after 2023-01-03). For earlier
# periods VALUE is reported in *thousands* of dollars. Normalizing everything to
# whole dollars avoids a silent 1000x understatement when SUMming mixed-vintage
# holdings — the classic 13F backfill bug. Current loaded periods are all
# post-cutover (multiplier 1), so this is a no-op for existing rows.
VALUE_UNIT_CUTOVER_PERIOD = dt.date(2022, 12, 31)


def value_unit_multiplier(period_of_report: object) -> int:
    """Return the VALUE->whole-dollars multiplier for a filing period.

    1000 when ``period_of_report`` is on/before 2022-12-31 (VALUE was in
    thousands), 1 afterward, and 1 when the period is missing/unparseable.
    """
    if period_of_report is None:
        return 1
    try:
        if pd.isna(period_of_report):
            return 1
    except (TypeError, ValueError):
        pass
    timestamp = pd.Timestamp(period_of_report)
    if pd.isna(timestamp):
        return 1
    return 1000 if timestamp <= pd.Timestamp(VALUE_UNIT_CUTOVER_PERIOD) else 1


def apply_value_unit_cutover(
    frame: pd.DataFrame,
    accession_periods: dict[str, dt.date],
) -> pd.DataFrame:
    """Scale ``value_usd`` to whole dollars using each holding's filing period.

    ``accession_periods`` maps accession_number -> period_of_report. Holdings
    whose accession is absent from the map are left unscaled (safe default).
    """
    if frame.empty or "value_usd" not in frame.columns or "accession_number" not in frame.columns:
        return frame
    multipliers = frame["accession_number"].map(
        lambda accession: value_unit_multiplier(accession_periods.get(accession))
    )
    scaled = frame.copy()
    scaled["value_usd"] = scaled["value_usd"] * multipliers.astype("int64")
    return scaled


def normalize_submission(frame: pd.DataFrame, source_period: str) -> pd.DataFrame:
    return pd.DataFrame(
        {
            "accession_number": frame["ACCESSION_NUMBER"].str.strip(),
            "filing_date": date_series(frame["FILING_DATE"]),
            "submission_type": frame["SUBMISSIONTYPE"].str.strip(),
            "cik": frame["CIK"].str.strip(),
            "period_of_report": date_series(frame["PERIODOFREPORT"]),
            "source_period": source_period,
        }
    )


def normalize_cover_page(frame: pd.DataFrame, source_period: str) -> pd.DataFrame:
    return pd.DataFrame(
        {
            "accession_number": frame["ACCESSION_NUMBER"].str.strip(),
            "report_calendar_or_quarter": date_series(frame["REPORTCALENDARORQUARTER"]),
            "is_amendment": frame["ISAMENDMENT"].str.strip(),
            "amendment_no": frame["AMENDMENTNO"].str.strip(),
            "amendment_type": frame["AMENDMENTTYPE"].str.strip(),
            "filing_manager_name": frame["FILINGMANAGER_NAME"].str.strip(),
            "filing_manager_city": frame["FILINGMANAGER_CITY"].str.strip(),
            "filing_manager_state_or_country": frame["FILINGMANAGER_STATEORCOUNTRY"].str.strip(),
            "report_type": frame["REPORTTYPE"].str.strip(),
            "form_13f_file_number": frame["FORM13FFILENUMBER"].str.strip(),
            "crd_number": frame["CRDNUMBER"].str.strip(),
            "sec_file_number": frame["SECFILENUMBER"].str.strip(),
            "source_period": source_period,
        }
    )


def normalize_summary_page(frame: pd.DataFrame, source_period: str) -> pd.DataFrame:
    return pd.DataFrame(
        {
            "accession_number": frame["ACCESSION_NUMBER"].str.strip(),
            "other_included_managers_count": int_series(frame["OTHERINCLUDEDMANAGERSCOUNT"]),
            "table_entry_total": int_series(frame["TABLEENTRYTOTAL"]),
            "table_value_total": float_series(frame["TABLEVALUETOTAL"]),
            "is_confidential_omitted": frame["ISCONFIDENTIALOMITTED"].str.strip(),
            "source_period": source_period,
        }
    )


def normalize_holdings(frame: pd.DataFrame, source_period: str) -> pd.DataFrame:
    return pd.DataFrame(
        {
            "accession_number": frame["ACCESSION_NUMBER"].str.strip(),
            "security_id": pd.NA,
            "infotable_sk": int_series(frame["INFOTABLE_SK"]),
            "name_of_issuer": frame["NAMEOFISSUER"].str.strip(),
            "title_of_class": frame["TITLEOFCLASS"].str.strip(),
            "cusip": frame["CUSIP"].map(normalize_cusip),
            "figi": frame["FIGI"].str.strip(),
            "value_usd": float_series(frame["VALUE"]),
            "share_quantity": float_series(frame["SSHPRNAMT"]),
            "share_quantity_type": frame["SSHPRNAMTTYPE"].str.strip(),
            "put_call": frame["PUTCALL"].str.strip().str.upper(),
            "investment_discretion": frame["INVESTMENTDISCRETION"].str.strip(),
            "other_manager": frame["OTHERMANAGER"].str.strip(),
            "voting_auth_sole": float_series(frame["VOTING_AUTH_SOLE"]),
            "voting_auth_shared": float_series(frame["VOTING_AUTH_SHARED"]),
            "voting_auth_none": float_series(frame["VOTING_AUTH_NONE"]),
            "source_period": source_period,
            "run_id": pd.NA,
        }
    )


def filter_cusips(frame: pd.DataFrame, cusips: Iterable[str] | None) -> pd.DataFrame:
    if cusips is None:
        return frame
    normalized = {normalize_cusip(cusip) for cusip in cusips}
    return frame[frame["CUSIP"].map(normalize_cusip).isin(normalized)].reset_index(drop=True)


class ThirteenFDataSet(Dataset):
    dataset_id = "sec_13f"
    source_name = "SEC Form 13F Data Sets"

    def ensure_schema(self, store: DuckDBStore) -> None:
        con = store.con
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS thirteenf_submissions (
                accession_number VARCHAR NOT NULL,
                filing_date DATE,
                submission_type VARCHAR,
                cik VARCHAR,
                period_of_report DATE,
                source_period VARCHAR NOT NULL,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS thirteenf_cover_pages (
                accession_number VARCHAR NOT NULL,
                report_calendar_or_quarter DATE,
                is_amendment VARCHAR,
                amendment_no VARCHAR,
                amendment_type VARCHAR,
                filing_manager_name VARCHAR,
                filing_manager_city VARCHAR,
                filing_manager_state_or_country VARCHAR,
                report_type VARCHAR,
                form_13f_file_number VARCHAR,
                crd_number VARCHAR,
                sec_file_number VARCHAR,
                source_period VARCHAR NOT NULL,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS thirteenf_summary_pages (
                accession_number VARCHAR NOT NULL,
                other_included_managers_count BIGINT,
                table_entry_total BIGINT,
                table_value_total DOUBLE,
                is_confidential_omitted VARCHAR,
                source_period VARCHAR NOT NULL,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS thirteenf_holdings (
                accession_number VARCHAR NOT NULL,
                security_id VARCHAR,
                infotable_sk BIGINT,
                name_of_issuer VARCHAR,
                title_of_class VARCHAR,
                cusip VARCHAR NOT NULL,
                figi VARCHAR,
                value_usd DOUBLE,
                share_quantity DOUBLE,
                share_quantity_type VARCHAR,
                put_call VARCHAR,
                investment_discretion VARCHAR,
                other_manager VARCHAR,
                voting_auth_sole DOUBLE,
                voting_auth_shared DOUBLE,
                voting_auth_none DOUBLE,
                source_period VARCHAR NOT NULL,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
        )
        for statement in (
            "ALTER TABLE thirteenf_holdings ADD COLUMN IF NOT EXISTS security_id VARCHAR",
            "ALTER TABLE thirteenf_holdings ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        ):
            con.execute(statement)
        con.execute(
            "CREATE INDEX IF NOT EXISTS idx_thirteenf_holdings_cusip ON thirteenf_holdings(cusip)"
        )
        con.execute(
            "CREATE INDEX IF NOT EXISTS idx_thirteenf_submissions_accession ON thirteenf_submissions(accession_number)"
        )
        con.execute(
            """
            CREATE OR REPLACE VIEW v_thirteenf_positioning_by_security AS
            SELECT
                h.source_period,
                max(s.period_of_report) AS report_period,
                h.cusip,
                any_value(h.security_id) AS security_id,
                count(*) AS holding_rows,
                count(DISTINCT h.accession_number) AS filing_count,
                sum(
                    CASE
                        WHEN coalesce(h.put_call, '') = ''
                         AND upper(coalesce(h.share_quantity_type, '')) = 'SH'
                        THEN coalesce(h.share_quantity, 0)
                        ELSE 0
                    END
                ) AS total_common_share_quantity,
                sum(
                    CASE
                        WHEN coalesce(h.put_call, '') = ''
                         AND upper(coalesce(h.share_quantity_type, '')) = 'SH'
                        THEN coalesce(h.value_usd, 0)
                        ELSE 0
                    END
                ) AS total_common_value_usd,
                sum(CASE WHEN h.put_call = 'CALL' THEN coalesce(h.share_quantity, 0) ELSE 0 END) AS call_share_quantity,
                sum(CASE WHEN h.put_call = 'PUT' THEN coalesce(h.share_quantity, 0) ELSE 0 END) AS put_share_quantity
            FROM thirteenf_holdings h
            LEFT JOIN thirteenf_submissions s
              ON s.accession_number = h.accession_number
             AND s.source_period = h.source_period
            GROUP BY h.source_period, h.cusip
            """
        )

    def load(self, store: DuckDBStore, options: ThirteenFOptions) -> DatasetLoadResult:
        session = requests_session(options.user_agent)
        dataset_url = options.dataset_url or discover_latest_dataset_url(session, options.request_timeout)
        zip_path = download_dataset_zip(session, dataset_url, options.cache_dir, options.request_timeout)
        source_period = source_period_from_path(zip_path)

        with store.transaction():
            self._delete_source_period(store, source_period, options.cusips)
            submission_rows = self._load_member(
                store,
                normalize_submission(read_tsv_member(zip_path, "SUBMISSION.tsv"), source_period),
                "thirteenf_submissions",
            )
            cover_rows = self._load_member(
                store,
                normalize_cover_page(read_tsv_member(zip_path, "COVERPAGE.tsv"), source_period),
                "thirteenf_cover_pages",
            )
            summary_rows = self._load_member(
                store,
                normalize_summary_page(read_tsv_member(zip_path, "SUMMARYPAGE.tsv"), source_period),
                "thirteenf_summary_pages",
            )
            holding_rows = self._load_holdings(store, zip_path, source_period, options)
            self._upsert_aapl_identifier(store)
            self._upsert_watermark(store, source_period)
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=dataset_url,
                cache_path=zip_path,
                status="available",
                metadata={
                    "source_period": source_period,
                    "submission_rows": submission_rows,
                    "cover_page_rows": cover_rows,
                    "summary_page_rows": summary_rows,
                    "holding_rows": holding_rows,
                    "cusips": None
                    if options.cusips is None
                    else [normalize_cusip(cusip) for cusip in options.cusips],
                    "full_holdings": options.cusips is None,
                },
                compute_hash=options.compute_source_hash,
            )

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=holding_rows,
            source=dataset_url,
            details={
                "source_period": source_period,
                "zip_path": str(zip_path),
                "submission_rows": submission_rows,
                "cover_page_rows": cover_rows,
                "summary_page_rows": summary_rows,
                "holding_rows": holding_rows,
                "cusips": None if options.cusips is None else [normalize_cusip(cusip) for cusip in options.cusips],
            },
        )

    def _delete_source_period(
        self,
        store: DuckDBStore,
        source_period: str,
        cusips: Iterable[str] | None,
    ) -> None:
        normalized_cusips = None if cusips is None else [normalize_cusip(cusip) for cusip in cusips]
        if normalized_cusips is None:
            store.con.execute("DELETE FROM thirteenf_holdings WHERE source_period = ?", [source_period])
        elif normalized_cusips:
            frame = pd.DataFrame({"cusip": normalized_cusips})
            store.con.register("thirteenf_delete_cusips", frame)
            try:
                store.con.execute(
                    """
                    DELETE FROM thirteenf_holdings
                    USING thirteenf_delete_cusips src
                    WHERE thirteenf_holdings.source_period = ?
                      AND thirteenf_holdings.cusip = src.cusip
                    """,
                    [source_period],
                )
            finally:
                store.con.unregister("thirteenf_delete_cusips")
        for table in ("thirteenf_summary_pages", "thirteenf_cover_pages", "thirteenf_submissions"):
            store.con.execute(f"DELETE FROM {table} WHERE source_period = ?", [source_period])

    def _load_member(self, store: DuckDBStore, frame: pd.DataFrame, table: str) -> int:
        if frame.empty:
            return 0
        relation_name = f"{table}_load"
        store.con.register(relation_name, frame)
        try:
            columns = ", ".join(frame.columns)
            store.con.execute(f"INSERT INTO {table} ({columns}) SELECT {columns} FROM {relation_name}")
        finally:
            store.con.unregister(relation_name)
        return len(frame)

    def _load_holdings(
        self,
        store: DuckDBStore,
        zip_path: Path,
        source_period: str,
        options: ThirteenFOptions,
    ) -> int:
        rows = 0
        # Map each accession to its period_of_report so VALUE can be normalized
        # to whole dollars across the 2023-01-03 unit cutover (see
        # value_unit_multiplier). Submissions for this source_period were loaded
        # just before holdings in ThirteenFDataSet.load.
        accession_periods = {
            accession: period
            for accession, period in store.con.execute(
                "SELECT accession_number, period_of_report FROM thirteenf_submissions WHERE source_period = ?",
                [source_period],
            ).fetchall()
        }
        with zipfile.ZipFile(zip_path) as archive, archive.open("INFOTABLE.tsv") as handle:
            for raw_chunk in pd.read_csv(
                handle,
                sep="\t",
                dtype=str,
                keep_default_na=False,
                chunksize=options.chunk_size,
            ):
                filtered = filter_cusips(raw_chunk, options.cusips)
                if filtered.empty:
                    continue
                normalized = normalize_holdings(filtered, source_period)
                normalized = apply_value_unit_cutover(normalized, accession_periods)
                normalized = self._apply_security_ids(store, normalized, options)
                rows += self._load_member(store, normalized, "thirteenf_holdings")
        return rows

    def _apply_security_ids(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: ThirteenFOptions,
    ) -> pd.DataFrame:
        if frame.empty:
            return frame
        cusips = sorted(frame["cusip"].dropna().unique().tolist())
        if not cusips:
            frame["run_id"] = options.run_id
            return frame
        lookup = pd.DataFrame({"cusip": cusips})
        store.con.register("thirteenf_cusip_lookup", lookup)
        try:
            rows = store.con.execute(
                """
                SELECT l.cusip, i.security_id
                FROM thirteenf_cusip_lookup l
                LEFT JOIN security_identifier_history i
                  ON i.id_type = 'CUSIP'
                 AND i.id_value = l.cusip
                QUALIFY row_number() OVER (
                    PARTITION BY l.cusip
                    ORDER BY
                        CASE WHEN i.source = ? THEN 1 ELSE 0 END,
                        i.source_loaded_at DESC NULLS LAST
                ) = 1
                """
                ,
                [CUSIP_FALLBACK_SOURCE],
            ).fetchall()
        finally:
            store.con.unregister("thirteenf_cusip_lookup")
        mapping = {cusip: security_id for cusip, security_id in rows if security_id}
        fallback_mapping = self._upsert_cusip_fallbacks(store, frame, mapping, options)
        mapping.update(fallback_mapping)
        frame["security_id"] = frame["cusip"].map(lambda cusip: mapping.get(cusip))
        frame["run_id"] = options.run_id
        return frame

    def _upsert_cusip_fallbacks(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        existing_mapping: dict[str, str],
        options: ThirteenFOptions,
    ) -> dict[str, str]:
        unknown = frame[
            frame["cusip"].notna()
            & ~frame["cusip"].isin(existing_mapping)
        ][["cusip", "name_of_issuer", "title_of_class", "source_period", "accession_number"]].drop_duplicates()
        if unknown.empty:
            return {}

        store.con.register("thirteenf_unknown_cusips", unknown)
        try:
            fallback = store.con.execute(
                """
                SELECT
                    u.cusip,
                    any_value(nullif(u.name_of_issuer, '')) AS name,
                    any_value(nullif(u.title_of_class, '')) AS title_of_class,
                    min(coalesce(s.period_of_report, s.filing_date, current_date)) AS first_seen_date,
                    max(coalesce(s.period_of_report, s.filing_date, current_date)) AS last_seen_date,
                    min(coalesce(s.filing_date, s.period_of_report, current_date)) AS as_of_date,
                    min(coalesce(s.filing_date::TIMESTAMP + INTERVAL 22 HOURS, now()::TIMESTAMP)) AS available_at
                FROM thirteenf_unknown_cusips u
                LEFT JOIN thirteenf_submissions s
                  ON s.accession_number = u.accession_number
                 AND s.source_period = u.source_period
                GROUP BY u.cusip
                """
            ).df()
        finally:
            store.con.unregister("thirteenf_unknown_cusips")

        if fallback.empty:
            return {}

        fallback["security_id"] = fallback["cusip"].map(security_id_for_cusip)
        securities = pd.DataFrame(
            {
                "security_id": fallback["security_id"],
                "issuer_id": fallback["cusip"].map(lambda value: f"CUSIP6-{str(value)[:6]}" if len(str(value)) >= 6 else pd.NA),
                "primary_symbol": pd.NA,
                "name": fallback["name"].fillna(fallback["cusip"]),
                "asset_class": "EQUITY",
                "country": "US",
                "currency": "USD",
                "active": True,
                "first_seen_date": fallback["first_seen_date"],
                "last_seen_date": fallback["last_seen_date"],
                "source": CUSIP_FALLBACK_SOURCE,
            }
        ).drop_duplicates(subset=["security_id"])
        identifiers = pd.DataFrame(
            {
                "security_id": fallback["security_id"],
                "id_type": "CUSIP",
                "id_value": fallback["cusip"],
                "valid_from": fallback["first_seen_date"],
                "valid_to": pd.NaT,
                "as_of_date": fallback["as_of_date"],
                "available_at": fallback["available_at"],
                "source": CUSIP_FALLBACK_SOURCE,
                "run_id": options.run_id,
            }
        ).drop_duplicates(subset=["security_id", "id_type", "id_value"])

        store.con.register("thirteenf_fallback_securities", securities)
        store.con.register("thirteenf_fallback_identifiers", identifiers)
        try:
            store.con.execute(
                """
                DELETE FROM securities
                USING thirteenf_fallback_securities src
                WHERE securities.security_id = src.security_id
                  AND securities.source = ?
                """,
                [CUSIP_FALLBACK_SOURCE],
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
                FROM thirteenf_fallback_securities src
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM securities dst
                    WHERE dst.security_id = src.security_id
                )
                """
            )
            store.con.execute(
                """
                DELETE FROM security_identifier_history
                USING thirteenf_fallback_identifiers src
                WHERE security_identifier_history.security_id = src.security_id
                  AND security_identifier_history.id_type = src.id_type
                  AND security_identifier_history.id_value = src.id_value
                  AND security_identifier_history.source = ?
                """,
                [CUSIP_FALLBACK_SOURCE],
            )
            insert_frame(store, identifiers, "security_identifier_history", "thirteenf_fallback_identifiers_insert")
        finally:
            for relation in ("thirteenf_fallback_securities", "thirteenf_fallback_identifiers"):
                store.con.unregister(relation)

        return dict(zip(fallback["cusip"], fallback["security_id"], strict=True))

    def _upsert_aapl_identifier(self, store: DuckDBStore) -> None:
        security_id = cik_security_id(AAPL_CIK)
        store.con.execute(
            """
            INSERT INTO securities (
                security_id, issuer_id, primary_symbol, name, asset_class,
                country, currency, active, first_seen_date, source
            )
            SELECT ?, ?, 'AAPL', 'Apple Inc.', 'EQUITY', 'US', 'USD', true, DATE '1980-12-12', 'SEC 13F default identifier seed'
            WHERE NOT EXISTS (
                SELECT 1 FROM securities WHERE security_id = ?
            )
            """,
            [security_id, f"CIK-{AAPL_CIK}", security_id],
        )
        store.con.execute(
            """
            DELETE FROM security_identifiers
            WHERE symbol = 'AAPL' AND id_type = 'CUSIP' AND id_value = ?
            """,
            [AAPL_CUSIP],
        )
        store.con.execute(
            """
            DELETE FROM security_identifier_history
            WHERE security_id = ?
              AND id_type = 'CUSIP'
              AND id_value = ?
              AND source = 'SEC 13F default identifier seed'
            """,
            [security_id, AAPL_CUSIP],
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
            VALUES (?, 'CUSIP', ?, DATE '1980-12-12', NULL, current_date, now(), 'SEC 13F default identifier seed', NULL)
            """,
            [security_id, AAPL_CUSIP],
        )
        store.con.execute(
            """
            INSERT INTO security_identifiers (symbol, id_type, id_value, source, updated_at)
            VALUES ('AAPL', 'CUSIP', ?, 'SEC 13F default identifier seed', now())
            """,
            [AAPL_CUSIP],
        )

    def _upsert_watermark(self, store: DuckDBStore, source_period: str) -> None:
        store.con.execute(
            """
            DELETE FROM dataset_watermarks
            WHERE dataset_id = ? AND watermark_name = 'source_period'
            """,
            [self.dataset_id],
        )
        store.con.execute(
            """
            INSERT INTO dataset_watermarks (
                dataset_id, watermark_name, watermark_value, updated_at
            )
            VALUES (?, 'source_period', ?, now())
            """,
            [self.dataset_id, source_period],
        )
