from __future__ import annotations

import datetime as dt
import hashlib
import uuid
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .features import refresh_feature_lineage
from .warehouse import insert_frame, json_dumps, now_utc_naive, quality_check


SOURCE_NAME = "atx-db SEC 13F ownership feature engine"
DEFAULT_OWNERSHIP_FEATURE_SET = "sec_13f_ownership_v1"


OWNERSHIP_FEATURE_DEFINITIONS = {
    "own_13f_filing_count": {
        "description": "Count of visible SEC Form 13F filings reporting the security for the report period.",
        "expression_sql": "thirteenf_security_ownership.filing_count",
        "lookback_days": 0,
    },
    "own_13f_holder_count": {
        "description": "Count of distinct visible SEC Form 13F managers reporting the security for the report period.",
        "expression_sql": "thirteenf_security_ownership.holder_count",
        "lookback_days": 0,
    },
    "own_13f_common_holder_count": {
        "description": "Count of managers reporting common-share rows for the security.",
        "expression_sql": "thirteenf_security_ownership.common_holder_count",
        "lookback_days": 0,
    },
    "own_13f_common_value_usd": {
        "description": "Aggregate reported 13F common-share market value in dollars for visible filings.",
        "expression_sql": "thirteenf_security_ownership.common_value_usd",
        "lookback_days": 0,
    },
    "own_13f_common_share_quantity": {
        "description": "Aggregate reported common-share quantity for visible 13F filings.",
        "expression_sql": "thirteenf_security_ownership.common_share_quantity",
        "lookback_days": 0,
    },
    "own_13f_avg_portfolio_weight": {
        "description": "Average manager portfolio weight among visible 13F common-share rows.",
        "expression_sql": "thirteenf_security_ownership.avg_portfolio_weight",
        "lookback_days": 0,
    },
    "own_13f_max_portfolio_weight": {
        "description": "Largest visible manager portfolio weight in the security for the report period.",
        "expression_sql": "thirteenf_security_ownership.max_portfolio_weight",
        "lookback_days": 0,
    },
    "own_13f_call_share_quantity": {
        "description": "Aggregate reported 13F call-option share-equivalent quantity.",
        "expression_sql": "thirteenf_security_ownership.call_share_quantity",
        "lookback_days": 0,
    },
    "own_13f_put_share_quantity": {
        "description": "Aggregate reported 13F put-option share-equivalent quantity.",
        "expression_sql": "thirteenf_security_ownership.put_share_quantity",
        "lookback_days": 0,
    },
    "own_13f_common_value_qoq_change": {
        "description": "Quarter-over-quarter change in aggregate visible 13F common-share value.",
        "expression_sql": "common_value_usd - lag(common_value_usd) by security ordered by report_period",
        "lookback_days": 95,
    },
    "own_13f_common_share_qoq_change": {
        "description": "Quarter-over-quarter change in aggregate visible 13F common-share quantity.",
        "expression_sql": "common_share_quantity - lag(common_share_quantity) by security ordered by report_period",
        "lookback_days": 95,
    },
    "own_13f_holder_count_qoq_change": {
        "description": "Quarter-over-quarter change in visible 13F manager holder count.",
        "expression_sql": "holder_count - lag(holder_count) by security ordered by report_period",
        "lookback_days": 95,
    },
}


FEATURE_COLUMNS = {
    "own_13f_filing_count": "filing_count",
    "own_13f_holder_count": "holder_count",
    "own_13f_common_holder_count": "common_holder_count",
    "own_13f_common_value_usd": "common_value_usd",
    "own_13f_common_share_quantity": "common_share_quantity",
    "own_13f_avg_portfolio_weight": "avg_portfolio_weight",
    "own_13f_max_portfolio_weight": "max_portfolio_weight",
    "own_13f_call_share_quantity": "call_share_quantity",
    "own_13f_put_share_quantity": "put_share_quantity",
    "own_13f_common_value_qoq_change": "common_value_usd_qoq_change",
    "own_13f_common_share_qoq_change": "common_share_quantity_qoq_change",
    "own_13f_holder_count_qoq_change": "holder_count_qoq_change",
}


@dataclass(frozen=True)
class OwnershipFeatureOptions:
    feature_set: str = DEFAULT_OWNERSHIP_FEATURE_SET
    source: str = SOURCE_NAME
    run_id: str | None = None


def manager_id_for_cik(cik: str) -> str:
    return f"SEC-13F-MANAGER-{int(cik):010d}"


def _uuid5_id(prefix: str, *parts: object) -> str:
    payload = "|".join([prefix, *(str(part) if part is not None else "" for part in parts)])
    return str(uuid.uuid5(uuid.NAMESPACE_URL, payload))


def _feature_hashes(features: pd.DataFrame) -> list[str]:
    hashes: list[str] = []
    for row in features.itertuples(index=False):
        value_text = "" if pd.isna(row.value) else f"{float(row.value):.17g}"
        payload = "|".join(
            [
                str(row.feature_set),
                str(row.feature_name),
                str(row.security_id),
                str(row.as_of_date),
                value_text,
            ]
        )
        hashes.append(hashlib.sha256(payload.encode("utf-8")).hexdigest())
    return hashes


def _manifest_id(feature_set: str, run_id: str | None) -> str:
    return _uuid5_id("sec-13f-ownership-manifest", feature_set, run_id or "")


class OwnershipFeatureDataset(Dataset):
    dataset_id = "sec_13f_ownership_features"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: OwnershipFeatureOptions) -> DatasetLoadResult:
        loaded_at = now_utc_naive()
        missing_sources = self._missing_source_tables(store)
        if missing_sources:
            with store.transaction():
                for table in (
                    "thirteenf_security_ownership",
                    "thirteenf_security_positions",
                    "thirteenf_manager_reports",
                    "thirteenf_managers",
                ):
                    store.con.execute(f"DELETE FROM {table}")
                store.con.execute("DELETE FROM feature_values WHERE feature_set = ?", [options.feature_set])
                store.con.execute("DELETE FROM feature_definitions WHERE feature_set = ?", [options.feature_set])
                store.con.execute("DELETE FROM feature_build_manifests WHERE feature_set = ?", [options.feature_set])
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=options.source,
                details={
                    "feature_set": options.feature_set,
                    "missing_source_tables": missing_sources,
                },
            )
        reports = self._build_manager_reports(store, options, loaded_at)
        managers = self._build_managers(store, reports, options, loaded_at)
        positions = self._build_positions(store, options, loaded_at)
        ownership = self._build_ownership(store, positions, managers, options, loaded_at)
        feature_definitions = self._build_feature_definitions(options)
        feature_values = self._build_feature_values(ownership, options)
        manifest = self._build_manifest(reports, positions, ownership, feature_values, options)

        with store.transaction():
            for table in (
                "thirteenf_security_ownership",
                "thirteenf_security_positions",
                "thirteenf_manager_reports",
                "thirteenf_managers",
            ):
                store.con.execute(f"DELETE FROM {table}")
            store.con.execute("DELETE FROM feature_values WHERE feature_set = ?", [options.feature_set])
            store.con.execute("DELETE FROM feature_definitions WHERE feature_set = ?", [options.feature_set])
            store.con.execute("DELETE FROM feature_build_manifests WHERE feature_set = ?", [options.feature_set])

            insert_frame(store, managers, "thirteenf_managers", "thirteenf_managers_insert")
            insert_frame(store, reports, "thirteenf_manager_reports", "thirteenf_manager_reports_insert")
            insert_frame(store, positions, "thirteenf_security_positions", "thirteenf_security_positions_insert")
            insert_frame(store, ownership, "thirteenf_security_ownership", "thirteenf_security_ownership_insert")
            insert_frame(store, feature_definitions, "feature_definitions", "ownership_feature_definitions_insert")
            insert_frame(store, feature_values, "feature_values", "ownership_feature_values_insert")
            insert_frame(store, manifest, "feature_build_manifests", "ownership_feature_manifest_insert")

        lineage = refresh_feature_lineage(store)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="thirteenf_security_ownership",
            check_name="rows_loaded",
            status="passed" if len(ownership) > 0 else "warning",
            observed_value=float(len(ownership)),
            threshold_value=1.0,
            details={
                "feature_set": options.feature_set,
                "manager_rows": len(managers),
                "manager_report_rows": len(reports),
                "position_rows": len(positions),
                "feature_value_rows": len(feature_values),
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(len(ownership)),
            source=options.source,
            details={
                "feature_set": options.feature_set,
                "manager_rows": int(len(managers)),
                "manager_report_rows": int(len(reports)),
                "security_position_rows": int(len(positions)),
                "security_ownership_rows": int(len(ownership)),
                "feature_value_rows": int(len(feature_values)),
                "feature_lineage": lineage,
            },
        )

    def _missing_source_tables(self, store: DuckDBStore) -> list[str]:
        required = (
            "thirteenf_submissions",
            "thirteenf_cover_pages",
            "thirteenf_summary_pages",
            "thirteenf_holdings",
        )
        rows = store.con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
              AND table_name IN ('thirteenf_submissions', 'thirteenf_cover_pages', 'thirteenf_summary_pages', 'thirteenf_holdings')
            """
        ).fetchall()
        existing = {str(row[0]) for row in rows}
        return [table for table in required if table not in existing]

    def _build_manager_reports(
        self,
        store: DuckDBStore,
        options: OwnershipFeatureOptions,
        loaded_at: dt.datetime,
    ) -> pd.DataFrame:
        reports = store.con.execute(
            """
            SELECT
                lpad(trim(s.cik), 10, '0') AS cik,
                s.accession_number,
                coalesce(s.period_of_report, c.report_calendar_or_quarter) AS report_period,
                s.filing_date,
                s.source_period,
                s.submission_type,
                c.report_calendar_or_quarter,
                upper(coalesce(c.is_amendment, '')) IN ('TRUE', 'Y', 'YES', '1') AS is_amendment,
                nullif(c.amendment_no, '') AS amendment_no,
                nullif(c.amendment_type, '') AS amendment_type,
                nullif(c.filing_manager_name, '') AS filing_manager_name,
                nullif(c.filing_manager_city, '') AS filing_manager_city,
                nullif(c.filing_manager_state_or_country, '') AS filing_manager_state_or_country,
                nullif(c.report_type, '') AS report_type,
                nullif(c.form_13f_file_number, '') AS form_13f_file_number,
                nullif(c.crd_number, '') AS crd_number,
                nullif(c.sec_file_number, '') AS sec_file_number,
                p.other_included_managers_count,
                p.table_entry_total,
                p.table_value_total,
                upper(coalesce(p.is_confidential_omitted, '')) IN ('TRUE', 'Y', 'YES', '1') AS is_confidential_omitted,
                coalesce(s.filing_date::TIMESTAMP + INTERVAL 22 HOURS, s.source_loaded_at) AS available_at
            FROM thirteenf_submissions s
            LEFT JOIN thirteenf_cover_pages c
              ON c.accession_number = s.accession_number
             AND c.source_period = s.source_period
            LEFT JOIN thirteenf_summary_pages p
              ON p.accession_number = s.accession_number
             AND p.source_period = s.source_period
            WHERE trim(coalesce(s.cik, '')) <> ''
            ORDER BY report_period, cik, accession_number
            """
        ).df()
        if reports.empty:
            return pd.DataFrame(columns=self._manager_report_columns())
        reports["manager_id"] = reports["cik"].map(manager_id_for_cik)
        reports["manager_report_id"] = [
            _uuid5_id("sec-13f-manager-report", row.source_period, row.accession_number)
            for row in reports.itertuples(index=False)
        ]
        reports["source"] = options.source
        reports["run_id"] = options.run_id
        reports["source_loaded_at"] = loaded_at
        return reports[self._manager_report_columns()]

    def _build_managers(
        self,
        store: DuckDBStore,
        reports: pd.DataFrame,
        options: OwnershipFeatureOptions,
        loaded_at: dt.datetime,
    ) -> pd.DataFrame:
        if reports.empty:
            return pd.DataFrame(columns=self._manager_columns())
        store.con.register("ownership_reports_source", reports)
        try:
            managers = store.con.execute(
                """
                WITH ranked AS (
                    SELECT
                        *,
                        row_number() OVER (
                            PARTITION BY cik
                            ORDER BY filing_date DESC NULLS LAST,
                                     report_period DESC NULLS LAST,
                                     source_period DESC,
                                     accession_number DESC
                        ) AS rn
                    FROM ownership_reports_source
                ),
                latest AS (
                    SELECT *
                    FROM ranked
                    WHERE rn = 1
                ),
                agg AS (
                    SELECT
                        cik,
                        min(report_period) AS first_report_period,
                        max(report_period) AS last_report_period,
                        min(filing_date) AS first_filing_date,
                        max(filing_date) AS last_filing_date,
                        count(DISTINCT accession_number) AS filing_count,
                        sum(CASE WHEN is_amendment THEN 1 ELSE 0 END) AS amendment_count,
                        count(DISTINCT source_period) AS source_period_count
                    FROM ownership_reports_source
                    GROUP BY cik
                )
                SELECT
                    latest.manager_id,
                    latest.cik,
                    latest.filing_manager_name AS manager_name,
                    latest.filing_manager_city AS city,
                    latest.filing_manager_state_or_country AS state_or_country,
                    latest.crd_number,
                    latest.sec_file_number,
                    agg.first_report_period,
                    agg.last_report_period,
                    agg.first_filing_date,
                    agg.last_filing_date,
                    agg.filing_count,
                    agg.amendment_count,
                    agg.source_period_count
                FROM latest
                JOIN agg USING (cik)
                ORDER BY latest.manager_id
                """
            ).df()
        finally:
            store.con.unregister("ownership_reports_source")
        managers["source"] = options.source
        managers["run_id"] = options.run_id
        managers["source_loaded_at"] = loaded_at
        return managers[self._manager_columns()]

    def _build_positions(
        self,
        store: DuckDBStore,
        options: OwnershipFeatureOptions,
        loaded_at: dt.datetime,
    ) -> pd.DataFrame:
        positions = store.con.execute(
            """
            WITH report_base AS (
                SELECT
                    lpad(trim(s.cik), 10, '0') AS cik,
                    s.accession_number,
                    coalesce(s.period_of_report, c.report_calendar_or_quarter) AS report_period,
                    s.filing_date,
                    s.source_period,
                    coalesce(p.table_value_total, 0) AS portfolio_value_usd,
                    coalesce(s.filing_date::TIMESTAMP + INTERVAL 22 HOURS, s.source_loaded_at) AS available_at
                FROM thirteenf_submissions s
                LEFT JOIN thirteenf_cover_pages c
                  ON c.accession_number = s.accession_number
                 AND c.source_period = s.source_period
                LEFT JOIN thirteenf_summary_pages p
                  ON p.accession_number = s.accession_number
                 AND p.source_period = s.source_period
                WHERE trim(coalesce(s.cik, '')) <> ''
            ),
            ticker_bridge AS (
                SELECT security_id, min(ticker) AS symbol
                FROM sec_company_tickers
                GROUP BY security_id
            )
            SELECT
                r.cik,
                h.accession_number,
                h.infotable_sk,
                h.security_id,
                t.symbol,
                h.cusip,
                nullif(h.figi, '') AS figi,
                nullif(h.name_of_issuer, '') AS name_of_issuer,
                nullif(h.title_of_class, '') AS title_of_class,
                r.report_period,
                r.filing_date,
                h.source_period,
                r.report_period AS as_of_date,
                r.available_at,
                h.value_usd,
                h.share_quantity,
                h.share_quantity_type,
                nullif(h.put_call, '') AS put_call,
                nullif(h.investment_discretion, '') AS investment_discretion,
                nullif(h.other_manager, '') AS other_manager,
                h.voting_auth_sole,
                h.voting_auth_shared,
                h.voting_auth_none,
                coalesce(h.voting_auth_sole, 0) + coalesce(h.voting_auth_shared, 0) + coalesce(h.voting_auth_none, 0) AS voting_auth_total,
                r.portfolio_value_usd,
                CASE
                    WHEN r.portfolio_value_usd > 0
                     AND h.value_usd IS NOT NULL
                     AND r.portfolio_value_usd >= h.value_usd
                    THEN h.value_usd / r.portfolio_value_usd
                    ELSE NULL
                END AS portfolio_weight,
                coalesce(h.put_call, '') = ''
                    AND upper(coalesce(h.share_quantity_type, '')) = 'SH' AS is_common_share,
                upper(coalesce(h.put_call, '')) IN ('CALL', 'PUT') AS is_option
            FROM thirteenf_holdings h
            JOIN report_base r
              ON r.accession_number = h.accession_number
             AND r.source_period = h.source_period
            LEFT JOIN ticker_bridge t
              ON t.security_id = h.security_id
            ORDER BY r.report_period, h.cusip, h.accession_number, h.infotable_sk
            """
        ).df()
        if positions.empty:
            return pd.DataFrame(columns=self._position_columns())
        positions["manager_id"] = positions["cik"].map(manager_id_for_cik)
        positions["manager_report_id"] = [
            _uuid5_id("sec-13f-manager-report", row.source_period, row.accession_number)
            for row in positions.itertuples(index=False)
        ]
        positions["position_id"] = [
            _uuid5_id(
                "sec-13f-position",
                row.source_period,
                row.accession_number,
                row.infotable_sk,
                row.cusip,
            )
            for row in positions.itertuples(index=False)
        ]
        positions["source"] = options.source
        positions["run_id"] = options.run_id
        positions["source_loaded_at"] = loaded_at
        return positions[self._position_columns()]

    def _build_ownership(
        self,
        store: DuckDBStore,
        positions: pd.DataFrame,
        managers: pd.DataFrame,
        options: OwnershipFeatureOptions,
        loaded_at: dt.datetime,
    ) -> pd.DataFrame:
        if positions.empty:
            return pd.DataFrame(columns=self._ownership_columns())
        store.con.register("ownership_positions_source", positions)
        store.con.register("ownership_managers_source", managers)
        try:
            ownership = store.con.execute(
                """
                WITH manager_security AS (
                    SELECT
                        security_id,
                        any_value(symbol) AS symbol,
                        cusip,
                        report_period,
                        source_period,
                        manager_id,
                        any_value(accession_number) AS accession_number,
                        max(available_at) AS available_at,
                        sum(CASE WHEN is_common_share THEN coalesce(value_usd, 0) ELSE 0 END) AS common_value_usd,
                        sum(CASE WHEN is_common_share THEN coalesce(share_quantity, 0) ELSE 0 END) AS common_share_quantity,
                        sum(CASE WHEN put_call = 'CALL' THEN coalesce(share_quantity, 0) ELSE 0 END) AS call_share_quantity,
                        sum(CASE WHEN put_call = 'PUT' THEN coalesce(share_quantity, 0) ELSE 0 END) AS put_share_quantity,
                        max(portfolio_value_usd) AS portfolio_value_usd,
                        max(CASE WHEN is_common_share THEN portfolio_weight END) AS max_portfolio_weight,
                        count(*) AS holding_row_count,
                        max(CASE WHEN is_common_share THEN 1 ELSE 0 END) = 1 AS has_common_share
                    FROM ownership_positions_source
                    GROUP BY security_id, cusip, report_period, source_period, manager_id
                ),
                grouped AS (
                    SELECT
                        security_id,
                        any_value(symbol) AS symbol,
                        cusip,
                        report_period,
                        source_period,
                        report_period AS as_of_date,
                        max(available_at) AS available_at,
                        sum(holding_row_count) AS holding_row_count,
                        count(DISTINCT accession_number) AS filing_count,
                        count(DISTINCT manager_id) AS holder_count,
                        count(DISTINCT CASE WHEN has_common_share THEN manager_id END) AS common_holder_count,
                        sum(common_value_usd) AS common_value_usd,
                        sum(common_share_quantity) AS common_share_quantity,
                        sum(call_share_quantity) AS call_share_quantity,
                        sum(put_share_quantity) AS put_share_quantity,
                        sum(portfolio_value_usd) AS manager_portfolio_value_usd,
                        avg(CASE WHEN has_common_share THEN max_portfolio_weight END) AS avg_portfolio_weight,
                        max(CASE WHEN has_common_share THEN max_portfolio_weight END) AS max_portfolio_weight,
                        arg_max(manager_id, CASE WHEN has_common_share THEN max_portfolio_weight ELSE NULL END) AS top_manager_id
                    FROM manager_security
                    GROUP BY security_id, cusip, report_period, source_period
                ),
                named AS (
                    SELECT
                        g.*,
                        m.manager_name AS top_manager_name
                    FROM grouped g
                    LEFT JOIN ownership_managers_source m
                      ON m.manager_id = g.top_manager_id
                ),
                lagged AS (
                    SELECT
                        *,
                        lag(report_period) OVER security_window AS prior_report_period,
                        lag(common_value_usd) OVER security_window AS prior_common_value_usd,
                        lag(common_share_quantity) OVER security_window AS prior_common_share_quantity,
                        lag(holder_count) OVER security_window AS prior_holder_count
                    FROM named
                    WINDOW security_window AS (
                        PARTITION BY coalesce(security_id, cusip)
                        ORDER BY report_period, source_period
                    )
                )
                SELECT
                    *,
                    common_value_usd - prior_common_value_usd AS common_value_usd_qoq_change,
                    common_share_quantity - prior_common_share_quantity AS common_share_quantity_qoq_change,
                    holder_count - prior_holder_count AS holder_count_qoq_change,
                    CASE
                        WHEN prior_common_value_usd IS NOT NULL AND prior_common_value_usd <> 0
                        THEN common_value_usd / prior_common_value_usd - 1
                        ELSE NULL
                    END AS common_value_usd_qoq_pct_change,
                    CASE
                        WHEN prior_common_share_quantity IS NOT NULL AND prior_common_share_quantity <> 0
                        THEN common_share_quantity / prior_common_share_quantity - 1
                        ELSE NULL
                    END AS common_share_quantity_qoq_pct_change
                FROM lagged
                ORDER BY report_period, cusip
                """
            ).df()
        finally:
            store.con.unregister("ownership_positions_source")
            store.con.unregister("ownership_managers_source")
        ownership["ownership_id"] = [
            _uuid5_id(
                "sec-13f-security-ownership",
                row.source_period,
                row.report_period,
                row.security_id,
                row.cusip,
            )
            for row in ownership.itertuples(index=False)
        ]
        ownership["source"] = options.source
        ownership["run_id"] = options.run_id
        ownership["source_loaded_at"] = loaded_at
        return ownership[self._ownership_columns()]

    def _build_feature_definitions(self, options: OwnershipFeatureOptions) -> pd.DataFrame:
        rows = []
        for feature_name, definition in OWNERSHIP_FEATURE_DEFINITIONS.items():
            rows.append(
                {
                    "feature_set": options.feature_set,
                    "feature_name": feature_name,
                    "description": definition["description"],
                    "expression_sql": definition["expression_sql"],
                    "input_tables_json": json_dumps(
                        [
                            "thirteenf_submissions",
                            "thirteenf_cover_pages",
                            "thirteenf_summary_pages",
                            "thirteenf_holdings",
                            "thirteenf_security_ownership",
                        ]
                    ),
                    "lookback_days": definition["lookback_days"],
                    "is_point_in_time_safe": True,
                    "available_at_policy": "Feature available only after the underlying 13F filing_date plus a conservative end-of-day timestamp.",
                    "owner": "atx-db",
                    "source": options.source,
                }
            )
        return pd.DataFrame(rows)

    def _build_feature_values(
        self,
        ownership: pd.DataFrame,
        options: OwnershipFeatureOptions,
    ) -> pd.DataFrame:
        columns = [
            "feature_set",
            "feature_name",
            "security_id",
            "symbol",
            "as_of_date",
            "value",
            "input_hash",
            "available_at",
            "source",
            "run_id",
        ]
        if ownership.empty:
            return pd.DataFrame(columns=columns)
        feature_source = ownership[ownership["security_id"].notna() & (ownership["security_id"] != "")]
        frames = []
        for feature_name, column in FEATURE_COLUMNS.items():
            values = feature_source[["security_id", "symbol", "as_of_date", "available_at", column]].rename(
                columns={column: "value"}
            )
            values = values.dropna(subset=["value"])
            values["feature_set"] = options.feature_set
            values["feature_name"] = feature_name
            values["source"] = options.source
            values["run_id"] = options.run_id
            frames.append(values)
        if not frames:
            return pd.DataFrame(columns=columns)
        features = pd.concat(frames, ignore_index=True)
        features["input_hash"] = _feature_hashes(features)
        return features[columns]

    def _build_manifest(
        self,
        reports: pd.DataFrame,
        positions: pd.DataFrame,
        ownership: pd.DataFrame,
        feature_values: pd.DataFrame,
        options: OwnershipFeatureOptions,
    ) -> pd.DataFrame:
        feature_names = sorted(OWNERSHIP_FEATURE_DEFINITIONS)
        symbols = sorted({str(value) for value in ownership["symbol"].dropna().tolist() if str(value)})
        return pd.DataFrame(
            [
                {
                    "manifest_id": _manifest_id(options.feature_set, options.run_id),
                    "feature_set": options.feature_set,
                    "run_id": options.run_id,
                    "symbols_json": json_dumps(symbols),
                    "feature_names_json": json_dumps(feature_names),
                    "input_tables_json": json_dumps(
                        [
                            "thirteenf_submissions",
                            "thirteenf_cover_pages",
                            "thirteenf_summary_pages",
                            "thirteenf_holdings",
                        ]
                    ),
                    "input_min_as_of_date": None if reports.empty else reports["report_period"].min(),
                    "input_max_as_of_date": None if reports.empty else reports["report_period"].max(),
                    "input_row_count": int(len(positions)),
                    "output_min_as_of_date": None if feature_values.empty else feature_values["as_of_date"].min(),
                    "output_max_as_of_date": None if feature_values.empty else feature_values["as_of_date"].max(),
                    "output_row_count": int(len(feature_values)),
                    "feature_count": int(len(feature_names)),
                    "min_available_at": None if feature_values.empty else feature_values["available_at"].min(),
                    "max_available_at": None if feature_values.empty else feature_values["available_at"].max(),
                    "params_json": json_dumps({"feature_set": options.feature_set}),
                    "source": options.source,
                }
            ]
        )

    @staticmethod
    def _manager_columns() -> list[str]:
        return [
            "manager_id",
            "cik",
            "manager_name",
            "city",
            "state_or_country",
            "crd_number",
            "sec_file_number",
            "first_report_period",
            "last_report_period",
            "first_filing_date",
            "last_filing_date",
            "filing_count",
            "amendment_count",
            "source_period_count",
            "source",
            "run_id",
            "source_loaded_at",
        ]

    @staticmethod
    def _manager_report_columns() -> list[str]:
        return [
            "manager_report_id",
            "manager_id",
            "accession_number",
            "cik",
            "report_period",
            "filing_date",
            "source_period",
            "submission_type",
            "report_calendar_or_quarter",
            "is_amendment",
            "amendment_no",
            "amendment_type",
            "filing_manager_name",
            "filing_manager_city",
            "filing_manager_state_or_country",
            "report_type",
            "form_13f_file_number",
            "crd_number",
            "sec_file_number",
            "other_included_managers_count",
            "table_entry_total",
            "table_value_total",
            "is_confidential_omitted",
            "available_at",
            "source",
            "run_id",
            "source_loaded_at",
        ]

    @staticmethod
    def _position_columns() -> list[str]:
        return [
            "position_id",
            "manager_report_id",
            "manager_id",
            "accession_number",
            "infotable_sk",
            "security_id",
            "symbol",
            "cusip",
            "figi",
            "name_of_issuer",
            "title_of_class",
            "report_period",
            "filing_date",
            "source_period",
            "as_of_date",
            "available_at",
            "value_usd",
            "share_quantity",
            "share_quantity_type",
            "put_call",
            "investment_discretion",
            "other_manager",
            "voting_auth_sole",
            "voting_auth_shared",
            "voting_auth_none",
            "voting_auth_total",
            "portfolio_value_usd",
            "portfolio_weight",
            "is_common_share",
            "is_option",
            "source",
            "run_id",
            "source_loaded_at",
        ]

    @staticmethod
    def _ownership_columns() -> list[str]:
        return [
            "ownership_id",
            "security_id",
            "symbol",
            "cusip",
            "report_period",
            "source_period",
            "as_of_date",
            "available_at",
            "holding_row_count",
            "filing_count",
            "holder_count",
            "common_holder_count",
            "common_value_usd",
            "common_share_quantity",
            "call_share_quantity",
            "put_share_quantity",
            "manager_portfolio_value_usd",
            "avg_portfolio_weight",
            "max_portfolio_weight",
            "top_manager_id",
            "top_manager_name",
            "prior_report_period",
            "prior_common_value_usd",
            "prior_common_share_quantity",
            "prior_holder_count",
            "common_value_usd_qoq_change",
            "common_share_quantity_qoq_change",
            "holder_count_qoq_change",
            "common_value_usd_qoq_pct_change",
            "common_share_quantity_qoq_pct_change",
            "source",
            "run_id",
            "source_loaded_at",
        ]
