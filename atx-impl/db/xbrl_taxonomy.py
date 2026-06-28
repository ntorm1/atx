from __future__ import annotations

import datetime as dt
import hashlib
import io
import re
import zipfile
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlparse
from xml.etree import ElementTree as ET

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, now_utc_naive, quality_check, record_source_file


DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS = (
    "https://xbrl.fasb.org/us-gaap/2026/us-gaap-2026.zip",
    "https://xbrl.fasb.org/srt/2026/srt-2026.zip",
)
SOURCE_NAME = "FASB/XBRL taxonomy packages"
XLINK = "{http://www.w3.org/1999/xlink}"
XBRLDT = "{http://xbrl.org/2005/xbrldt}"
LINK_TAG_TYPES = {
    "presentationLink": "presentation",
    "calculationLink": "calculation",
    "definitionLink": "definition",
}
ARC_TAG_TYPES = {
    "presentationArc": "presentation",
    "calculationArc": "calculation",
    "definitionArc": "definition",
}
FRAME_RE = re.compile(r"^CY(?P<year>\d{4})(?:Q(?P<quarter>[1-4]))?(?P<instant>I)?$")


@dataclass(frozen=True)
class XbrlTaxonomyOptions:
    package_urls: tuple[str, ...] = DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS
    request_timeout: int = 120
    user_agent: str = "atx-impl XBRL taxonomy loader nathan.tormaschy@gmail.com"
    run_id: str | None = None


@dataclass(frozen=True)
class TaxonomyPackage:
    taxonomy_package_id: str
    taxonomy: str
    release_year: int
    source_url: str
    package_sha256: str
    byte_count: int
    file_count: int
    linkbase_file_count: int
    relationship_count: int
    source_loaded_at: dt.datetime


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _xlink(attrs: dict[str, Any], name: str) -> str | None:
    return attrs.get(f"{XLINK}{name}") or attrs.get(name)


def _xbrldt(attrs: dict[str, Any], name: str) -> str | None:
    return attrs.get(f"{XBRLDT}{name}") or attrs.get(name)


def _float_or_none(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _int_or_none(value: Any) -> int | None:
    if value in (None, ""):
        return None
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def _bool_or_none(value: Any) -> bool | None:
    if value in (None, ""):
        return None
    normalized = str(value).strip().lower()
    if normalized in {"true", "1"}:
        return True
    if normalized in {"false", "0"}:
        return False
    return None


def _package_id_from_url(url: str) -> str:
    name = urlparse(url).path.rsplit("/", 1)[-1]
    return name.removesuffix(".zip") or "xbrl-taxonomy"


def _taxonomy_from_package(package_id: str) -> str:
    if package_id.startswith("srt"):
        return "srt"
    if package_id.startswith("us-gaap"):
        return "us-gaap"
    return package_id.split("-", 1)[0]


def _release_year(package_id: str) -> int:
    match = re.search(r"(20\d{2})", package_id)
    return int(match.group(1)) if match else 0


def _concept_from_href(href: str | None) -> tuple[str | None, str | None]:
    if not href or "#" not in href:
        return None, None
    fragment = href.rsplit("#", 1)[-1]
    if "_" not in fragment:
        return None, fragment or None
    taxonomy, concept = fragment.split("_", 1)
    return taxonomy or None, concept or None


def _concept_kind(concept: str | None) -> str | None:
    if not concept:
        return None
    if concept.endswith("Axis"):
        return "axis"
    if concept.endswith("Domain"):
        return "domain"
    if concept.endswith("Member"):
        return "member"
    if concept.endswith("Table"):
        return "table"
    if concept.endswith("LineItems"):
        return "line_items"
    if concept.endswith("Abstract"):
        return "abstract"
    return "concept"


def _relationship_kind(arcrole: str | None) -> str:
    if not arcrole:
        return "relationship"
    return arcrole.rstrip("/").rsplit("/", 1)[-1].replace("-", "_")


def _relationship_id(row: dict[str, Any]) -> str:
    parts = [
        row.get("taxonomy_package_id"),
        row.get("source_file"),
        row.get("linkbase_type"),
        row.get("role_uri"),
        row.get("arcrole"),
        row.get("from_label"),
        row.get("to_label"),
        row.get("parent_taxonomy"),
        row.get("parent_concept"),
        row.get("child_taxonomy"),
        row.get("child_concept"),
        row.get("order_value"),
        row.get("weight"),
        row.get("preferred_label"),
        row.get("target_role"),
    ]
    return hashlib.sha256("|".join("" if value is None else str(value) for value in parts).encode("utf-8")).hexdigest()


def _dimension_edge_id(relationship_id: str) -> str:
    return hashlib.sha256(f"xbrl_dimension_edge|{relationship_id}".encode("utf-8")).hexdigest()


def _fact_frame_id(row: dict[str, Any]) -> str:
    parts = [row.get("source"), row.get("taxonomy"), row.get("concept"), row.get("unit"), row.get("frame")]
    return hashlib.sha256("|".join("" if value is None else str(value) for value in parts).encode("utf-8")).hexdigest()


def _is_dimension_relationship(row: dict[str, Any]) -> bool:
    if row.get("linkbase_type") != "definition":
        return False
    arcrole = str(row.get("arcrole") or "")
    if "/dim/" in arcrole or "dimension" in arcrole:
        return True
    kinds = {row.get("parent_concept_kind"), row.get("child_concept_kind")}
    return bool(kinds & {"axis", "domain", "member", "table", "line_items"})


def _download_package(url: str, *, timeout: int, user_agent: str) -> tuple[bytes, str]:
    response = requests.get(url, timeout=timeout, headers={"User-Agent": user_agent})
    response.raise_for_status()
    content = response.content
    return content, hashlib.sha256(content).hexdigest()


def _parse_package(
    *,
    content: bytes,
    source_url: str,
    package_sha256: str,
    loaded_at: dt.datetime,
    observed_concepts: set[tuple[str, str]],
) -> tuple[TaxonomyPackage, list[dict[str, Any]]]:
    package_id = _package_id_from_url(source_url)
    taxonomy = _taxonomy_from_package(package_id)
    release_year = _release_year(package_id)
    relationships: list[dict[str, Any]] = []
    linkbase_file_count = 0

    with zipfile.ZipFile(io.BytesIO(content)) as archive:
        names = archive.namelist()
        for name in names:
            lower = name.lower()
            if not lower.endswith((".xml", ".xsd")):
                continue
            if not any(token in lower for token in ("-pre-", "-cal-", "-def-")):
                continue
            try:
                root = ET.fromstring(archive.read(name))
            except ET.ParseError:
                continue
            file_relationship_count = 0
            role_hrefs = {
                elem.attrib.get("roleURI"): _xlink(elem.attrib, "href")
                for elem in root.iter()
                if _local_name(elem.tag) == "roleRef" and elem.attrib.get("roleURI")
            }
            for link in root.iter():
                linkbase_type = LINK_TAG_TYPES.get(_local_name(link.tag))
                if not linkbase_type:
                    continue
                role_uri = _xlink(link.attrib, "role")
                role_name = role_uri.rstrip("/").rsplit("/", 1)[-1] if role_uri else None
                locs: dict[str, str] = {}
                for child in list(link):
                    if _local_name(child.tag) == "loc":
                        label = _xlink(child.attrib, "label")
                        href = _xlink(child.attrib, "href")
                        if label and href:
                            locs[label] = href
                for child in list(link):
                    arc_type = ARC_TAG_TYPES.get(_local_name(child.tag))
                    if arc_type != linkbase_type:
                        continue
                    from_label = _xlink(child.attrib, "from")
                    to_label = _xlink(child.attrib, "to")
                    parent_href = locs.get(from_label or "")
                    child_href = locs.get(to_label or "")
                    parent_taxonomy, parent_concept = _concept_from_href(parent_href)
                    child_taxonomy, child_concept = _concept_from_href(child_href)
                    if not parent_concept or not child_concept:
                        continue
                    row = {
                        "relationship_id": None,
                        "taxonomy_package_id": package_id,
                        "taxonomy": taxonomy,
                        "release_year": release_year,
                        "linkbase_type": linkbase_type,
                        "source_file": name,
                        "role_uri": role_uri,
                        "role_name": role_name,
                        "role_href": role_hrefs.get(role_uri),
                        "arcrole": _xlink(child.attrib, "arcrole"),
                        "from_label": from_label,
                        "to_label": to_label,
                        "parent_href": parent_href,
                        "parent_taxonomy": parent_taxonomy,
                        "parent_concept": parent_concept,
                        "parent_concept_kind": _concept_kind(parent_concept),
                        "child_href": child_href,
                        "child_taxonomy": child_taxonomy,
                        "child_concept": child_concept,
                        "child_concept_kind": _concept_kind(child_concept),
                        "order_value": _float_or_none(child.attrib.get("order")),
                        "weight": _float_or_none(child.attrib.get("weight")),
                        "priority": _int_or_none(child.attrib.get("priority")),
                        "preferred_label": _xlink(child.attrib, "preferredLabel") or child.attrib.get("preferredLabel"),
                        "use": child.attrib.get("use"),
                        "closed": _bool_or_none(_xbrldt(child.attrib, "closed")),
                        "context_element": _xbrldt(child.attrib, "contextElement"),
                        "usable": _bool_or_none(_xbrldt(child.attrib, "usable")),
                        "target_role": _xlink(child.attrib, "targetRole") or child.attrib.get("targetRole"),
                        "touches_observed_concept": (
                            (parent_taxonomy, parent_concept) in observed_concepts
                            or (child_taxonomy, child_concept) in observed_concepts
                        ),
                        "source_url": source_url,
                        "source_loaded_at": loaded_at,
                    }
                    row["relationship_id"] = _relationship_id(row)
                    relationships.append(row)
                    file_relationship_count += 1
            if file_relationship_count:
                linkbase_file_count += 1

    package = TaxonomyPackage(
        taxonomy_package_id=package_id,
        taxonomy=taxonomy,
        release_year=release_year,
        source_url=source_url,
        package_sha256=package_sha256,
        byte_count=len(content),
        file_count=len(names),
        linkbase_file_count=linkbase_file_count,
        relationship_count=len(relationships),
        source_loaded_at=loaded_at,
    )
    return package, relationships


def _roles_frame(relationships: pd.DataFrame) -> pd.DataFrame:
    columns = [
        "role_id",
        "taxonomy_package_id",
        "taxonomy",
        "release_year",
        "role_uri",
        "role_name",
        "role_href",
        "linkbase_type",
        "source_file",
        "relationship_count",
        "source_url",
        "source_loaded_at",
    ]
    if relationships.empty:
        return pd.DataFrame(columns=columns)
    grouped = (
        relationships.groupby(
            [
                "taxonomy_package_id",
                "taxonomy",
                "release_year",
                "role_uri",
                "role_name",
                "role_href",
                "linkbase_type",
                "source_file",
                "source_url",
                "source_loaded_at",
            ],
            dropna=False,
            sort=True,
        )
        .size()
        .reset_index(name="relationship_count")
    )
    grouped["role_id"] = grouped.apply(
        lambda row: hashlib.sha256(
            "|".join(
                "" if row[column] is None else str(row[column])
                for column in ("taxonomy_package_id", "role_uri", "linkbase_type", "source_file")
            ).encode("utf-8")
        ).hexdigest(),
        axis=1,
    )
    return grouped[columns]


def _dimension_edges_frame(relationships: pd.DataFrame) -> pd.DataFrame:
    columns = [
        "dimension_edge_id",
        "relationship_id",
        "taxonomy_package_id",
        "taxonomy",
        "release_year",
        "role_uri",
        "role_name",
        "source_file",
        "relationship_kind",
        "arcrole",
        "source_taxonomy",
        "source_concept",
        "source_concept_kind",
        "target_taxonomy",
        "target_concept",
        "target_concept_kind",
        "order_value",
        "context_element",
        "closed",
        "usable",
        "target_role",
        "touches_observed_concept",
        "source_url",
        "source_loaded_at",
    ]
    if relationships.empty:
        return pd.DataFrame(columns=columns)
    mask = relationships.apply(lambda row: _is_dimension_relationship(row.to_dict()), axis=1)
    if not mask.any():
        return pd.DataFrame(columns=columns)
    frame = relationships.loc[mask].copy()
    frame["dimension_edge_id"] = frame["relationship_id"].map(_dimension_edge_id)
    frame["relationship_kind"] = frame["arcrole"].map(_relationship_kind)
    frame["source_taxonomy"] = frame["parent_taxonomy"]
    frame["source_concept"] = frame["parent_concept"]
    frame["source_concept_kind"] = frame["parent_concept_kind"]
    frame["target_taxonomy"] = frame["child_taxonomy"]
    frame["target_concept"] = frame["child_concept"]
    frame["target_concept_kind"] = frame["child_concept_kind"]
    return frame[columns].drop_duplicates(subset=["dimension_edge_id"])


def _parse_sec_frame(frame: str) -> dict[str, Any]:
    match = FRAME_RE.match(frame or "")
    if not match:
        return {
            "frame_year": None,
            "frame_quarter": None,
            "frame_period": "unknown",
            "is_instant": None,
        }
    quarter = match.group("quarter")
    is_instant = bool(match.group("instant"))
    frame_period = "instant" if is_instant else ("quarter_duration" if quarter else "annual_duration")
    return {
        "frame_year": int(match.group("year")),
        "frame_quarter": int(quarter) if quarter else None,
        "frame_period": frame_period,
        "is_instant": is_instant,
    }


def refresh_xbrl_fact_frames(store: DuckDBStore) -> int:
    facts = store.con.execute(
        """
        SELECT
            source,
            taxonomy,
            concept,
            unit,
            frame,
            security_id,
            accession_number,
            period_start,
            period_end,
            filed_date,
            available_at,
            source_loaded_at
        FROM sec_company_facts
        WHERE frame IS NOT NULL
          AND frame <> ''
          AND taxonomy IS NOT NULL
          AND taxonomy <> ''
          AND concept IS NOT NULL
          AND concept <> ''
          AND unit IS NOT NULL
          AND unit <> ''
        """
    ).df()
    columns = [
        "fact_frame_id",
        "source",
        "taxonomy",
        "concept",
        "unit",
        "frame",
        "frame_year",
        "frame_quarter",
        "frame_period",
        "is_instant",
        "fact_count",
        "security_count",
        "accession_count",
        "first_period_start",
        "last_period_end",
        "first_filed_date",
        "last_filed_date",
        "first_available_at",
        "last_available_at",
        "latest_source_loaded_at",
    ]
    if facts.empty:
        frame = pd.DataFrame(columns=columns)
    else:
        rows: list[dict[str, Any]] = []
        grouped = facts.groupby(["source", "taxonomy", "concept", "unit", "frame"], dropna=False, sort=True)
        for (source, taxonomy, concept, unit, sec_frame), group in grouped:
            parsed = _parse_sec_frame(str(sec_frame))
            row = {
                "source": source,
                "taxonomy": taxonomy,
                "concept": concept,
                "unit": unit,
                "frame": sec_frame,
                **parsed,
                "fact_count": int(len(group)),
                "security_count": int(group["security_id"].nunique()),
                "accession_count": int(group["accession_number"].nunique()),
                "first_period_start": group["period_start"].min(),
                "last_period_end": group["period_end"].max(),
                "first_filed_date": group["filed_date"].min(),
                "last_filed_date": group["filed_date"].max(),
                "first_available_at": group["available_at"].min(),
                "last_available_at": group["available_at"].max(),
                "latest_source_loaded_at": group["source_loaded_at"].max(),
            }
            row["fact_frame_id"] = _fact_frame_id(row)
            rows.append(row)
        frame = pd.DataFrame(rows)[columns]

    with store.transaction():
        store.con.execute("DELETE FROM xbrl_fact_frames")
        if not frame.empty:
            insert_frame(store, frame, "xbrl_fact_frames", "xbrl_fact_frames_insert")
    return int(len(frame))


def refresh_xbrl_taxonomy(store: DuckDBStore, options: XbrlTaxonomyOptions | None = None) -> dict[str, int]:
    options = options or XbrlTaxonomyOptions()
    observed_rows = store.con.execute(
        """
        SELECT DISTINCT taxonomy, concept
        FROM xbrl_concept_catalog
        WHERE taxonomy IS NOT NULL
          AND taxonomy <> ''
          AND concept IS NOT NULL
          AND concept <> ''
        """
    ).fetchall()
    observed_concepts = {(str(taxonomy), str(concept)) for taxonomy, concept in observed_rows}
    loaded_at = now_utc_naive()

    packages: list[TaxonomyPackage] = []
    relationships: list[dict[str, Any]] = []
    for url in options.package_urls:
        content, package_sha256 = _download_package(
            url,
            timeout=options.request_timeout,
            user_agent=options.user_agent,
        )
        record_source_file(
            store,
            dataset_id="xbrl_taxonomy",
            source_url=url,
            status="fetched",
            sha256=package_sha256,
            metadata={"byte_count": len(content), "run_id": options.run_id},
        )
        package, package_relationships = _parse_package(
            content=content,
            source_url=url,
            package_sha256=package_sha256,
            loaded_at=loaded_at,
            observed_concepts=observed_concepts,
        )
        packages.append(package)
        relationships.extend(package_relationships)

    package_frame = pd.DataFrame([package.__dict__ for package in packages])
    relationship_frame = pd.DataFrame(relationships)
    if not relationship_frame.empty:
        relationship_frame = relationship_frame.drop_duplicates(subset=["relationship_id"])
    roles_frame = _roles_frame(relationship_frame)
    dimension_frame = _dimension_edges_frame(relationship_frame)

    with store.transaction():
        store.con.execute("DELETE FROM xbrl_dimension_edges")
        store.con.execute("DELETE FROM xbrl_taxonomy_relationships")
        store.con.execute("DELETE FROM xbrl_taxonomy_roles")
        store.con.execute("DELETE FROM xbrl_taxonomy_packages")
        if not package_frame.empty:
            insert_frame(store, package_frame, "xbrl_taxonomy_packages", "xbrl_taxonomy_packages_insert")
        if not roles_frame.empty:
            insert_frame(store, roles_frame, "xbrl_taxonomy_roles", "xbrl_taxonomy_roles_insert")
        if not relationship_frame.empty:
            insert_frame(
                store,
                relationship_frame,
                "xbrl_taxonomy_relationships",
                "xbrl_taxonomy_relationships_insert",
            )
        if not dimension_frame.empty:
            insert_frame(store, dimension_frame, "xbrl_dimension_edges", "xbrl_dimension_edges_insert")

    fact_frames = refresh_xbrl_fact_frames(store)
    return {
        "packages": int(len(package_frame)),
        "roles": int(len(roles_frame)),
        "relationships": int(len(relationship_frame)),
        "dimension_edges": int(len(dimension_frame)),
        "fact_frames": fact_frames,
    }


class XbrlTaxonomyDataset(Dataset):
    dataset_id = "xbrl_taxonomy"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: XbrlTaxonomyOptions) -> DatasetLoadResult:
        details = refresh_xbrl_taxonomy(store, options)
        rows_loaded = details["relationships"] + details["fact_frames"]
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="xbrl_taxonomy_relationships",
            check_name="rows_loaded",
            status="passed" if details["relationships"] > 0 else "warning",
            observed_value=float(details["relationships"]),
            threshold_value=1.0,
            details=details,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=SOURCE_NAME,
            details=details,
        )
