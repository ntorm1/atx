from __future__ import annotations

import datetime as dt
import re
import uuid
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "atx-db identifier resolver"
LEGAL_SUFFIXES = {
    "INC",
    "INCORPORATED",
    "CORP",
    "CORPORATION",
    "CO",
    "COMPANY",
    "LTD",
    "LIMITED",
    "PLC",
    "LLC",
    "LP",
    "L P",
    "SA",
    "S A",
    "NV",
    "N V",
    "AG",
    "SE",
    "ADR",
    "ADS",
    "HLDG",
    "HLDGS",
    "HOLDING",
    "HOLDINGS",
}


@dataclass(frozen=True)
class IdentifierResolutionOptions:
    source_dataset_id: str = "sec_13f"
    source_period: str | None = None
    min_confidence: float = 0.8
    include_already_mapped: bool = True
    source: str = SOURCE_NAME
    run_id: str | None = None


def normalize_entity_name(value: str | None) -> str:
    text = (value or "").upper()
    text = text.replace("&", " AND ")
    text = re.sub(r"[^0-9A-Z]+", " ", text)
    text = re.sub(r"\b(CL|CLASS|COM|COMMON|ORD|SHS|SHARES|SPONSORED|NEW)\b", " ", text)
    parts = [part for part in text.split() if part not in LEGAL_SUFFIXES]
    return " ".join(parts)


def candidate_id_for(
    *,
    source_dataset_id: str,
    source_period: str | None,
    source_key_type: str,
    source_key_value: str,
    target_security_id: str,
    match_method: str,
) -> str:
    payload = "|".join(
        [
            source_dataset_id,
            source_period or "",
            source_key_type,
            source_key_value,
            target_security_id,
            match_method,
        ]
    )
    return str(uuid.uuid5(uuid.NAMESPACE_URL, payload))


class IdentifierResolutionCandidateDataset(Dataset):
    dataset_id = "identifier_resolution_candidates"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: IdentifierResolutionOptions) -> DatasetLoadResult:
        if options.min_confidence < 0 or options.min_confidence > 1:
            raise ValueError("min_confidence must be in [0, 1]")
        frame = self._build_candidates(store, options)
        rows = self._replace_candidates(store, frame, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="identifier_resolution_candidates",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source_dataset_id": options.source_dataset_id,
                "source_period": options.source_period,
                "min_confidence": options.min_confidence,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "source_dataset_id": options.source_dataset_id,
                "source_period": options.source_period,
                "min_confidence": options.min_confidence,
            },
        )

    def _source_cusips(self, store: DuckDBStore, options: IdentifierResolutionOptions) -> pd.DataFrame:
        where = ["h.cusip IS NOT NULL", "h.cusip <> ''"]
        params: list[object] = []
        if options.source_period is not None:
            where.append("h.source_period = ?")
            params.append(options.source_period)
        return store.con.execute(
            f"""
            WITH ranked_names AS (
                SELECT
                    h.cusip,
                    h.name_of_issuer,
                    any_value(h.security_id) AS source_security_id,
                    max(h.source_period) AS source_period,
                    max(coalesce(s.period_of_report, s.filing_date, current_date)) AS max_report_date,
                    max(coalesce(s.filing_date, s.period_of_report, current_date)) AS max_filing_date,
                    count(*) AS holding_rows,
                    row_number() OVER (
                        PARTITION BY h.cusip
                        ORDER BY count(*) DESC, h.name_of_issuer
                    ) AS rn
                FROM thirteenf_holdings h
                LEFT JOIN thirteenf_submissions s
                  ON s.accession_number = h.accession_number
                 AND s.source_period = h.source_period
                WHERE {" AND ".join(where)}
                GROUP BY h.cusip, h.name_of_issuer
            )
            SELECT *
            FROM ranked_names
            WHERE rn = 1
            """,
            params,
        ).df()

    def _sec_targets(self, store: DuckDBStore) -> pd.DataFrame:
        return store.con.execute(
            """
            SELECT
                security_id,
                cik,
                ticker,
                title,
                source_loaded_at
            FROM sec_company_tickers
            QUALIFY row_number() OVER (
                PARTITION BY security_id
                ORDER BY source_loaded_at DESC NULLS LAST, ticker
            ) = 1
            """
        ).df()

    def _build_candidates(self, store: DuckDBStore, options: IdentifierResolutionOptions) -> pd.DataFrame:
        source = self._source_cusips(store, options)
        targets = self._sec_targets(store)
        if source.empty or targets.empty:
            return pd.DataFrame()

        source["source_normalized_name"] = source["name_of_issuer"].map(normalize_entity_name)
        targets["target_normalized_name"] = targets["title"].map(normalize_entity_name)
        source = source[source["source_normalized_name"] != ""].reset_index(drop=True)
        targets = targets[targets["target_normalized_name"] != ""].reset_index(drop=True)
        if source.empty or targets.empty:
            return pd.DataFrame()

        target_counts = (
            targets.groupby("target_normalized_name")["security_id"]
            .nunique()
            .reset_index(name="target_count")
        )
        joined = source.merge(
            targets,
            left_on="source_normalized_name",
            right_on="target_normalized_name",
            how="inner",
        ).merge(target_counts, on="target_normalized_name", how="left")
        if joined.empty:
            return pd.DataFrame()

        rows = []
        for row in joined.itertuples(index=False):
            target_count = int(row.target_count or 0)
            confidence = 0.98 if target_count == 1 else 0.65
            if confidence < options.min_confidence:
                continue
            source_security_id = None if pd.isna(row.source_security_id) else str(row.source_security_id)
            if source_security_id == row.security_id:
                status = "already_mapped"
            elif source_security_id and not source_security_id.startswith("US-CUSIP-"):
                status = "conflict"
            else:
                status = "proposed"
            if status == "already_mapped" and not options.include_already_mapped:
                continue
            as_of_date = row.max_filing_date or row.max_report_date or dt.date.today()
            available_at = pd.Timestamp(as_of_date) + pd.Timedelta(hours=22)
            match_method = "issuer_name_exact_normalized"
            rows.append(
                {
                    "candidate_id": candidate_id_for(
                        source_dataset_id=options.source_dataset_id,
                        source_period=row.source_period,
                        source_key_type="CUSIP",
                        source_key_value=row.cusip,
                        target_security_id=row.security_id,
                        match_method=match_method,
                    ),
                    "source_dataset_id": options.source_dataset_id,
                    "source_table": "thirteenf_holdings",
                    "source_period": row.source_period,
                    "source_key_type": "CUSIP",
                    "source_key_value": row.cusip,
                    "source_security_id": source_security_id,
                    "source_name": row.name_of_issuer,
                    "source_normalized_name": row.source_normalized_name,
                    "target_security_id": row.security_id,
                    "target_id_type": "CIK",
                    "target_id_value": row.cik,
                    "target_name": row.title,
                    "target_normalized_name": row.target_normalized_name,
                    "match_method": match_method,
                    "confidence": confidence,
                    "candidate_status": status,
                    "as_of_date": as_of_date,
                    "available_at": available_at,
                    "details_json": json_dumps(
                        {
                            "target_count_for_normalized_name": target_count,
                            "source_holding_rows": row.holding_rows,
                            "ticker": row.ticker,
                            "source_loaded_at": row.source_loaded_at,
                        }
                    ),
                    "run_id": options.run_id,
                }
            )
        return pd.DataFrame(rows)

    def _replace_candidates(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: IdentifierResolutionOptions,
    ) -> int:
        with store.transaction():
            predicates = ["source_dataset_id = ?"]
            params: list[object] = [options.source_dataset_id]
            if options.source_period is not None:
                predicates.append("source_period = ?")
                params.append(options.source_period)
            store.con.execute(
                f"DELETE FROM identifier_resolution_candidates WHERE {' AND '.join(predicates)}",
                params,
            )
            if frame.empty:
                return 0
            insert_frame(
                store,
                frame,
                "identifier_resolution_candidates",
                "identifier_resolution_candidates_insert",
            )
        return int(len(frame))
