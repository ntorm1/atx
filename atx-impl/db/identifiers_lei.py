from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "GLEIF"
DATASET_ID = "identifiers_lei"

# GLEIF's Registration Authorities list codes the SEC EDGAR CIK scheme as
# RA000453 (https://www.gleif.org/en/about-lei/code-lists/gleif-registration-authorities-list).
# Only rows whose Level-1 RegistrationAuthorityID matches this scheme carry a
# usable, deterministic cik crosswalk; every other RA scheme is dropped.
SEC_REGISTRATION_AUTHORITY_ID = "RA000453"

ALIAS_COLUMNS = [
    "security_id",
    "id_type",
    "id_value",
    "valid_from",
    "valid_to",
    "as_of_date",
    "available_at",
    "source",
    "run_id",
]

PARENT_EDGE_COLUMNS = [
    "child_entity_id",
    "parent_entity_id",
    "relationship_type",
    "valid_from",
    "valid_to",
    "as_of_date",
    "available_at",
    "source",
    "run_id",
]


@dataclass(frozen=True)
class LeiLoadOptions:
    lei_file: Path
    lei_level2_file: Path | None = None
    source: str = SOURCE_NAME
    as_of_date: dt.date | None = None
    run_id: str | None = None


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


def _normalize_cik(value: object) -> str | None:
    text = _normalize_str(value)
    if text is None:
        return None
    digits = text.strip()
    if not digits.isdigit():
        return None
    return digits.zfill(10)


def _parsed_level1_frame(raw: pd.DataFrame) -> pd.DataFrame:
    """Flatten a raw GLEIF Level-1 (LEI-CDF) frame to ``lei, cik, legal_name``.

    ``cik`` is derived from the ``Entity.RegistrationAuthority.*`` columns and is
    ``NA`` whenever the RA scheme is not the SEC EDGAR CIK scheme
    (:data:`SEC_REGISTRATION_AUTHORITY_ID`) or the RA entity id is blank -- every
    LEI record is kept (this is a parse, not a filter); callers that need only
    the usable crosswalk should use :func:`derive_cik_lei_crosswalk`.
    """
    if raw is None or raw.empty:
        return pd.DataFrame(columns=["lei", "cik", "legal_name"])

    ra_id = raw.get("Entity.RegistrationAuthority.RegistrationAuthorityID", pd.Series(dtype=object)).map(
        _normalize_str
    )
    ra_entity_id = raw.get(
        "Entity.RegistrationAuthority.RegistrationAuthorityEntityID", pd.Series(dtype=object)
    )
    cik = ra_entity_id.where(ra_id == SEC_REGISTRATION_AUTHORITY_ID).map(_normalize_cik)
    out = pd.DataFrame(
        {
            "lei": raw.get("LEI", pd.Series(dtype=object)).map(_normalize_str),
            "cik": cik,
            "legal_name": raw.get("Entity.LegalName", pd.Series(dtype=object)).map(_normalize_str),
        }
    )
    return out[out["lei"].notna()].reset_index(drop=True)


def derive_cik_lei_crosswalk(raw: pd.DataFrame) -> pd.DataFrame:
    """Deterministic, offline ``lei <-> cik`` crosswalk from a GLEIF Golden Copy
    Level-1 (LEI-CDF) frame.

    Only rows whose ``Entity.RegistrationAuthority.RegistrationAuthorityID`` is
    the SEC EDGAR CIK scheme (:data:`SEC_REGISTRATION_AUTHORITY_ID`) expose a
    usable US-registration crosswalk; every other RA scheme -- and rows with a
    blank RA entity id -- are dropped rather than guessed at. Pure transform, no
    DuckDB access.
    """
    parsed = _parsed_level1_frame(raw)
    return parsed[parsed["cik"].notna()].reset_index(drop=True)


def parse_gleif_file(path: str | Path) -> pd.DataFrame:
    """Read an injectable GLEIF Golden-Copy Level-1 (LEI-CDF) export.

    Offline CSV ONLY -- the shape of a GLEIF Golden Copy bulk download (or a
    trimmed extract carrying at least ``LEI``, ``Entity.LegalName``, and the
    ``Entity.RegistrationAuthority.*`` columns). Never a network call. Returns a
    flat frame with columns ``lei, cik, legal_name`` (``cik`` is ``NA`` for
    entities with no usable SEC registration-authority crosswalk).
    """
    path = Path(path)
    if path.suffix.lower() != ".csv":
        raise ValueError(f"Unsupported GLEIF Level-1 file extension: {path.suffix!r} ({path})")
    raw = pd.read_csv(path, dtype=str)
    return _parsed_level1_frame(raw)


def parse_gleif_level2_file(path: str | Path) -> pd.DataFrame:
    """Read an injectable GLEIF Golden-Copy Level-2 (relationship) export.

    Offline CSV ONLY. Returns a flat frame with columns
    ``child_lei, parent_lei, relationship_type, valid_from, valid_to`` --
    ``child_lei``/``parent_lei`` are the GLEIF relationship start/end node LEIs
    (the child/subsidiary is the start node, the parent is the end node), dates
    are left as raw strings (parsed downstream in :func:`compute_entity_parent_edges`
    so the pure transform stays testable without a DuckDB dependency).
    """
    path = Path(path)
    if path.suffix.lower() != ".csv":
        raise ValueError(f"Unsupported GLEIF Level-2 file extension: {path.suffix!r} ({path})")
    raw = pd.read_csv(path, dtype=str)
    out = pd.DataFrame(
        {
            "child_lei": raw.get("Relationship.StartNode.NodeID", pd.Series(dtype=object)).map(_normalize_str),
            "parent_lei": raw.get("Relationship.EndNode.NodeID", pd.Series(dtype=object)).map(_normalize_str),
            "relationship_type": raw.get("Relationship.RelationshipType", pd.Series(dtype=object)).map(
                _normalize_str
            ),
            "valid_from": raw.get("Relationship.RelationshipPeriods.StartDate", pd.Series(dtype=object)).map(
                _normalize_str
            ),
            "valid_to": raw.get("Relationship.RelationshipPeriods.EndDate", pd.Series(dtype=object)).map(
                _normalize_str
            ),
        }
    )
    out = out[out["child_lei"].notna() & out["parent_lei"].notna() & out["relationship_type"].notna()]
    return out.reset_index(drop=True)


def compute_lei_alias_rows(
    gleif: pd.DataFrame,
    security_lookup: pd.DataFrame,
    *,
    source: str = SOURCE_NAME,
    run_id: str | None,
    as_of_date: dt.date,
    available_at: dt.datetime,
) -> pd.DataFrame:
    """Pure transform: GLEIF ``lei/cik`` crosswalk frame + resolved
    ``entity_id -> security_id`` lookup in, long ``security_identifier_history``
    ``LEI`` alias frame out.

    ``gleif`` carries one row per LEI with a ``cik`` (as parsed/derived by
    :func:`parse_gleif_file` / :func:`derive_cik_lei_crosswalk`; entities with no
    cik crosswalk are already excluded by the caller). ``security_lookup`` carries
    one row per ``(entity_id, security_id)`` pair currently under that entity --
    LEI is an entity-level identifier, so it fans out to every security_id sharing
    the entity (e.g. multiple share classes). No DuckDB access; safe to unit test
    in isolation.
    """
    if gleif is None or gleif.empty or security_lookup is None or security_lookup.empty:
        return pd.DataFrame(columns=ALIAS_COLUMNS)

    gleif = gleif.copy()
    gleif["entity_id"] = "CIK-" + gleif["cik"].astype(str)
    joined = gleif.merge(security_lookup[["entity_id", "security_id"]], on="entity_id", how="inner")
    if joined.empty:
        return pd.DataFrame(columns=ALIAS_COLUMNS)

    out = pd.DataFrame(
        {
            "security_id": joined["security_id"],
            "id_type": "LEI",
            "id_value": joined["lei"],
            "valid_from": as_of_date,
            "valid_to": pd.NaT,
            "as_of_date": as_of_date,
            "available_at": available_at,
            "source": source,
            "run_id": run_id,
        }
    )
    return out[ALIAS_COLUMNS].drop_duplicates(subset=["security_id", "id_type", "id_value", "source"])


def compute_entity_parent_edges(
    level2: pd.DataFrame,
    lei_entity_lookup: pd.DataFrame,
    *,
    source: str = SOURCE_NAME,
    run_id: str | None,
    available_at: dt.datetime,
) -> pd.DataFrame:
    """Pure transform: GLEIF Level-2 relationship frame + resolved
    ``lei -> entity_id`` lookup in, long entity->entity parent-edge frame out.

    Each relationship carries its OWN GLEIF-reported validity window
    (``valid_from``/``valid_to``, parsed here from the raw ISO-date strings
    :func:`parse_gleif_level2_file` returns); ``available_at`` is the warehouse
    load-time provenance stamp, not GLEIF's window. A relationship referencing a
    LEI with no known ``entity_id`` (its Level-1 record has not been loaded yet)
    is dropped rather than guessed at. No DuckDB access; safe to unit test in
    isolation.
    """
    if level2 is None or level2.empty or lei_entity_lookup is None or lei_entity_lookup.empty:
        return pd.DataFrame(columns=PARENT_EDGE_COLUMNS)

    lookup = lei_entity_lookup[["lei", "entity_id"]].drop_duplicates(subset=["lei"])
    joined = level2.merge(
        lookup.rename(columns={"lei": "child_lei", "entity_id": "child_entity_id"}), on="child_lei", how="inner"
    )
    joined = joined.merge(
        lookup.rename(columns={"lei": "parent_lei", "entity_id": "parent_entity_id"}), on="parent_lei", how="inner"
    )
    if joined.empty:
        return pd.DataFrame(columns=PARENT_EDGE_COLUMNS)

    valid_from = pd.to_datetime(joined["valid_from"], errors="coerce").dt.date
    valid_to = pd.to_datetime(joined["valid_to"], errors="coerce").dt.date
    as_of_date = pd.Timestamp(available_at).date()

    out = pd.DataFrame(
        {
            "child_entity_id": joined["child_entity_id"],
            "parent_entity_id": joined["parent_entity_id"],
            "relationship_type": joined["relationship_type"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "as_of_date": as_of_date,
            "available_at": available_at,
            "source": source,
            "run_id": run_id,
        }
    )
    out = out[out["valid_from"].notna()]
    return out[PARENT_EDGE_COLUMNS].drop_duplicates(
        subset=["child_entity_id", "parent_entity_id", "relationship_type", "valid_from"]
    )


def _entity_security_lookup(store: DuckDBStore, entity_ids: list[str]) -> pd.DataFrame:
    """All (entity_id -> security_id) pairs currently in the identifier spine.

    Mirrors :func:`db.security_master.security_entity_ids_asof`'s ENTITY_ID
    priority (bitemporal history first, current ``securities.entity_id`` only
    for securities with no ENTITY_ID history at all), but inverted -- given a
    set of candidate entity_ids, return every security_id currently under them.
    """
    if not entity_ids:
        return pd.DataFrame(columns=["entity_id", "security_id"])
    lookup = pd.DataFrame({"entity_id": sorted(set(entity_ids))})
    store.con.register("lei_entity_lookup", lookup)
    try:
        rows = store.con.execute(
            """
            WITH history_match AS (
                SELECT DISTINCT h.id_value AS entity_id, h.security_id
                FROM security_identifier_history h
                JOIN lei_entity_lookup l ON l.entity_id = h.id_value
                WHERE h.id_type = 'ENTITY_ID'
                  AND h.valid_to IS NULL
            ),
            current_match AS (
                SELECT DISTINCT s.entity_id, s.security_id
                FROM securities s
                JOIN lei_entity_lookup l ON l.entity_id = s.entity_id
                LEFT JOIN (SELECT DISTINCT security_id FROM security_identifier_history WHERE id_type = 'ENTITY_ID') hp
                  ON hp.security_id = s.security_id
                WHERE hp.security_id IS NULL
            )
            SELECT entity_id, security_id FROM history_match
            UNION
            SELECT entity_id, security_id FROM current_match
            """
        ).fetchall()
    finally:
        store.con.unregister("lei_entity_lookup")
    return pd.DataFrame(rows, columns=["entity_id", "security_id"])


def _lei_entity_lookup_for(store: DuckDBStore, leis: list[str]) -> pd.DataFrame:
    """(lei -> entity_id) pairs already attached in the identifier spine, for
    resolving Level-2 relationship endpoints to entities."""
    if not leis:
        return pd.DataFrame(columns=["lei", "entity_id"])
    lookup = pd.DataFrame({"lei": sorted(set(leis))})
    store.con.register("lei_code_lookup", lookup)
    try:
        rows = store.con.execute(
            """
            SELECT DISTINCT l.lei, h2.id_value AS entity_id
            FROM lei_code_lookup l
            JOIN security_identifier_history h1
              ON h1.id_type = 'LEI' AND h1.id_value = l.lei
            JOIN security_identifier_history h2
              ON h2.security_id = h1.security_id AND h2.id_type = 'ENTITY_ID' AND h2.valid_to IS NULL
            """
        ).fetchall()
    finally:
        store.con.unregister("lei_code_lookup")
    return pd.DataFrame(rows, columns=["lei", "entity_id"])


class LeiAliasDataset(Dataset):
    """Offline GLEIF LEI loader: attaches ``LEI`` aliases to the ``entity_id``
    level and optionally ingests GLEIF Level-2 parent relationships.

    Reads an injectable GLEIF Golden-Copy Level-1 export (``--lei-file``, CSV).
    Rows with a usable SEC EDGAR CIK registration-authority crosswalk resolve
    ``cik -> entity_id`` (see :data:`SEC_REGISTRATION_AUTHORITY_ID`) and get an
    ``LEI`` alias row on every ``security_id`` currently under that entity.
    Entities with no SEC crosswalk, or whose derived entity_id is not present in
    the warehouse yet, are silently skipped -- LEI is an entity-anchor, not a
    matching problem requiring the resolution ledger. Optionally also reads a
    GLEIF Level-2 relationship export (``--lei-level2-file``) and writes
    entity->entity parent edges into ``entity_parent_edges``. No network call.
    """

    dataset_id = DATASET_ID
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: LeiLoadOptions) -> DatasetLoadResult:
        gleif = parse_gleif_file(options.lei_file)
        crosswalk = gleif[gleif["cik"].notna()].reset_index(drop=True)
        as_of_date = options.as_of_date or dt.date.today()
        available_at = pd.Timestamp(as_of_date) + pd.Timedelta(hours=22)

        entity_ids = list(("CIK-" + crosswalk["cik"].astype(str))) if not crosswalk.empty else []
        security_lookup = _entity_security_lookup(store, entity_ids)
        alias_rows = compute_lei_alias_rows(
            crosswalk,
            security_lookup,
            source=options.source,
            run_id=options.run_id,
            as_of_date=as_of_date,
            available_at=available_at,
        )

        parent_edges = pd.DataFrame(columns=PARENT_EDGE_COLUMNS)
        if options.lei_level2_file is not None:
            level2 = parse_gleif_level2_file(options.lei_level2_file)
            leis = list(pd.concat([level2["child_lei"], level2["parent_lei"]], ignore_index=True)) if not level2.empty else []
            # Prefer this run's own Level-1 crosswalk (freshest, and not yet
            # committed to the DB); fall back to LEI aliases already persisted
            # from prior runs for endpoints not present in this file.
            from_this_file = pd.DataFrame(
                {"lei": crosswalk["lei"], "entity_id": "CIK-" + crosswalk["cik"].astype(str)}
            ) if not crosswalk.empty else pd.DataFrame(columns=["lei", "entity_id"])
            from_db = _lei_entity_lookup_for(store, leis)
            lei_entity_lookup = (
                pd.concat([from_this_file, from_db], ignore_index=True)
                .drop_duplicates(subset=["lei"])
                .reset_index(drop=True)
            )
            parent_edges = compute_entity_parent_edges(
                level2,
                lei_entity_lookup,
                source=options.source,
                run_id=options.run_id,
                available_at=available_at,
            )

        alias_rows_written = 0
        parent_edge_rows_written = 0
        with store.transaction():
            if not alias_rows.empty:
                store.con.register("lei_alias_insert", alias_rows)
                try:
                    store.con.execute(
                        """
                        DELETE FROM security_identifier_history
                        USING lei_alias_insert src
                        WHERE security_identifier_history.security_id = src.security_id
                          AND security_identifier_history.id_type = src.id_type
                          AND security_identifier_history.id_value = src.id_value
                          AND security_identifier_history.source = src.source
                        """
                    )
                finally:
                    store.con.unregister("lei_alias_insert")
                alias_rows_written = insert_frame(
                    store, alias_rows, "security_identifier_history", "lei_alias_rows"
                )
            if not parent_edges.empty:
                store.con.register("lei_parent_edge_insert", parent_edges)
                try:
                    store.con.execute(
                        """
                        DELETE FROM entity_parent_edges
                        USING lei_parent_edge_insert src
                        WHERE entity_parent_edges.child_entity_id = src.child_entity_id
                          AND entity_parent_edges.parent_entity_id = src.parent_entity_id
                          AND entity_parent_edges.relationship_type = src.relationship_type
                          AND entity_parent_edges.valid_from = src.valid_from
                          AND entity_parent_edges.source = src.source
                        """
                    )
                finally:
                    store.con.unregister("lei_parent_edge_insert")
                parent_edge_rows_written = insert_frame(
                    store, parent_edges, "entity_parent_edges", "lei_parent_edge_rows"
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
                "lei_file": str(options.lei_file),
                "lei_level2_file": str(options.lei_level2_file) if options.lei_level2_file else None,
                "parent_edge_rows": parent_edge_rows_written,
                "gleif_rows": int(len(gleif)),
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(alias_rows_written),
            source=options.source,
            details={
                "lei_file": str(options.lei_file),
                "lei_level2_file": str(options.lei_level2_file) if options.lei_level2_file else None,
                "gleif_rows": int(len(gleif)),
                "alias_rows": alias_rows_written,
                "parent_edge_rows": parent_edge_rows_written,
            },
        )
