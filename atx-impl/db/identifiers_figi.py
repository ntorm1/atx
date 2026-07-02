from __future__ import annotations

import datetime as dt
import json
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .identifier_decisions import decision_id_for, now_utc_naive
from .identifier_resolution import candidate_id_for
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "OpenFIGI"
DATASET_ID = "identifiers_figi"
MATCH_METHOD = "openfigi_cusip_mapping"
# A cusip that resolves to more than one existing security_id (conflict) or to
# none at all (unmatched) is routed to the resolution ledger instead of merged;
# both are sub-1.0 confidence since the match itself is not clean.
CONFLICT_CONFIDENCE = 0.5

ALIAS_COLUMNS = [
    "security_id",
    "id_type",
    "id_value",
    "internal_cusip",
    "valid_from",
    "valid_to",
    "as_of_date",
    "available_at",
    "source",
    "run_id",
]


@dataclass(frozen=True)
class FigiLoadOptions:
    figi_file: Path
    source: str = SOURCE_NAME
    as_of_date: dt.date | None = None
    run_id: str | None = None


def _normalize_cusip(value: object) -> str | None:
    if value is None:
        return None
    try:
        if pd.isna(value):
            return None
    except (TypeError, ValueError):
        pass
    text = str(value).strip().upper()
    return text or None


def _normalize_str(value: object) -> str | None:
    if value is None:
        return None
    try:
        if pd.isna(value):
            return None
    except (TypeError, ValueError):
        pass
    text = str(value).strip()
    return text or None


def _mapping_frame_from_records(records: list[dict]) -> pd.DataFrame:
    frame = pd.DataFrame(records)
    for column in ("cusip", "figi", "ticker", "name"):
        if column not in frame.columns:
            frame[column] = pd.NA
    return frame[["cusip", "figi", "ticker", "name"]]


def _parse_mapping_v3_json(payload: dict) -> pd.DataFrame:
    """Parse the shape of a saved ``POST /v3/mapping`` call.

    The OpenFIGI mapping endpoint takes an ordered list of request objects
    (``{"idType": "ID_CUSIP", "idValue": "<cusip>"}``) and returns a
    positionally-aligned list of responses, each either ``{"data": [...]}``
    (one or more FIGI matches) or ``{"error": "..."}`` (no match). A saved
    request/response pair is the natural offline snapshot of a live call.
    """
    requests_ = payload.get("requests") or []
    responses = payload.get("responses") or []
    records: list[dict] = []
    for request, response in zip(requests_, responses):
        if not isinstance(response, dict) or "data" not in response:
            continue
        cusip = request.get("idValue")
        for item in response.get("data") or []:
            records.append(
                {
                    "cusip": cusip,
                    "figi": item.get("figi"),
                    "ticker": item.get("ticker"),
                    "name": item.get("name"),
                }
            )
    return _mapping_frame_from_records(records)


def parse_openfigi_file(path: str | Path) -> pd.DataFrame:
    """Read an injectable OpenFIGI mapping export into a flat mapping frame.

    Supports two OFFLINE shapes, never a network call:
      - CSV with (at least) ``cusip, figi`` columns (``ticker``/``name`` optional) --
        the shape of a bulk dump.
      - JSON: either the raw ``POST /v3/mapping`` request/response pair
        (``{"requests": [...], "responses": [...]}``) or a flat array of
        ``{"cusip", "figi", "ticker", "name"}`` records.

    Returns a frame with columns ``cusip, figi, ticker, name``, normalized
    (stripped, cusip upper-cased) and with blank/NaN cusip or figi rows
    dropped.
    """
    path = Path(path)
    suffix = path.suffix.lower()
    if suffix == ".csv":
        frame = pd.read_csv(path, dtype=str)
        frame = _mapping_frame_from_records(frame.to_dict(orient="records"))
    elif suffix == ".json":
        payload = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(payload, dict) and "responses" in payload:
            frame = _parse_mapping_v3_json(payload)
        elif isinstance(payload, list):
            frame = _mapping_frame_from_records(payload)
        else:
            raise ValueError(f"Unrecognized OpenFIGI JSON shape in {path}")
    else:
        raise ValueError(f"Unsupported OpenFIGI mapping file extension: {path.suffix!r} ({path})")

    frame["cusip"] = frame["cusip"].map(_normalize_cusip)
    frame["figi"] = frame["figi"].map(_normalize_str)
    frame["ticker"] = frame["ticker"].map(_normalize_str)
    frame["name"] = frame["name"].map(_normalize_str)
    frame = frame[frame["cusip"].notna() & frame["figi"].notna()].reset_index(drop=True)
    return frame


def compute_figi_alias_rows(
    mapping: pd.DataFrame,
    resolution: pd.DataFrame,
    *,
    source: str = SOURCE_NAME,
    run_id: str | None,
    as_of_date: dt.date,
    available_at: dt.datetime,
) -> pd.DataFrame:
    """Pure transform: OpenFIGI mapping frame + resolved cusip->security_id in,
    long ``security_identifier_history`` alias frame out.

    ``mapping`` carries one row per ``cusip`` with ``figi``/``ticker`` (as parsed
    by :func:`parse_openfigi_file`). ``resolution`` carries exactly one row per
    ``cusip`` that has an UNAMBIGUOUS ``security_id`` match (ambiguous/unmatched
    cusips must already be filtered out by the caller and routed through the
    resolution ledger instead). Emits two rows per matched cusip -- a ``FIGI``
    alias and a ``TICKER`` alias -- each carrying the source cusip in the
    internal-only ``internal_cusip`` column, never as ``id_type``/``id_value``.
    No DuckDB access; safe to unit test in isolation.
    """
    if mapping is None or mapping.empty or resolution is None or resolution.empty:
        return pd.DataFrame(columns=ALIAS_COLUMNS)

    joined = mapping.merge(resolution[["cusip", "security_id"]], on="cusip", how="inner")
    joined = joined[joined["figi"].notna() & (joined["figi"] != "")]
    if joined.empty:
        return pd.DataFrame(columns=ALIAS_COLUMNS)

    frames = [
        pd.DataFrame(
            {
                "security_id": joined["security_id"],
                "id_type": "FIGI",
                "id_value": joined["figi"],
                "internal_cusip": joined["cusip"],
                "valid_from": as_of_date,
                "valid_to": pd.NaT,
                "as_of_date": as_of_date,
                "available_at": available_at,
                "source": source,
                "run_id": run_id,
            }
        )
    ]
    ticker_rows = joined[joined["ticker"].notna() & (joined["ticker"] != "")]
    if not ticker_rows.empty:
        frames.append(
            pd.DataFrame(
                {
                    "security_id": ticker_rows["security_id"],
                    "id_type": "TICKER",
                    "id_value": ticker_rows["ticker"],
                    "internal_cusip": ticker_rows["cusip"],
                    "valid_from": as_of_date,
                    "valid_to": pd.NaT,
                    "as_of_date": as_of_date,
                    "available_at": available_at,
                    "source": source,
                    "run_id": run_id,
                }
            )
        )
    out = pd.concat(frames, ignore_index=True)
    return out[ALIAS_COLUMNS].drop_duplicates(subset=["security_id", "id_type", "id_value", "source"])


def _cusip_security_matches(store: DuckDBStore, cusips: list[str]) -> pd.DataFrame:
    """All (cusip -> security_id) matches from the existing CUSIP alias history.

    Returns every candidate row, including cusips that map to more than one
    distinct ``security_id`` -- ambiguity detection is the caller's job.
    """
    if not cusips:
        return pd.DataFrame(columns=["cusip", "security_id"])
    lookup = pd.DataFrame({"cusip": sorted(set(cusips))})
    store.con.register("figi_cusip_lookup", lookup)
    try:
        rows = store.con.execute(
            """
            SELECT DISTINCT l.cusip, h.security_id
            FROM figi_cusip_lookup l
            JOIN security_identifier_history h
              ON h.id_type = 'CUSIP'
             AND h.id_value = l.cusip
            """
        ).fetchall()
    finally:
        store.con.unregister("figi_cusip_lookup")
    return pd.DataFrame(rows, columns=["cusip", "security_id"])


class FigiAliasDataset(Dataset):
    """Offline OpenFIGI loader: cusip -> figi resolution against the identifier spine.

    Reads an injectable OpenFIGI mapping export (CSV or the ``POST /v3/mapping``
    JSON shape). Unambiguous cusip -> security_id matches (exactly one security
    already carries that CUSIP alias) get a FIGI + TICKER alias row; the source
    CUSIP is persisted only in the internal-only ``internal_cusip`` column.
    Ambiguous (multi-security) or unmatched cusips are written as
    ``identifier_resolution_candidates`` and routed through
    ``identifier_resolution_decisions`` -- never merged blindly. No network call.
    """

    dataset_id = DATASET_ID
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FigiLoadOptions) -> DatasetLoadResult:
        mapping = parse_openfigi_file(options.figi_file)
        as_of_date = options.as_of_date or dt.date.today()
        available_at = pd.Timestamp(as_of_date) + pd.Timedelta(hours=22)

        matches = _cusip_security_matches(store, list(mapping["cusip"]))
        match_counts = matches.groupby("cusip")["security_id"].nunique() if not matches.empty else pd.Series(dtype=int)
        unambiguous_cusips = set(match_counts[match_counts == 1].index)
        ambiguous_cusips = set(match_counts[match_counts > 1].index)
        matched_cusips = set(mapping["cusip"]) & set(matches["cusip"]) if not matches.empty else set()
        unmatched_cusips = set(mapping["cusip"]) - matched_cusips

        unambiguous_resolution = matches[matches["cusip"].isin(unambiguous_cusips)]
        alias_rows = compute_figi_alias_rows(
            mapping,
            unambiguous_resolution,
            source=options.source,
            run_id=options.run_id,
            as_of_date=as_of_date,
            available_at=available_at,
        )

        conflict_matches = matches[matches["cusip"].isin(ambiguous_cusips)]
        candidates = self._build_candidates(
            mapping,
            conflict_matches,
            unmatched_cusips,
            options=options,
            as_of_date=as_of_date,
            available_at=available_at,
        )

        alias_rows_written = 0
        candidate_rows_written = 0
        decision_rows_written = 0
        with store.transaction():
            if not alias_rows.empty:
                store.con.register("figi_alias_insert", alias_rows)
                try:
                    store.con.execute(
                        """
                        DELETE FROM security_identifier_history
                        USING figi_alias_insert src
                        WHERE security_identifier_history.security_id = src.security_id
                          AND security_identifier_history.id_type = src.id_type
                          AND security_identifier_history.id_value = src.id_value
                          AND security_identifier_history.source = src.source
                        """
                    )
                finally:
                    store.con.unregister("figi_alias_insert")
                alias_rows_written = insert_frame(
                    store, alias_rows, "security_identifier_history", "figi_alias_rows"
                )
            if not candidates.empty:
                store.con.execute(
                    "DELETE FROM identifier_resolution_candidates WHERE source_dataset_id = ? AND match_method = ?",
                    [DATASET_ID, MATCH_METHOD],
                )
                candidate_rows_written = insert_frame(
                    store, candidates, "identifier_resolution_candidates", "figi_candidate_rows"
                )
                decisions = self._build_decisions(candidates, options=options)
                store.con.execute(
                    "DELETE FROM identifier_resolution_decisions WHERE source_dataset_id = ? AND decision_method = ?",
                    [DATASET_ID, MATCH_METHOD],
                )
                decision_rows_written = insert_frame(
                    store, decisions, "identifier_resolution_decisions", "figi_decision_rows"
                )

        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="security_identifier_history",
            check_name="rows_loaded",
            status="passed" if alias_rows_written > 0 else "warning",
            observed_value=float(alias_rows_written),
            threshold_value=1.0,
            details={
                "figi_file": str(options.figi_file),
                "candidate_rows": candidate_rows_written,
                "decision_rows": decision_rows_written,
                "unmatched_cusips": len(unmatched_cusips),
                "ambiguous_cusips": len(ambiguous_cusips),
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(alias_rows_written),
            source=options.source,
            details={
                "figi_file": str(options.figi_file),
                "mapping_rows": int(len(mapping)),
                "alias_rows": alias_rows_written,
                "candidate_rows": candidate_rows_written,
                "decision_rows": decision_rows_written,
                "unmatched_cusips": len(unmatched_cusips),
                "ambiguous_cusips": len(ambiguous_cusips),
            },
        )

    def _build_candidates(
        self,
        mapping: pd.DataFrame,
        conflict_matches: pd.DataFrame,
        unmatched_cusips: set[str],
        *,
        options: FigiLoadOptions,
        as_of_date: dt.date,
        available_at: dt.datetime,
    ) -> pd.DataFrame:
        mapping_by_cusip = mapping.set_index("cusip")
        rows: list[dict] = []

        if not conflict_matches.empty:
            for row in conflict_matches.itertuples(index=False):
                cusip = row.cusip
                map_row = mapping_by_cusip.loc[cusip]
                figi = map_row["figi"] if not isinstance(map_row, pd.DataFrame) else map_row.iloc[0]["figi"]
                name = map_row["name"] if not isinstance(map_row, pd.DataFrame) else map_row.iloc[0]["name"]
                rows.append(
                    self._candidate_row(
                        cusip=cusip,
                        security_id=row.security_id,
                        figi=figi,
                        name=name,
                        status="conflict",
                        confidence=CONFLICT_CONFIDENCE,
                        options=options,
                        as_of_date=as_of_date,
                        available_at=available_at,
                    )
                )

        for cusip in sorted(unmatched_cusips):
            map_row = mapping_by_cusip.loc[cusip]
            figi = map_row["figi"] if not isinstance(map_row, pd.DataFrame) else map_row.iloc[0]["figi"]
            name = map_row["name"] if not isinstance(map_row, pd.DataFrame) else map_row.iloc[0]["name"]
            rows.append(
                self._candidate_row(
                    cusip=cusip,
                    security_id=None,
                    figi=figi,
                    name=name,
                    status="proposed",
                    confidence=CONFLICT_CONFIDENCE,
                    options=options,
                    as_of_date=as_of_date,
                    available_at=available_at,
                )
            )
        return pd.DataFrame(rows)

    def _candidate_row(
        self,
        *,
        cusip: str,
        security_id: str | None,
        figi: str,
        name: str | None,
        status: str,
        confidence: float,
        options: FigiLoadOptions,
        as_of_date: dt.date,
        available_at: dt.datetime,
    ) -> dict:
        # An unmatched cusip has no target_security_id to route the FIGI onto yet;
        # a conflicting cusip already has a (wrong-to-merge-blindly) candidate target.
        target_security_id = security_id or f"UNRESOLVED-CUSIP-{cusip}"
        return {
            "candidate_id": candidate_id_for(
                source_dataset_id=DATASET_ID,
                source_period=None,
                source_key_type="CUSIP",
                source_key_value=cusip,
                target_security_id=target_security_id,
                match_method=MATCH_METHOD,
            ),
            "source_dataset_id": DATASET_ID,
            "source_table": "security_identifier_history",
            "source_period": None,
            "source_key_type": "CUSIP",
            "source_key_value": cusip,
            "source_security_id": security_id,
            "source_name": name,
            "source_normalized_name": None,
            "target_security_id": target_security_id,
            "target_id_type": "FIGI",
            "target_id_value": figi,
            "target_name": name,
            "target_normalized_name": None,
            "match_method": MATCH_METHOD,
            "confidence": confidence,
            "candidate_status": status,
            "as_of_date": as_of_date,
            "available_at": available_at,
            "details_json": json_dumps({"figi_file": str(options.figi_file), "status_reason": status}),
            "run_id": options.run_id,
        }

    def _build_decisions(self, candidates: pd.DataFrame, *, options: FigiLoadOptions) -> pd.DataFrame:
        decided_at = now_utc_naive()
        rows = []
        for row in candidates.itertuples(index=False):
            rows.append(
                {
                    "decision_id": decision_id_for(row.candidate_id, MATCH_METHOD),
                    "candidate_id": row.candidate_id,
                    "source_dataset_id": row.source_dataset_id,
                    "source_table": row.source_table,
                    "source_period": row.source_period,
                    "source_key_type": row.source_key_type,
                    "source_key_value": row.source_key_value,
                    "source_security_id": row.source_security_id,
                    "target_security_id": row.target_security_id,
                    "target_id_type": row.target_id_type,
                    "target_id_value": row.target_id_value,
                    "match_method": row.match_method,
                    "confidence": row.confidence,
                    "candidate_status": row.candidate_status,
                    "decision_status": "needs_review",
                    "decision_method": MATCH_METHOD,
                    "decided_by": "system:openfigi_cusip_mapping_v1",
                    "decided_at": decided_at,
                    "effective_from": row.as_of_date,
                    "as_of_date": row.as_of_date,
                    "available_at": row.available_at,
                    "notes_json": json_dumps({"candidate_status": row.candidate_status}),
                    "run_id": row.run_id,
                }
            )
        return pd.DataFrame(rows)
