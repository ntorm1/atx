"""Tier1-S2 T4: deterministic alias-candidate mining (research only).

Given the observed companyfacts corpus and the FASB calculation linkbase, rank
every us-gaap concept against every canonical registry item so a curator can
pick the alias rows to add to the seeds. This module NEVER writes a rule, an
alias, or a seed. Its only artifact is research/alias_candidates.csv.

TF-IDF and cosine similarity are implemented here in ~40 lines of stdlib
arithmetic: the project has no sklearn dependency and does not want one for a
research script.
"""
from __future__ import annotations

import csv
import math
import re
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from .connection import DuckDBStore

ALIAS_CANDIDATE_COLUMNS = (
    "item_id",
    "canonical_code",
    "rank",
    "label_similarity",
    "taxonomy",
    "concept",
    "filer_count",
    "fact_count",
    "first_fiscal_year",
    "last_fiscal_year",
    "parent_concept",
    "statement_placement",
    "already_mapped",
)

_CAMEL_SPLIT = re.compile(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+")
_STOPWORDS = frozenset(
    {
        "and", "of", "the", "to", "for", "from", "in", "on", "at", "by", "a", "an",
        "or", "with", "per", "its", "other", "abstract", "member", "axis", "domain",
    }
)


@dataclass(frozen=True)
class AliasMiningOptions:
    minimum_filer_count: int = 5
    minimum_fact_count: int = 1
    top_n: int = 25
    minimum_similarity: float = 0.0
    taxonomies: tuple[str, ...] = ("us-gaap", "dei")


@dataclass(frozen=True)
class ConceptProfile:
    taxonomy: str
    concept: str
    filer_count: int
    fact_count: int
    first_fiscal_year: int | None
    last_fiscal_year: int | None
    parent_concept: str | None
    statement_placement: str | None
    labels: tuple[str, ...]


@dataclass(frozen=True)
class AliasCandidate:
    item_id: int
    canonical_code: str
    rank: int
    label_similarity: float
    taxonomy: str
    concept: str
    filer_count: int
    fact_count: int
    first_fiscal_year: int | None
    last_fiscal_year: int | None
    parent_concept: str | None
    statement_placement: str | None
    already_mapped: bool


def tokenize_identifier(value: str) -> tuple[str, ...]:
    """Split CamelCase or free text into lowercase content tokens."""

    tokens: list[str] = []
    for chunk in re.split(r"[^A-Za-z0-9]+", value or ""):
        for part in _CAMEL_SPLIT.findall(chunk):
            token = part.lower()
            if token and token not in _STOPWORDS and not token.isdigit():
                tokens.append(token)
    return tuple(tokens)


def inverse_document_frequency(documents: Sequence[Sequence[str]]) -> dict[str, float]:
    """Smoothed IDF over tokenized documents; deterministic for a fixed input."""

    total = len(documents)
    counts: dict[str, int] = {}
    for document in documents:
        for token in set(document):
            counts[token] = counts.get(token, 0) + 1
    return {token: math.log((1.0 + total) / (1.0 + count)) + 1.0 for token, count in sorted(counts.items())}


def tf_idf_vector(tokens: Sequence[str], idf: Mapping[str, float]) -> dict[str, float]:
    """L2-normalized TF-IDF vector; unknown tokens are dropped."""

    if not tokens:
        return {}
    raw: dict[str, float] = {}
    for token in tokens:
        weight = idf.get(token)
        if weight is None:
            continue
        raw[token] = raw.get(token, 0.0) + weight
    norm = math.sqrt(sum(value * value for value in raw.values()))
    if norm == 0.0:
        return {}
    return {token: value / norm for token, value in sorted(raw.items())}


def cosine_similarity(left: Mapping[str, float], right: Mapping[str, float]) -> float:
    """Dot product of two already-normalized sparse vectors."""

    if len(left) > len(right):
        left, right = right, left
    return sum(value * right.get(token, 0.0) for token, value in left.items())


def load_concept_profiles(
    store: DuckDBStore,
    options: AliasMiningOptions | None = None,
) -> tuple[ConceptProfile, ...]:
    """Per-concept filer/fact/year counts, calculation parent, statement placement."""

    options = options or AliasMiningOptions()
    frame = store.con.execute(
        """
        WITH observed AS (
            SELECT
                f.taxonomy,
                f.concept,
                count(DISTINCT f.cik) AS filer_count,
                count(*) AS fact_count,
                min(f.fiscal_year) AS first_fiscal_year,
                max(f.fiscal_year) AS last_fiscal_year,
                list_sort(list_distinct(list(f.label) FILTER (WHERE f.label IS NOT NULL))) AS labels
            FROM sec_company_facts f
            WHERE f.taxonomy = ANY(?)
            GROUP BY f.taxonomy, f.concept
        ),
        arcs AS (
            SELECT
                child_taxonomy AS taxonomy,
                child_concept AS concept,
                min(parent_concept) AS parent_concept
            FROM xbrl_taxonomy_relationships
            WHERE linkbase_type = 'calculation'
            GROUP BY child_taxonomy, child_concept
        ),
        placement AS (
            SELECT taxonomy, concept, min(statement_type) AS statement_type
            FROM fundamental_statement_map
            WHERE is_active
            GROUP BY taxonomy, concept
        )
        SELECT
            o.taxonomy,
            o.concept,
            o.filer_count,
            o.fact_count,
            o.first_fiscal_year,
            o.last_fiscal_year,
            a.parent_concept,
            coalesce(self_place.statement_type, parent_place.statement_type) AS statement_placement,
            o.labels
        FROM observed o
        LEFT JOIN arcs a ON a.taxonomy = o.taxonomy AND a.concept = o.concept
        LEFT JOIN placement self_place ON self_place.taxonomy = o.taxonomy AND self_place.concept = o.concept
        LEFT JOIN placement parent_place ON parent_place.taxonomy = o.taxonomy AND parent_place.concept = a.parent_concept
        WHERE o.filer_count >= ? AND o.fact_count >= ?
        ORDER BY o.taxonomy, o.concept
        """,
        [list(options.taxonomies), options.minimum_filer_count, options.minimum_fact_count],
    ).fetchall()
    return tuple(
        ConceptProfile(
            taxonomy=str(row[0]),
            concept=str(row[1]),
            filer_count=int(row[2]),
            fact_count=int(row[3]),
            first_fiscal_year=None if row[4] is None else int(row[4]),
            last_fiscal_year=None if row[5] is None else int(row[5]),
            parent_concept=None if row[6] is None else str(row[6]),
            statement_placement=None if row[7] is None else str(row[7]),
            labels=tuple(str(label) for label in (row[8] or ())),
        )
        for row in frame
    )


def load_item_label_sets(store: DuckDBStore) -> dict[int, tuple[str, tuple[str, ...]]]:
    """item_id -> (canonical_code, label strings) from the registry and the map."""

    rows = store.con.execute(
        """
        SELECT
            i.item_id,
            i.canonical_code,
            list_sort(list_distinct(
                list(i.canonical_code)
                || list(coalesce(i.definition, ''))
                || coalesce(list(m.canonical_label) FILTER (WHERE m.canonical_label IS NOT NULL), [])
                || coalesce(list(m.canonical_metric) FILTER (WHERE m.canonical_metric IS NOT NULL), [])
            )) AS labels
        FROM fundamental_item i
        LEFT JOIN fundamental_statement_map m ON m.item_id = i.item_id AND m.is_active
        WHERE i.item_id < 2000
        GROUP BY i.item_id, i.canonical_code
        ORDER BY i.item_id
        """
    ).fetchall()
    return {
        int(row[0]): (str(row[1]), tuple(str(label) for label in (row[2] or ()) if str(label).strip()))
        for row in rows
    }


def load_existing_alias_owners(store: DuckDBStore) -> dict[tuple[str, str], int]:
    """(alias_scheme, alias_code) -> item_id for aliases already curated."""

    rows = store.con.execute(
        """
        SELECT alias_scheme, alias_code, min(item_id)
        FROM fundamental_item_alias
        GROUP BY alias_scheme, alias_code
        UNION
        SELECT taxonomy, concept, min(item_id)
        FROM fundamental_statement_map
        WHERE is_active AND item_id IS NOT NULL
        GROUP BY taxonomy, concept
        """
    ).fetchall()
    owners: dict[tuple[str, str], int] = {}
    for scheme, code, item_id in rows:
        owners.setdefault((str(scheme), str(code)), int(item_id))
    return owners


def score_alias_candidates(
    profiles: Iterable[ConceptProfile],
    item_labels: Mapping[int, tuple[str, tuple[str, ...]]],
    *,
    existing_alias_owners: Mapping[tuple[str, str], int],
    top_n: int = 25,
    minimum_similarity: float = 0.0,
) -> tuple[AliasCandidate, ...]:
    """Pure ranking: cosine(concept tokens, item label tokens) per item."""

    profile_list = sorted(profiles, key=lambda p: (p.taxonomy, p.concept))
    concept_tokens = {
        (p.taxonomy, p.concept): tokenize_identifier(p.concept) + tuple(
            token for label in p.labels for token in tokenize_identifier(label)
        )
        for p in profile_list
    }
    item_tokens = {
        item_id: tuple(token for label in labels for token in tokenize_identifier(label))
        for item_id, (_, labels) in sorted(item_labels.items())
    }
    documents = [*concept_tokens.values(), *item_tokens.values()]
    idf = inverse_document_frequency(documents)
    concept_vectors = {key: tf_idf_vector(tokens, idf) for key, tokens in concept_tokens.items()}
    item_vectors = {item_id: tf_idf_vector(tokens, idf) for item_id, tokens in item_tokens.items()}

    candidates: list[AliasCandidate] = []
    for item_id in sorted(item_vectors):
        canonical_code = item_labels[item_id][0]
        scored: list[tuple[float, ConceptProfile]] = []
        for profile in profile_list:
            similarity = cosine_similarity(
                item_vectors[item_id], concept_vectors[(profile.taxonomy, profile.concept)]
            )
            if similarity < minimum_similarity:
                continue
            scored.append((similarity, profile))
        scored.sort(key=lambda pair: (-round(pair[0], 12), -pair[1].filer_count, pair[1].concept))
        for rank, (similarity, profile) in enumerate(scored[:top_n], start=1):
            owner = existing_alias_owners.get((profile.taxonomy, profile.concept))
            candidates.append(
                AliasCandidate(
                    item_id=item_id,
                    canonical_code=canonical_code,
                    rank=rank,
                    label_similarity=round(similarity, 6),
                    taxonomy=profile.taxonomy,
                    concept=profile.concept,
                    filer_count=profile.filer_count,
                    fact_count=profile.fact_count,
                    first_fiscal_year=profile.first_fiscal_year,
                    last_fiscal_year=profile.last_fiscal_year,
                    parent_concept=profile.parent_concept,
                    statement_placement=profile.statement_placement,
                    already_mapped=owner == item_id,
                )
            )
    return tuple(candidates)


def mine_alias_candidates(
    store: DuckDBStore,
    options: AliasMiningOptions | None = None,
) -> tuple[AliasCandidate, ...]:
    """Load, score, and rank. Read-only against the warehouse."""

    options = options or AliasMiningOptions()
    return score_alias_candidates(
        load_concept_profiles(store, options),
        load_item_label_sets(store),
        existing_alias_owners=load_existing_alias_owners(store),
        top_n=options.top_n,
        minimum_similarity=options.minimum_similarity,
    )


def write_alias_candidates(candidates: Iterable[AliasCandidate], path: Path | str) -> int:
    """Write the ranked candidates as CSV; returns the row count."""

    rows = list(candidates)
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(ALIAS_CANDIDATE_COLUMNS)
        for candidate in rows:
            writer.writerow(
                [
                    candidate.item_id,
                    candidate.canonical_code,
                    candidate.rank,
                    f"{candidate.label_similarity:.6f}",
                    candidate.taxonomy,
                    candidate.concept,
                    candidate.filer_count,
                    candidate.fact_count,
                    "" if candidate.first_fiscal_year is None else candidate.first_fiscal_year,
                    "" if candidate.last_fiscal_year is None else candidate.last_fiscal_year,
                    "" if candidate.parent_concept is None else candidate.parent_concept,
                    "" if candidate.statement_placement is None else candidate.statement_placement,
                    "true" if candidate.already_mapped else "false",
                ]
            )
    return len(rows)
