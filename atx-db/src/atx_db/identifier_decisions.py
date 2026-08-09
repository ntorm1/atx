from __future__ import annotations

import datetime as dt
import uuid
from dataclasses import dataclass
from typing import Iterable

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, now_utc_naive, quality_check


SOURCE_NAME = "atx-db identifier decision manager"
DEFAULT_DECISION_METHOD = "auto_confidence_status_policy_v1"
VALID_CANDIDATE_STATUSES = {"already_mapped", "proposed", "conflict"}
VALID_DECISION_STATUSES = {"accepted", "rejected", "needs_review"}

# Sentinel prefix written by identifiers_figi.py (and any other candidate
# producer) into target_security_id when a source key (e.g. a CUSIP) has no
# resolved security_id yet -- a placeholder to satisfy the NOT NULL
# constraint on identifier_resolution_candidates/_decisions.target_security_id.
# It must NEVER be treated as a real security_id: the apply path below rejects
# it (and any other target_security_id absent from securities) rather than
# risk writing a fake key into security_identifier_history.
UNRESOLVED_CUSIP_PREFIX = "UNRESOLVED-CUSIP-"


@dataclass(frozen=True)
class IdentifierResolutionDecisionOptions:
    source_dataset_id: str = "sec_13f"
    source_period: str | None = None
    min_accept_confidence: float = 0.98
    min_review_confidence: float = 0.8
    accept_candidate_statuses: tuple[str, ...] = ("already_mapped",)
    review_candidate_statuses: tuple[str, ...] = ("proposed", "conflict")
    apply_accepted: bool = True
    decision_method: str = DEFAULT_DECISION_METHOD
    decided_by: str = "system:auto_identifier_resolution_v1"
    source: str = SOURCE_NAME
    run_id: str | None = None


def decision_id_for(candidate_id: str, decision_method: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"{candidate_id}:{decision_method}"))


def _normalized_statuses(values: Iterable[str]) -> tuple[str, ...]:
    return tuple(str(value).strip().lower() for value in values if str(value).strip())


class IdentifierResolutionDecisionDataset(Dataset):
    dataset_id = "identifier_resolution_decisions"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: IdentifierResolutionDecisionOptions,
    ) -> DatasetLoadResult:
        self._validate_options(options)
        decisions = self._build_decisions(store, options)
        with store.transaction():
            rows = self._replace_decisions(store, decisions, options)
            applied_identifier_rows = 0
            updated_holding_rows = 0
            if options.apply_accepted and not decisions.empty:
                applied_identifier_rows = self._apply_accepted_identifiers(store, decisions, options)
                updated_holding_rows = self._apply_accepted_13f_holdings(store, decisions)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="identifier_resolution_decisions",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source_dataset_id": options.source_dataset_id,
                "source_period": options.source_period,
                "decision_method": options.decision_method,
                "apply_accepted": options.apply_accepted,
                "applied_identifier_rows": applied_identifier_rows,
                "updated_holding_rows": updated_holding_rows,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "source_dataset_id": options.source_dataset_id,
                "source_period": options.source_period,
                "decision_method": options.decision_method,
                "accepted_rows": int((decisions.get("decision_status", pd.Series(dtype=str)) == "accepted").sum()),
                "needs_review_rows": int((decisions.get("decision_status", pd.Series(dtype=str)) == "needs_review").sum()),
                "applied_identifier_rows": applied_identifier_rows,
                "updated_holding_rows": updated_holding_rows,
            },
        )

    def _validate_options(self, options: IdentifierResolutionDecisionOptions) -> None:
        if options.min_review_confidence < 0 or options.min_review_confidence > 1:
            raise ValueError("min_review_confidence must be in [0, 1]")
        if options.min_accept_confidence < 0 or options.min_accept_confidence > 1:
            raise ValueError("min_accept_confidence must be in [0, 1]")
        if options.min_review_confidence > options.min_accept_confidence:
            raise ValueError("min_review_confidence must be <= min_accept_confidence")
        accept_statuses = set(_normalized_statuses(options.accept_candidate_statuses))
        review_statuses = set(_normalized_statuses(options.review_candidate_statuses))
        unknown = (accept_statuses | review_statuses) - VALID_CANDIDATE_STATUSES
        if unknown:
            raise ValueError(f"Unknown candidate status values: {sorted(unknown)}")
        if not options.decision_method.strip():
            raise ValueError("decision_method must not be empty")
        if not options.decided_by.strip():
            raise ValueError("decided_by must not be empty")

    def _candidate_frame(
        self,
        store: DuckDBStore,
        options: IdentifierResolutionDecisionOptions,
    ) -> pd.DataFrame:
        where = ["source_dataset_id = ?", "confidence >= ?"]
        params: list[object] = [options.source_dataset_id, options.min_review_confidence]
        if options.source_period is not None:
            where.append("source_period = ?")
            params.append(options.source_period)
        return store.con.execute(
            f"""
            SELECT *
            FROM identifier_resolution_candidates
            WHERE {" AND ".join(where)}
            ORDER BY source_key_type, source_key_value, target_security_id
            """,
            params,
        ).df()

    def _build_decisions(
        self,
        store: DuckDBStore,
        options: IdentifierResolutionDecisionOptions,
    ) -> pd.DataFrame:
        candidates = self._candidate_frame(store, options)
        if candidates.empty:
            return pd.DataFrame()

        accept_statuses = set(_normalized_statuses(options.accept_candidate_statuses))
        review_statuses = set(_normalized_statuses(options.review_candidate_statuses))
        decided_at = now_utc_naive()
        rows = []
        for row in candidates.itertuples(index=False):
            candidate_status = str(row.candidate_status).strip().lower()
            decision_status: str | None = None
            if candidate_status in accept_statuses and float(row.confidence) >= options.min_accept_confidence:
                decision_status = "accepted"
            elif candidate_status in review_statuses and float(row.confidence) >= options.min_review_confidence:
                decision_status = "needs_review"
            if decision_status is None:
                continue
            as_of_date = row.as_of_date or dt.date.today()
            available_at = row.available_at
            if pd.isna(available_at):
                available_at = pd.Timestamp(as_of_date) + pd.Timedelta(hours=22)
            rows.append(
                {
                    "decision_id": decision_id_for(row.candidate_id, options.decision_method),
                    "candidate_id": row.candidate_id,
                    "source_dataset_id": row.source_dataset_id,
                    "source_table": row.source_table,
                    "source_period": row.source_period,
                    "source_key_type": row.source_key_type,
                    "source_key_value": row.source_key_value,
                    "source_security_id": None if pd.isna(row.source_security_id) else row.source_security_id,
                    "target_security_id": row.target_security_id,
                    "target_id_type": row.target_id_type,
                    "target_id_value": row.target_id_value,
                    "match_method": row.match_method,
                    "confidence": float(row.confidence),
                    "candidate_status": candidate_status,
                    "decision_status": decision_status,
                    "decision_method": options.decision_method,
                    "decided_by": options.decided_by,
                    "decided_at": decided_at,
                    "effective_from": as_of_date,
                    "as_of_date": as_of_date,
                    "available_at": available_at,
                    "notes_json": json_dumps(
                        {
                            "policy": {
                                "min_accept_confidence": options.min_accept_confidence,
                                "min_review_confidence": options.min_review_confidence,
                                "accept_candidate_statuses": sorted(accept_statuses),
                                "review_candidate_statuses": sorted(review_statuses),
                            },
                            "candidate_status": candidate_status,
                            "candidate_details_json": row.details_json,
                        }
                    ),
                    "run_id": options.run_id,
                }
            )
        return pd.DataFrame(rows)

    def _replace_decisions(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: IdentifierResolutionDecisionOptions,
    ) -> int:
        predicates = ["source_dataset_id = ?", "decision_method = ?"]
        params: list[object] = [options.source_dataset_id, options.decision_method]
        if options.source_period is not None:
            predicates.append("source_period = ?")
            params.append(options.source_period)
        store.con.execute(
            f"DELETE FROM identifier_resolution_decisions WHERE {' AND '.join(predicates)}",
            params,
        )
        if frame.empty:
            return 0
        return insert_frame(
            store,
            frame,
            "identifier_resolution_decisions",
            "identifier_resolution_decisions_insert",
        )

    def _accepted_cusips(self, frame: pd.DataFrame) -> pd.DataFrame:
        if frame.empty:
            return pd.DataFrame()
        accepted = frame[
            (frame["decision_status"] == "accepted")
            & (frame["source_key_type"].str.upper() == "CUSIP")
        ].copy()
        if accepted.empty:
            return pd.DataFrame()
        return accepted.drop_duplicates(
            subset=["source_key_value", "source_period", "target_security_id", "effective_from"]
        ).reset_index(drop=True)

    def _reject_unresolvable_targets(self, store: DuckDBStore, accepted: pd.DataFrame) -> None:
        """Guard the consumption point: an accepted row's target_security_id must be
        a real key in securities.security_id. This blocks the UNRESOLVED-CUSIP-*
        placeholder (written by candidate producers like identifiers_figi.py for
        unmatched source keys) -- and any other bogus target_security_id -- from
        ever being written into security_identifier_history, regardless of how the
        decision row came to be marked "accepted". Fails loudly rather than
        silently corrupting the identifier spine.
        """
        targets = sorted(set(accepted["target_security_id"].dropna()))
        if not targets:
            return
        sentinel_targets = [t for t in targets if str(t).startswith(UNRESOLVED_CUSIP_PREFIX)]
        if sentinel_targets:
            raise ValueError(
                "Refusing to apply accepted identifier decision(s) with an "
                f"UNRESOLVED-CUSIP sentinel target_security_id: {sentinel_targets!r}. "
                "This sentinel is a placeholder for an unmatched source key, not a "
                "real security_id, and must never be written into "
                "security_identifier_history."
            )
        lookup = pd.DataFrame({"target_security_id": targets})
        store.con.register("identifier_resolution_target_check", lookup)
        try:
            missing = store.con.execute(
                """
                SELECT l.target_security_id
                FROM identifier_resolution_target_check l
                LEFT JOIN securities s ON s.security_id = l.target_security_id
                WHERE s.security_id IS NULL
                """
            ).fetchall()
        finally:
            store.con.unregister("identifier_resolution_target_check")
        if missing:
            missing_ids = sorted(row[0] for row in missing)
            raise ValueError(
                "Refusing to apply accepted identifier decision(s) whose "
                f"target_security_id does not exist in securities: {missing_ids!r}."
            )

    def _apply_accepted_identifiers(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: IdentifierResolutionDecisionOptions,
    ) -> int:
        accepted = self._accepted_cusips(frame)
        if accepted.empty:
            return 0
        self._reject_unresolvable_targets(store, accepted)
        identifiers = pd.DataFrame(
            {
                "security_id": accepted["target_security_id"],
                "id_type": "CUSIP",
                "id_value": accepted["source_key_value"],
                "valid_from": accepted["effective_from"],
                "valid_to": pd.NaT,
                "as_of_date": accepted["as_of_date"],
                "available_at": accepted["available_at"],
                "source": SOURCE_NAME,
                "run_id": options.run_id,
            }
        ).drop_duplicates(subset=["security_id", "id_type", "id_value", "valid_from"])
        store.con.register("identifier_resolution_accepted_identifiers", identifiers)
        try:
            store.con.execute(
                """
                DELETE FROM security_identifier_history
                USING identifier_resolution_accepted_identifiers src
                WHERE security_identifier_history.security_id = src.security_id
                  AND security_identifier_history.id_type = src.id_type
                  AND security_identifier_history.id_value = src.id_value
                  AND security_identifier_history.valid_from = src.valid_from
                  AND security_identifier_history.source = ?
                """,
                [SOURCE_NAME],
            )
        finally:
            store.con.unregister("identifier_resolution_accepted_identifiers")
        return insert_frame(
            store,
            identifiers,
            "security_identifier_history",
            "identifier_resolution_identifiers_insert",
        )

    def _apply_accepted_13f_holdings(self, store: DuckDBStore, frame: pd.DataFrame) -> int:
        accepted = self._accepted_cusips(frame)
        if accepted.empty:
            return 0
        holdings = accepted[
            (accepted["source_dataset_id"] == "sec_13f")
            & (accepted["source_table"] == "thirteenf_holdings")
            & accepted["source_period"].notna()
        ][["source_key_value", "source_period", "target_security_id"]].drop_duplicates()
        if holdings.empty:
            return 0
        # Guard the consumption point, mirroring _apply_accepted_identifiers:
        # target_security_id is written directly into thirteenf_holdings.security_id
        # below, so it must be a real key in securities.security_id before the
        # UPDATE runs -- this blocks the UNRESOLVED-CUSIP-* placeholder (and any
        # other bogus target_security_id) from corrupting 13F holding rows.
        self._reject_unresolvable_targets(store, holdings)
        store.con.register("identifier_resolution_accepted_holdings", holdings)
        try:
            pending = int(
                store.con.execute(
                    """
                    SELECT count(*)
                    FROM thirteenf_holdings h
                    JOIN identifier_resolution_accepted_holdings src
                      ON h.cusip = src.source_key_value
                     AND h.source_period = src.source_period
                    WHERE h.security_id IS NULL
                       OR h.security_id <> src.target_security_id
                    """
                ).fetchone()[0]
            )
            store.con.execute(
                """
                UPDATE thirteenf_holdings AS h
                SET security_id = src.target_security_id
                FROM identifier_resolution_accepted_holdings src
                WHERE h.cusip = src.source_key_value
                  AND h.source_period = src.source_period
                  AND (h.security_id IS NULL OR h.security_id <> src.target_security_id)
                """
            )
        finally:
            store.con.unregister("identifier_resolution_accepted_holdings")
        return pending
