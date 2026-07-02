from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .identifier_decisions import decision_id_for, now_utc_naive
from .identifier_resolution import candidate_id_for
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "GLEIF"
DATASET_ID = "identifiers_lei"
MATCH_METHOD = "gleif_cik_lei"
# A cik that resolves to more than one distinct LEI (lapsed-then-reissued
# registration, LOU duplicate/transfer artifact) is ambiguous -- routed to the
# resolution ledger instead of merged; sub-1.0 confidence since the match
# itself is not clean. Mirrors identifiers_figi.py's CONFLICT_CONFIDENCE.
CONFLICT_CONFIDENCE = 0.5

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


def split_unambiguous_cik_lei_crosswalk(crosswalk: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Split a ``cik <-> lei`` crosswalk into unambiguous vs ambiguous rows.

    A GLEIF Golden Copy is NOT guaranteed 1:1 on ``cik <-> lei``: a
    lapsed-then-reissued registration or an LOU duplicate/transfer artifact can
    carry more than one distinct LEI for the same cik, and in edge cases (fund
    /series-trust, successor CIKs) more than one distinct cik can claim the same
    LEI. Detect both directions via ``groupby(...).nunique()`` -- mirrors
    :func:`identifiers_figi._cusip_security_matches`'s ambiguity check, just
    computed in-memory since the crosswalk (unlike FIGI's cusip match) doesn't
    depend on existing DB state. Returns ``(unambiguous, ambiguous)`` -- rows
    whose cik maps to exactly one lei AND whose lei maps to exactly one cik go
    into ``unambiguous``; every row touching an ambiguous cik or lei goes into
    ``ambiguous`` (both/all of its rows, so every side of the conflict is
    surfaced to the resolution ledger, not just the "extra" ones). Pure
    transform, no DuckDB access.
    """
    if crosswalk is None or crosswalk.empty:
        empty = crosswalk if crosswalk is not None else pd.DataFrame(columns=["lei", "cik", "legal_name"])
        return empty, empty.iloc[0:0]

    cik_lei_counts = crosswalk.groupby("cik")["lei"].nunique()
    lei_cik_counts = crosswalk.groupby("lei")["cik"].nunique()
    ambiguous_ciks = set(cik_lei_counts[cik_lei_counts > 1].index)
    ambiguous_leis = set(lei_cik_counts[lei_cik_counts > 1].index)

    is_ambiguous = crosswalk["cik"].isin(ambiguous_ciks) | crosswalk["lei"].isin(ambiguous_leis)
    ambiguous = crosswalk[is_ambiguous].reset_index(drop=True)
    unambiguous = crosswalk[~is_ambiguous].reset_index(drop=True)
    return unambiguous, ambiguous


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
    cik crosswalk are already excluded by the caller). A GLEIF Golden Copy is NOT
    guaranteed 1:1 on cik<->lei (lapsed-then-reissued registrations, LOU
    duplicate/transfer artifacts can carry >1 LEI per cik); the caller MUST
    already have dropped any cik claimed by more than one distinct LEI (and
    routed it to the resolution ledger instead -- see
    :func:`split_unambiguous_cik_lei_crosswalk` and
    :meth:`LeiAliasDataset._build_candidates`) before calling this function,
    exactly as :func:`identifiers_figi.compute_figi_alias_rows` requires an
    already-unambiguous ``resolution`` frame. ``security_lookup`` carries one
    row per ``(entity_id, security_id)`` pair currently under that entity --
    LEI is an entity-level identifier, so it fans out to every security_id
    sharing the entity (e.g. multiple share classes). No DuckDB access; safe to
    unit test in isolation.
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

    Deliberate divergence: this resolves CURRENT membership only (``valid_to IS
    NULL``), without the ``available_at <= as_of_ts`` no-lookahead guard
    ``security_entity_ids_asof`` applies -- correct here because this is a
    write-path lookup for attaching a new alias to today's entity membership,
    not a bitemporal as-of reader; do not reuse this as a PIT-safe historical
    reader.
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
    matching problem requiring the resolution ledger. A cik claimed by more than
    one distinct LEI (or a LEI claimed by more than one distinct cik) IS a
    matching problem -- the Golden Copy is not a safe 1:1 key -- so those rows
    are never merged blindly; they are routed into
    ``identifier_resolution_candidates``/``identifier_resolution_decisions``
    instead (``decision_status='needs_review'``, mirroring
    ``identifiers_figi.py``'s conflict routing for ambiguous CUSIPs). Optionally
    also reads a GLEIF Level-2 relationship export (``--lei-level2-file``) and
    writes entity->entity parent edges into ``entity_parent_edges``. No network
    call.
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

        # A GLEIF Golden Copy is NOT a safe 1:1 cik<->lei key -- split off any
        # cik claimed by >1 distinct LEI (or LEI claimed by >1 distinct cik)
        # before merging; those go to the resolution ledger, never the alias
        # table. See split_unambiguous_cik_lei_crosswalk.
        unambiguous_crosswalk, ambiguous_crosswalk = split_unambiguous_cik_lei_crosswalk(crosswalk)

        entity_ids = (
            list(("CIK-" + unambiguous_crosswalk["cik"].astype(str))) if not unambiguous_crosswalk.empty else []
        )
        security_lookup = _entity_security_lookup(store, entity_ids)
        alias_rows = compute_lei_alias_rows(
            unambiguous_crosswalk,
            security_lookup,
            source=options.source,
            run_id=options.run_id,
            as_of_date=as_of_date,
            available_at=available_at,
        )

        candidates = self._build_candidates(
            store,
            ambiguous_crosswalk,
            options=options,
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
        candidate_rows_written = 0
        decision_rows_written = 0
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
            if not candidates.empty:
                store.con.execute(
                    "DELETE FROM identifier_resolution_candidates WHERE source_dataset_id = ? AND match_method = ?",
                    [DATASET_ID, MATCH_METHOD],
                )
                candidate_rows_written = insert_frame(
                    store, candidates, "identifier_resolution_candidates", "lei_candidate_rows"
                )
                decisions = self._build_decisions(candidates, options=options)
                store.con.execute(
                    "DELETE FROM identifier_resolution_decisions WHERE source_dataset_id = ? AND decision_method = ?",
                    [DATASET_ID, MATCH_METHOD],
                )
                decision_rows_written = insert_frame(
                    store, decisions, "identifier_resolution_decisions", "lei_decision_rows"
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
                "candidate_rows": candidate_rows_written,
                "decision_rows": decision_rows_written,
                "ambiguous_ciks": int(ambiguous_crosswalk["cik"].nunique()) if not ambiguous_crosswalk.empty else 0,
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
                "candidate_rows": candidate_rows_written,
                "decision_rows": decision_rows_written,
                "ambiguous_ciks": int(ambiguous_crosswalk["cik"].nunique()) if not ambiguous_crosswalk.empty else 0,
            },
        )

    def _build_candidates(
        self,
        store: DuckDBStore,
        ambiguous_crosswalk: pd.DataFrame,
        *,
        options: LeiLoadOptions,
        as_of_date: dt.date,
        available_at: dt.datetime,
    ) -> pd.DataFrame:
        """One candidate row per ambiguous (cik, lei, security_id) triple -- every
        LEI a conflicting cik claims, on every security_id currently under that
        cik's entity, gets its own row so the full conflict is visible to a
        reviewer (mirrors identifiers_figi.py's per-match conflict candidate
        rows, where ``target_security_id`` is always a real existing
        ``security_id``, never a placeholder -- an ambiguous cik is a conflict
        among known entities/securities, not a no-match problem like FIGI's
        unmatched cusip case).

        A cik whose entity has no security_id in the warehouse yet still can't
        be routed to a real target_security_id; those rows are dropped (same
        "silently skipped, not a resolution-ledger problem" treatment the
        unambiguous path already gives unmatched entities), since there is no
        row shape to safely route a fully-fictitious security to.
        """
        if ambiguous_crosswalk is None or ambiguous_crosswalk.empty:
            return pd.DataFrame()

        crosswalk = ambiguous_crosswalk.copy()
        crosswalk["entity_id"] = "CIK-" + crosswalk["cik"].astype(str)
        entity_ids = list(crosswalk["entity_id"].unique())
        security_lookup = _entity_security_lookup(store, entity_ids)
        joined = crosswalk.merge(security_lookup[["entity_id", "security_id"]], on="entity_id", how="inner")

        rows: list[dict] = []
        for row in joined.itertuples(index=False):
            rows.append(
                self._candidate_row(
                    cik=row.cik,
                    lei=row.lei,
                    security_id=row.security_id,
                    name=row.legal_name,
                    options=options,
                    as_of_date=as_of_date,
                    available_at=available_at,
                )
            )
        return pd.DataFrame(rows)

    def _candidate_row(
        self,
        *,
        cik: str,
        lei: str,
        security_id: str,
        name: str | None,
        options: LeiLoadOptions,
        as_of_date: dt.date,
        available_at: dt.datetime,
    ) -> dict:
        return {
            "candidate_id": candidate_id_for(
                source_dataset_id=DATASET_ID,
                source_period=None,
                source_key_type="CIK",
                source_key_value=cik,
                target_security_id=security_id,
                match_method=f"{MATCH_METHOD}:{lei}",
            ),
            "source_dataset_id": DATASET_ID,
            "source_table": "security_identifier_history",
            "source_period": None,
            "source_key_type": "CIK",
            "source_key_value": cik,
            "source_security_id": security_id,
            "source_name": name,
            "source_normalized_name": None,
            "target_security_id": security_id,
            "target_id_type": "LEI",
            "target_id_value": lei,
            "target_name": name,
            "target_normalized_name": None,
            "match_method": MATCH_METHOD,
            "confidence": CONFLICT_CONFIDENCE,
            "candidate_status": "conflict",
            "as_of_date": as_of_date,
            "available_at": available_at,
            "details_json": json_dumps({"lei_file": str(options.lei_file), "status_reason": "ambiguous_cik_lei"}),
            "run_id": options.run_id,
        }

    def _build_decisions(self, candidates: pd.DataFrame, *, options: LeiLoadOptions) -> pd.DataFrame:
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
                    "decided_by": "system:gleif_cik_lei_mapping_v1",
                    "decided_at": decided_at,
                    "effective_from": row.as_of_date,
                    "as_of_date": row.as_of_date,
                    "available_at": row.available_at,
                    "notes_json": json_dumps({"candidate_status": row.candidate_status}),
                    "run_id": row.run_id,
                }
            )
        return pd.DataFrame(rows)
