"""Foreign private issuer / IFRS filer disclosure (task P11, ruling RX12).

The fundamentals pipeline standardizes ``us-gaap`` (+ ``dei`` cover-page) Company Facts only:
``fundamentals.SUPPORTED_FACT_TAXONOMIES`` drops ``ifrs-full`` at load. An issuer that reports
under IFRS therefore has no standardized fundamentals, and before this module nothing said why.
This module labels those issuers explicitly (reason ``ifrs_reporter_not_standardized``) so panel,
coverage and screen consumers can report them instead of leaving them silently missing. It does
NOT map ``ifrs-full`` concepts to canonical items; that is backlog under RX12.

Evidence (retained and offline; nothing is fetched):

* Taxonomy evidence: the retained SEC ``companyfacts.zip``. Members are read one at a time and
  never extracted. :func:`scan_companyfacts_taxonomies` counts facts per taxonomy for every
  member. For members that carry any ``ifrs-full`` fact it also records the basis of every
  financial filing: accession, form, ``filed``, and its ``us-gaap`` and ``ifrs-full`` fact counts.
* Form evidence: a relation shaped like ``sec_submissions`` (``cik``, ``accession_number``,
  ``form``, ``filing_date``) with 20-F/40-F annual and registration forms, 6-K and domestic
  10-K/10-Q. The warehouse table is still partial while the submissions load is being resumed, so
  :func:`scan_submissions_forms` produces the same shape from the retained ``submissions.zip``.

Classification uses taxonomy evidence, not form. A 20-F/40-F filer that reports under US GAAP is
standardized like any 10-K filer, so it keeps ``reporting_basis='us_gaap'`` and gets no reason.
A filer whose Company Facts carry ``ifrs-full`` and no ``us-gaap`` facts is ``ifrs_only``. A
filer whose latest financial filing is ``ifrs-full`` after earlier ``us-gaap`` filings is
``ifrs_after_us_gaap``. Both get the reason. Form evidence is disclosed next to the label and
never overrides it.

Point in time: :data:`INTERVALS_TABLE` holds one row per consecutive run of IFRS financial filings.
``valid_from`` and ``valid_to`` follow the fundamentals clock (``filed`` + 46 h,
:data:`~atx_db._fundamental_clock.FUNDAMENTAL_CLOCK_POLICY`). A consumer applies the reason at
decision time ``t`` only when ``valid_from <= t < valid_to`` (:func:`ifrs_reason_predicate`), so a
later switch to IFRS never labels earlier dates. The archive is one current snapshot and only
carries its own ``filed`` dates. For filings before IFRS XBRL tagging (FPIs using IFRS tagged
from fiscal 2017/2018) it has no taxonomy evidence, so those years get no IFRS interval. They are
counted as foreign-form filings with no financial facts, and are never inferred to be IFRS.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import logging
import os
import re
import tempfile
import zipfile
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import duckdb
import pandas as pd

from ._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ._submissions_archive import SubmissionsArchive

LOGGER = logging.getLogger(__name__)

IFRS_REPORTER_REASON = "ifrs_reporter_not_standardized"
DISCLOSURE_VERSION = "foreign_filer_disclosure_v1"
EVIDENCE_BASIS = "companyfacts_archive_snapshot"
REASON_CLOCK_POLICY = FUNDAMENTAL_CLOCK_POLICY
REASON_CLOCK_HOURS = 46

STANDARDIZED_TAXONOMY = "us-gaap"
IFRS_TAXONOMY = "ifrs-full"
FINANCIAL_TAXONOMIES = (STANDARDIZED_TAXONOMY, IFRS_TAXONOMY)
COVER_TAXONOMY = "dei"

FOREIGN_ANNUAL_FORMS = ("20-F", "20-F/A", "40-F", "40-F/A")
# Form 20-F / 40-F used as an Exchange Act registration statement: same form, foreign issuer only.
FOREIGN_REGISTRATION_FORMS = (
    "20FR12B", "20FR12B/A", "20FR12G", "20FR12G/A", "40FR12B", "40FR12B/A", "40FR12G", "40FR12G/A",
)
FOREIGN_INTERIM_FORMS = ("6-K", "6-K/A")
DOMESTIC_PERIODIC_FORMS = (
    "10-K", "10-K/A", "10-KT", "10-KT/A", "10-K405", "10-K405/A",
    "10-Q", "10-Q/A", "10-QT", "10-QT/A",
)
EVIDENCE_FORMS = FOREIGN_ANNUAL_FORMS + FOREIGN_REGISTRATION_FORMS + FOREIGN_INTERIM_FORMS + DOMESTIC_PERIODIC_FORMS

REPORTING_BASES = (
    "ifrs_only",               # ifrs-full facts, no us-gaap facts -> reason
    "ifrs_after_us_gaap",      # both; latest financial filing is ifrs-full -> reason
    "us_gaap_after_ifrs",      # both; latest financial filing is us-gaap -> historical interval only
    "mixed_taxonomies",        # both taxonomies, but no financial filing carries a filed date
    "us_gaap",                 # us-gaap facts only -> standardized, kept
    "no_financial_facts",      # companyfacts member without us-gaap/ifrs-full facts (or {} placeholder)
    "no_companyfacts_member",  # foreign-form filer absent from the companyfacts archive
)
REASON_BASES = ("ifrs_only", "ifrs_after_us_gaap")

DISCLOSURE_TABLE = "foreign_filer_disclosure"
INTERVALS_TABLE = "foreign_filer_reason_intervals"
MEMBERS_TABLE = "foreign_filer_cf_members"
FILINGS_TABLE = "foreign_filer_cf_filings"

_MEMBER = re.compile(r"^CIK([0-9]{10})\.json$")
_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,62}$")
# Byte-level structure of an SEC companyfacts member: {"cik":..,"entityName":..,"facts":{"<taxonomy>":
# {"<Concept>":{"label":..,"description":..,"units":{"<unit>":[{fact},..]}},..},..}}. Inside a JSON
# string every quote is escaped, so none of these quote-bearing literals can match string content.
# A taxonomy object closes with units } concept } taxonomy } after either a fact array (]}}},") or an
# empty units object ({}}},", measured in the retained archive); a concept boundary has only two braces.
_FACTS_KEY = b'"facts":{'
_TAXONOMY_SEPARATOR = b'}}},"'
_EMPTY_OBJECT_SEPARATOR = b':{},"'
_TAXONOMY_KEY = re.compile(rb'"([a-z][a-z0-9-]*)":\{(?:"[A-Za-z]|\})')
_ACCN = b'"accn":"'
_FACT_ORDERED = re.compile(rb'"accn":"([^"]*)"[^{}]*?"form":"([^"]*)","filed":"([^"]*)"')
_FACT_OBJECT = re.compile(rb'\{[^{}]*"accn":"[^{}]*\}')
_FACT_FIELDS = {name: re.compile(rb'"' + name + rb'":"([^"]*)"') for name in (b"accn", b"form", b"filed")}
_FOREIGN_FORM_FACT_LITERALS = tuple(b'"form":"' + form.encode() + b'"' for form in FOREIGN_ANNUAL_FORMS)
_DOMESTIC_FORM_FACT_LITERALS = tuple(b'"form":"' + form.encode() + b'"' for form in DOMESTIC_PERIODIC_FORMS)
# Submissions pre-filter: every 20-F/40-F family form starts one of these JSON strings.
_FOREIGN_FORM_PREFIXES = (b'"20-F', b'"40-F', b'"20FR', b'"40FR')
_HISTORY_NAME = re.compile(rb'"name":"(CIK[0-9]{10}-submissions-[0-9]+\.json)"')


@dataclass(frozen=True)
class CompanyFactsMemberProfile:
    """Taxonomy fact counts of one companyfacts member (``member_status`` 'facts'|'placeholder'|'no_facts')."""

    cik: str
    member: str
    member_status: str
    uncompressed_bytes: int
    us_gaap_facts: int
    ifrs_facts: int
    dei_facts: int
    other_facts: int
    other_taxonomies: str | None
    foreign_form_facts: int
    domestic_form_facts: int
    filing_detail: bool


@dataclass(frozen=True)
class CompanyFactsFilingBasis:
    """One financial filing of an IFRS-bearing member: which financial taxonomy its facts use."""

    cik: str
    accession_number: str
    form: str | None
    filed: dt.date | None
    us_gaap_facts: int
    ifrs_facts: int


@dataclass(frozen=True)
class CompanyFactsTaxonomyScan:
    source_path: str
    source_sha256: str
    source_bytes: int
    members: tuple[CompanyFactsMemberProfile, ...]
    filings: tuple[CompanyFactsFilingBasis, ...]
    requested_absent: tuple[str, ...]


@dataclass(frozen=True)
class SubmissionFormRow:
    """A ``sec_submissions``-shaped evidence row from the retained bulk archive."""

    cik: str
    accession_number: str
    form: str
    filing_date: dt.date | None
    report_date: dt.date | None
    source_url: str


@dataclass(frozen=True)
class TaxonomyArtifacts:
    members_path: Path
    filings_path: Path


def _cik(value: str | int) -> str:
    return f"{int(str(value).strip()):010d}"


def _date(value: Any) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def _identifier(name: str) -> str:
    if not _IDENTIFIER.fullmatch(name):
        raise ValueError(f"invalid SQL identifier {name!r}")
    return name


def _sql_list(values: Iterable[str]) -> str:
    return ", ".join("'" + value.replace("'", "''") + "'" for value in values)


def _sha256(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _stat_identity(path: Path) -> tuple[int, int, int]:
    stat = path.stat()
    return stat.st_ino, stat.st_size, stat.st_mtime_ns


def _taxonomy_blocks(payload: bytes) -> list[tuple[str, int, int]]:
    """``(taxonomy, begin, end)`` byte spans of the ``facts`` object's children, in order."""
    start = payload.find(_FACTS_KEY)
    if start < 0:
        if b'"facts"' in payload or _ACCN.rstrip(b'"') in payload:
            # Pretty-printed or otherwise non-compact JSON: refuse rather than count zero facts.
            raise ValueError("companyfacts member is not in SEC's compact JSON encoding")
        return []
    cursor = start + len(_FACTS_KEY)
    if payload[cursor:cursor + 1] == b"}":
        return []
    starts = [cursor]
    position = payload.find(_TAXONOMY_SEPARATOR, cursor)
    while position >= 0:
        starts.append(position + len(_TAXONOMY_SEPARATOR) - 1)
        position = payload.find(_TAXONOMY_SEPARATOR, position + 1)
    # An empty object followed by a sibling key: a boundary only when that key is a taxonomy key.
    position = payload.find(_EMPTY_OBJECT_SEPARATOR, cursor)
    while position >= 0:
        key_start = position + len(_EMPTY_OBJECT_SEPARATOR) - 1
        if _TAXONOMY_KEY.match(payload, key_start):
            starts.append(key_start)
        position = payload.find(_EMPTY_OBJECT_SEPARATOR, position + 1)
    starts.sort()
    blocks: list[tuple[str, int, int]] = []
    for index, begin in enumerate(starts):
        match = _TAXONOMY_KEY.match(payload, begin)
        if match is None:
            raise ValueError(f"companyfacts taxonomy key not found at byte {begin}")
        end = starts[index + 1] if index + 1 < len(starts) else len(payload)
        blocks.append((match.group(1).decode("ascii"), begin, end))
    return blocks


def _fact_triples(payload: bytes, begin: int, end: int) -> list[tuple[bytes, bytes | None, bytes | None]]:
    """(accn, form, filed) of every fact in ``payload[begin:end]``; count-verified, never partial."""
    expected = payload.count(_ACCN, begin, end)
    ordered: list[tuple[bytes, bytes | None, bytes | None]] = list(_FACT_ORDERED.findall(payload, begin, end))
    if len(ordered) == expected:
        return ordered
    # SEC key order changed or a fact lacks form/filed: parse each fact object independently.
    triples: list[tuple[bytes, bytes | None, bytes | None]] = []
    for fact in _FACT_OBJECT.findall(payload, begin, end):
        fields = [_FACT_FIELDS[name].search(fact) for name in (b"accn", b"form", b"filed")]
        accn = fields[0]
        if accn is None:
            continue
        triples.append((accn.group(1), None if fields[1] is None else fields[1].group(1),
                        None if fields[2] is None else fields[2].group(1)))
    if len(triples) != expected:
        raise ValueError(f"companyfacts fact parse mismatch: {len(triples)} facts for {expected} accessions")
    return triples


def profile_companyfacts_member(
    member: str, payload: bytes,
) -> tuple[CompanyFactsMemberProfile, list[CompanyFactsFilingBasis]]:
    """Profile one member's bytes; per-filing basis only when it carries ``ifrs-full`` facts."""
    match = _MEMBER.match(member)
    if match is None:
        raise ValueError(f"not a companyfacts member name: {member!r}")
    cik = match.group(1)
    if payload == b"{}":
        return CompanyFactsMemberProfile(cik, member, "placeholder", len(payload), 0, 0, 0, 0, None, 0, 0,
                                         False), []
    blocks = _taxonomy_blocks(payload)
    counts: dict[str, int] = {}
    for taxonomy, begin, end in blocks:
        counts[taxonomy] = counts.get(taxonomy, 0) + payload.count(_ACCN, begin, end)
    if payload.count(b'"accn"') != sum(counts.values()):
        raise ValueError(f"{member}: fact accessions outside the compact taxonomy structure")
    others =sorted(name for name in counts if name not in (*FINANCIAL_TAXONOMIES, COVER_TAXONOMY))
    ifrs_facts = counts.get(IFRS_TAXONOMY, 0)
    filings: list[CompanyFactsFilingBasis] = []
    if ifrs_facts:
        by_accession: dict[bytes, list[Any]] = {}
        for taxonomy, begin, end in blocks:
            if taxonomy not in FINANCIAL_TAXONOMIES:
                continue
            column = 2 if taxonomy == STANDARDIZED_TAXONOMY else 3
            for accn, form, filed in _fact_triples(payload, begin, end):
                entry = by_accession.get(accn)
                if entry is None:
                    entry = by_accession[accn] = [form, filed, 0, 0]
                entry[column] += 1
        for accn, (form, filed, us_gaap, ifrs) in sorted(by_accession.items()):
            filings.append(CompanyFactsFilingBasis(
                cik, accn.decode("ascii", "replace"), None if form is None else form.decode("utf-8", "replace"),
                _date(None if filed is None else filed.decode("ascii", "replace")), us_gaap, ifrs,
            ))
    profile = CompanyFactsMemberProfile(
        cik=cik,
        member=member,
        member_status="facts" if blocks else "no_facts",
        uncompressed_bytes=len(payload),
        us_gaap_facts=counts.get(STANDARDIZED_TAXONOMY, 0),
        ifrs_facts=ifrs_facts,
        dei_facts=counts.get(COVER_TAXONOMY, 0),
        other_facts=sum(counts[name] for name in others),
        other_taxonomies=",".join(others) or None,
        foreign_form_facts=sum(payload.count(literal) for literal in _FOREIGN_FORM_FACT_LITERALS),
        domestic_form_facts=sum(payload.count(literal) for literal in _DOMESTIC_FORM_FACT_LITERALS),
        filing_detail=bool(ifrs_facts),
    )
    return profile, filings


def scan_companyfacts_taxonomies(
    zip_path: str | Path, *, ciks: Iterable[str | int] | None = None, progress_every: int = 2000,
) -> CompanyFactsTaxonomyScan:
    """Taxonomy profile of the retained companyfacts archive, one member in memory at a time.

    ``ciks`` bounds the read to those members (absent ones are returned in ``requested_absent``).
    The source is SHA-256 identified and must not change while it is read.
    """
    path = Path(zip_path)
    before = _stat_identity(path)
    source_sha256 = _sha256(path)
    members: list[CompanyFactsMemberProfile] = []
    filings: list[CompanyFactsFilingBasis] = []
    absent: list[str] = []
    with zipfile.ZipFile(path) as archive:
        infos = {info.filename: info for info in archive.infolist() if _MEMBER.match(info.filename)}
        if ciks is None:
            names = sorted(infos)
        else:
            wanted = sorted({f"CIK{_cik(cik)}.json" for cik in ciks})
            absent = [name[3:13] for name in wanted if name not in infos]
            names = [name for name in wanted if name in infos]
        for index, name in enumerate(names, start=1):
            profile, member_filings = profile_companyfacts_member(name, archive.read(infos[name]))
            members.append(profile)
            filings.extend(member_filings)
            if progress_every and index % progress_every == 0:
                LOGGER.info("companyfacts taxonomy scan: members=%d/%d ifrs_filings=%d", index, len(names),
                            len(filings))
    if _stat_identity(path) != before:
        raise ValueError("companyfacts archive changed while it was scanned")
    return CompanyFactsTaxonomyScan(str(path), source_sha256, before[1], tuple(members), tuple(filings),
                                    tuple(absent))


def _columnar_rows(block: dict[str, Any], cik: str, wanted: frozenset[str], source_url: str,
                   ) -> Iterator[SubmissionFormRow]:
    forms = block.get("form") or []
    accessions = block.get("accessionNumber") or []
    filing_dates = block.get("filingDate") or []
    report_dates = block.get("reportDate") or []
    for index, raw_form in enumerate(forms):
        form = str(raw_form or "").strip().upper()
        if form not in wanted or index >= len(accessions) or not accessions[index]:
            continue
        yield SubmissionFormRow(
            cik=cik,
            accession_number=str(accessions[index]).strip(),
            form=form,
            filing_date=_date(filing_dates[index] if index < len(filing_dates) else None),
            report_date=_date(report_dates[index] if index < len(report_dates) else None),
            source_url=source_url,
        )


def scan_submissions_forms(
    zip_path: str | Path,
    *,
    ciks: Iterable[str | int] | None = None,
    forms: Iterable[str] = EVIDENCE_FORMS,
    progress_every: int = 20000,
) -> Iterator[SubmissionFormRow]:
    """``sec_submissions``-shaped form evidence for 20-F/40-F-family filers in the retained bulk archive.

    Each entity's main and history members are read through the bounded directory reader, one
    entity at a time. Only entities whose bytes contain a 20-F/40-F-family form string are parsed;
    for those, every filing whose form is in ``forms`` is yielded, including domestic 10-K/10-Q
    and 6-K rows. History member names come from the main member's ``filings.files`` list.
    """
    wanted = frozenset(form.strip().upper() for form in forms)
    with SubmissionsArchive(Path(zip_path)) as archive:
        if ciks is None:
            names: Iterable[str] = archive.main_members()
        else:
            names = [f"CIK{cik}.json" for cik in sorted({_cik(value) for value in ciks})]
        for index, member in enumerate(names, start=1):
            if progress_every and index % progress_every == 0:
                LOGGER.info("submissions form scan: main_members=%d", index)
            if member not in archive:
                continue
            cik = member[3:13]
            main = archive.read(member)
            history = [(name.decode("ascii"), archive.read(name.decode("ascii")))
                       for name in dict.fromkeys(_HISTORY_NAME.findall(main)) if name.decode("ascii") in archive]
            if not any(prefix in blob for _, blob in [(member, main), *history] for prefix in _FOREIGN_FORM_PREFIXES):
                continue
            payload = json.loads(main)
            yield from _columnar_rows((payload.get("filings") or {}).get("recent") or {}, cik, wanted,
                                      f"{zip_path}!{member}")
            for name, blob in history:
                yield from _columnar_rows(json.loads(blob), cik, wanted, f"{zip_path}!{name}")


def taxonomy_scan_frames(scan: CompanyFactsTaxonomyScan) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Member and filing frames carrying the archive identity on every row."""
    identity: dict[str, str | int] = {"source_sha256": scan.source_sha256, "source_bytes": scan.source_bytes,
                                      "evidence_basis": EVIDENCE_BASIS}
    members = pd.DataFrame(
        [vars(profile) for profile in scan.members],
        columns=list(CompanyFactsMemberProfile.__dataclass_fields__),
    ).assign(**identity)
    filings = pd.DataFrame(
        [vars(filing) for filing in scan.filings],
        columns=list(CompanyFactsFilingBasis.__dataclass_fields__),
    ).assign(**identity)
    return members, filings


_MEMBER_CASTS = """
    CAST(cik AS VARCHAR) AS cik, CAST(member AS VARCHAR) AS member, CAST(member_status AS VARCHAR) AS member_status,
    CAST(uncompressed_bytes AS BIGINT) AS uncompressed_bytes, CAST(us_gaap_facts AS BIGINT) AS us_gaap_facts,
    CAST(ifrs_facts AS BIGINT) AS ifrs_facts, CAST(dei_facts AS BIGINT) AS dei_facts,
    CAST(other_facts AS BIGINT) AS other_facts, CAST(other_taxonomies AS VARCHAR) AS other_taxonomies,
    CAST(foreign_form_facts AS BIGINT) AS foreign_form_facts,
    CAST(domestic_form_facts AS BIGINT) AS domestic_form_facts, CAST(filing_detail AS BOOLEAN) AS filing_detail,
    CAST(source_sha256 AS VARCHAR) AS source_sha256, CAST(source_bytes AS BIGINT) AS source_bytes,
    CAST(evidence_basis AS VARCHAR) AS evidence_basis
"""
_FILING_CASTS = """
    CAST(cik AS VARCHAR) AS cik, CAST(accession_number AS VARCHAR) AS accession_number,
    CAST(form AS VARCHAR) AS form, CAST(filed AS DATE) AS filed, CAST(us_gaap_facts AS BIGINT) AS us_gaap_facts,
    CAST(ifrs_facts AS BIGINT) AS ifrs_facts, CAST(source_sha256 AS VARCHAR) AS source_sha256,
    CAST(source_bytes AS BIGINT) AS source_bytes, CAST(evidence_basis AS VARCHAR) AS evidence_basis
"""


def materialize_taxonomy_scan(
    con: duckdb.DuckDBPyConnection,
    scan: CompanyFactsTaxonomyScan,
    *,
    members_table: str = MEMBERS_TABLE,
    filings_table: str = FILINGS_TABLE,
    temporary: bool = True,
) -> tuple[str, str]:
    """Create the scan's member and filing tables in ``con``; returns their names."""
    kind = "TEMP TABLE" if temporary else "TABLE"
    members, filings = taxonomy_scan_frames(scan)
    for view, frame, table, casts in (
        ("_ff_members_frame", members, _identifier(members_table), _MEMBER_CASTS),
        ("_ff_filings_frame", filings, _identifier(filings_table), _FILING_CASTS),
    ):
        con.register(view, frame)
        try:
            con.execute(f"CREATE OR REPLACE {kind} {table} AS SELECT {casts} FROM {view}")
        finally:
            con.unregister(view)
    return members_table, filings_table


def taxonomy_artifact_paths(directory: str | Path, source_sha256: str) -> TaxonomyArtifacts:
    if not re.fullmatch(r"[0-9a-f]{64}", source_sha256):
        raise ValueError("source_sha256 must be a lowercase SHA-256 hex digest")
    base = Path(directory) / f"companyfacts.zip.taxonomy-v1-{source_sha256}"
    return TaxonomyArtifacts(base.with_name(base.name + ".members.parquet"),
                             base.with_name(base.name + ".filings.parquet"))


def write_taxonomy_artifacts(scan: CompanyFactsTaxonomyScan, directory: str | Path | None = None) -> TaxonomyArtifacts:
    """Publish the scan as two SHA-keyed parquet files (atomic replace) next to the archive by default."""
    target = Path(directory) if directory is not None else Path(scan.source_path).parent
    target.mkdir(parents=True, exist_ok=True)
    artifacts = taxonomy_artifact_paths(target, scan.source_sha256)
    con = duckdb.connect(":memory:")
    try:
        con.execute("SET memory_limit='256MB'")
        con.execute("SET threads=1")
        members_table, filings_table = materialize_taxonomy_scan(con, scan)
        for table, destination in ((members_table, artifacts.members_path), (filings_table, artifacts.filings_path)):
            fd, partial = tempfile.mkstemp(prefix=".foreign-filers-partial-", suffix=".parquet", dir=target)
            os.close(fd)
            partial_path = Path(partial)
            try:
                literal = str(partial_path).replace("'", "''")
                con.execute(f"COPY (SELECT * FROM {table} ORDER BY ALL) TO '{literal}' (FORMAT parquet)")
                os.replace(partial_path, destination)
            finally:
                partial_path.unlink(missing_ok=True)
    finally:
        con.close()
    return artifacts


def parquet_relation(path: str | Path) -> str:
    return "read_parquet('" + str(path).replace("'", "''") + "')"


def _filing_basis_sql(filings: str) -> str:
    # A filing's basis is its dominant financial taxonomy. Measured on the retained archive: 469 of 8,777
    # filings carry both, and 426 of them are >= 90 % ifrs-full with a median of 3 stray us-gaap facts.
    # Those facts cannot produce standardized statements, so they must not break an IFRS run.
    return f"""
        SELECT cik, accession_number, form, filed,
               CASE WHEN ifrs_facts > us_gaap_facts THEN 'ifrs' ELSE 'us_gaap' END AS basis,
               ifrs_facts > 0 AND us_gaap_facts > 0 AS mixed
        FROM {filings} AS f
        WHERE filed IS NOT NULL AND (ifrs_facts > 0 OR us_gaap_facts > 0)
    """


def _intervals_sql(filings: str) -> str:
    clock = f"INTERVAL {REASON_CLOCK_HOURS} HOUR"
    return f"""
        WITH fin AS ({_filing_basis_sql(filings)}),
        marked AS (
            SELECT *, CASE WHEN basis IS DISTINCT FROM lag(basis) OVER (
                PARTITION BY cik ORDER BY filed, accession_number) THEN 1 ELSE 0 END AS run_start
            FROM fin
        ),
        runs AS (
            SELECT *, sum(run_start) OVER (
                PARTITION BY cik ORDER BY filed, accession_number
                ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS run_no
            FROM marked
        ),
        bounds AS (
            SELECT cik, run_no, min(basis) AS basis, min(filed) AS first_filed, max(filed) AS last_filed,
                   first(accession_number ORDER BY filed, accession_number) AS first_accession,
                   last(accession_number ORDER BY filed, accession_number) AS last_accession,
                   count(*) AS filings
            FROM runs GROUP BY cik, run_no
        ),
        chained AS (
            SELECT *, lead(first_filed) OVER (PARTITION BY cik ORDER BY run_no) AS ended_by_filed,
                      lead(basis) OVER (PARTITION BY cik ORDER BY run_no) AS ended_by_basis
            FROM bounds
        )
        SELECT cik,
               '{IFRS_REPORTER_REASON}' AS reason_code,
               CAST(first_filed AS TIMESTAMP) + {clock} AS valid_from,
               CAST(ended_by_filed AS TIMESTAMP) + {clock} AS valid_to,
               first_filed, last_filed, ended_by_filed, ended_by_basis,
               first_accession, last_accession, filings AS ifrs_filings,
               CAST(row_number() OVER (PARTITION BY cik ORDER BY run_no) AS INTEGER) AS interval_no,
               '{REASON_CLOCK_POLICY}' AS clock_policy,
               '{DISCLOSURE_VERSION}' AS disclosure_version
        FROM chained
        WHERE basis = 'ifrs' AND (ended_by_filed IS NULL OR ended_by_filed > first_filed)
    """


def _submissions_sql(submissions: str | None) -> tuple[str, str]:
    foreign = FOREIGN_ANNUAL_FORMS + FOREIGN_REGISTRATION_FORMS
    if submissions is None:
        empty = """SELECT CAST(NULL AS VARCHAR) AS cik, 0::BIGINT AS foreign_annual_filings,
                   0::BIGINT AS foreign_registration_filings, 0::BIGINT AS foreign_interim_filings,
                   0::BIGINT AS domestic_periodic_filings, CAST(NULL AS DATE) AS first_foreign_form_filed,
                   CAST(NULL AS DATE) AS last_foreign_form_filed, CAST(NULL AS DATE) AS last_domestic_periodic_filed
                   WHERE false"""
        return empty, "SELECT CAST(NULL AS VARCHAR) AS cik WHERE false"
    aggregate = f"""
        SELECT cik,
               count(DISTINCT accession_number) FILTER (WHERE form_key IN ({_sql_list(FOREIGN_ANNUAL_FORMS)}))
                   AS foreign_annual_filings,
               count(DISTINCT accession_number) FILTER (WHERE form_key IN ({_sql_list(FOREIGN_REGISTRATION_FORMS)}))
                   AS foreign_registration_filings,
               count(DISTINCT accession_number) FILTER (WHERE form_key IN ({_sql_list(FOREIGN_INTERIM_FORMS)}))
                   AS foreign_interim_filings,
               count(DISTINCT accession_number) FILTER (WHERE form_key IN ({_sql_list(DOMESTIC_PERIODIC_FORMS)}))
                   AS domestic_periodic_filings,
               min(filing_date) FILTER (WHERE form_key IN ({_sql_list(foreign)})) AS first_foreign_form_filed,
               max(filing_date) FILTER (WHERE form_key IN ({_sql_list(foreign)})) AS last_foreign_form_filed,
               max(filing_date) FILTER (WHERE form_key IN ({_sql_list(DOMESTIC_PERIODIC_FORMS)}))
                   AS last_domestic_periodic_filed
        FROM (
            SELECT cik, accession_number, filing_date, upper(trim(form)) AS form_key
            FROM {submissions} AS s
            WHERE upper(trim(form)) IN ({_sql_list(EVIDENCE_FORMS)})
        ) AS s
        GROUP BY cik
    """
    return aggregate, f"SELECT cik FROM {submissions} AS s"


def _disclosure_sql(members: str, filings: str, intervals: str, submissions: str | None) -> str:
    aggregate, any_rows = _submissions_sql(submissions)
    reason_bases = _sql_list(REASON_BASES)
    not_supplied = "true" if submissions is None else "false"
    return f"""
        WITH member AS (SELECT * FROM {members} AS m),
        sub AS ({aggregate}),
        fin AS ({_filing_basis_sql(filings)}),
        fil AS (
            SELECT cik,
                   count(*) FILTER (WHERE basis = 'ifrs') AS ifrs_filings,
                   count(*) FILTER (WHERE basis = 'us_gaap') AS us_gaap_filings,
                   count(*) FILTER (WHERE mixed) AS mixed_filings,
                   min(filed) FILTER (WHERE basis = 'ifrs') AS ifrs_first_filed,
                   max(filed) FILTER (WHERE basis = 'ifrs') AS ifrs_last_filed,
                   min(filed) FILTER (WHERE basis = 'us_gaap') AS us_gaap_first_filed,
                   max(filed) FILTER (WHERE basis = 'us_gaap') AS us_gaap_last_filed,
                   last(basis ORDER BY filed, accession_number) AS latest_financial_basis,
                   max(filed) AS latest_financial_filed
            FROM fin GROUP BY cik
        ),
        iv AS (
            SELECT cik, count(*) AS ifrs_intervals,
                   max(valid_from) FILTER (WHERE valid_to IS NULL) AS open_valid_from
            FROM {intervals} GROUP BY cik
        ),
        scope AS (
            SELECT cik FROM member WHERE ifrs_facts > 0 OR foreign_form_facts > 0
            UNION
            SELECT cik FROM sub WHERE foreign_annual_filings + foreign_registration_filings > 0
        ),
        sub_any AS (SELECT DISTINCT cik FROM ({any_rows}) AS a WHERE cik IN (SELECT cik FROM scope)),
        classified AS (
            SELECT scope.cik,
                   CASE
                       WHEN member.cik IS NULL THEN 'no_companyfacts_member'
                       WHEN member.ifrs_facts > 0 AND member.us_gaap_facts = 0 THEN 'ifrs_only'
                       WHEN member.ifrs_facts > 0 AND fil.latest_financial_basis = 'ifrs' THEN 'ifrs_after_us_gaap'
                       WHEN member.ifrs_facts > 0 AND fil.latest_financial_basis = 'us_gaap' THEN 'us_gaap_after_ifrs'
                       WHEN member.ifrs_facts > 0 THEN 'mixed_taxonomies'
                       WHEN member.us_gaap_facts > 0 THEN 'us_gaap'
                       ELSE 'no_financial_facts'
                   END AS reporting_basis,
                   coalesce(sub.foreign_annual_filings, 0) + coalesce(sub.foreign_registration_filings, 0) > 0
                       AS submissions_foreign_form,
                   coalesce(member.foreign_form_facts, 0) > 0 AS companyfacts_foreign_form,
                   CASE WHEN {not_supplied} THEN 'not_supplied'
                        WHEN sub_any.cik IS NULL THEN 'no_rows'
                        ELSE 'rows' END AS submissions_evidence,
                   sub.foreign_annual_filings, sub.foreign_registration_filings, sub.foreign_interim_filings,
                   sub.domestic_periodic_filings, sub.first_foreign_form_filed, sub.last_foreign_form_filed,
                   sub.last_domestic_periodic_filed,
                   coalesce(member.member_status, 'absent') AS companyfacts_member_status,
                   member.us_gaap_facts, member.ifrs_facts, member.dei_facts, member.other_taxonomies,
                   member.foreign_form_facts, member.domestic_form_facts,
                   fil.ifrs_filings, fil.us_gaap_filings, fil.mixed_filings,
                   fil.ifrs_first_filed, fil.ifrs_last_filed, fil.us_gaap_first_filed, fil.us_gaap_last_filed,
                   fil.latest_financial_basis, fil.latest_financial_filed,
                   coalesce(iv.ifrs_intervals, 0) AS ifrs_intervals, iv.open_valid_from,
                   member.source_sha256 AS taxonomy_source_sha256
            FROM scope
            LEFT JOIN member ON member.cik = scope.cik
            LEFT JOIN sub ON sub.cik = scope.cik
            LEFT JOIN fil ON fil.cik = scope.cik
            LEFT JOIN iv ON iv.cik = scope.cik
            LEFT JOIN sub_any ON sub_any.cik = scope.cik
        )
        SELECT cik, reporting_basis,
               CASE WHEN reporting_basis IN ({reason_bases}) THEN '{IFRS_REPORTER_REASON}' END AS reason_code,
               CASE WHEN reporting_basis IN ({reason_bases}) THEN open_valid_from END AS reason_valid_from,
               submissions_foreign_form OR companyfacts_foreign_form AS foreign_form_filer,
               * EXCLUDE (cik, reporting_basis, open_valid_from),
               '{EVIDENCE_BASIS}' AS evidence_basis,
               '{REASON_CLOCK_POLICY}' AS clock_policy,
               '{DISCLOSURE_VERSION}' AS disclosure_version
        FROM classified
    """


def build_foreign_filer_disclosure(
    con: duckdb.DuckDBPyConnection,
    *,
    members: str = MEMBERS_TABLE,
    filings: str = FILINGS_TABLE,
    submissions: str | None = "sec_submissions",
    disclosure_table: str = DISCLOSURE_TABLE,
    intervals_table: str = INTERVALS_TABLE,
    temporary: bool = True,
) -> dict[str, Any]:
    """Create the disclosure and PIT reason-interval tables in ``con``; returns :func:`summarize_disclosure`.

    ``members``/``filings`` are relations with the :func:`materialize_taxonomy_scan` shape (a table name
    or :func:`parquet_relation`; pass a subquery in parentheses, ``(SELECT ...)``). ``submissions`` is a ``sec_submissions``-shaped relation (10-digit
    ``cik``, ``accession_number``, ``form``, ``filing_date``) or ``None`` for taxonomy evidence only.
    Relation arguments are trusted SQL from the caller; created table names are validated.
    """
    kind = "TEMP TABLE" if temporary else "TABLE"
    intervals = _identifier(intervals_table)
    disclosure = _identifier(disclosure_table)
    con.execute(f"CREATE OR REPLACE {kind} {intervals} AS {_intervals_sql(filings)}")
    con.execute(f"CREATE OR REPLACE {kind} {disclosure} AS {_disclosure_sql(members, filings, intervals, submissions)}")
    return summarize_disclosure(con, disclosure_table=disclosure, intervals_table=intervals)


def ifrs_reason_predicate(interval_alias: str, cik_sql: str, decision_sql: str) -> str:
    """Join predicate: the IFRS reason applies to ``cik_sql`` at decision time ``decision_sql``."""
    alias = _identifier(interval_alias)
    return (f"({alias}.cik = {cik_sql} AND {alias}.valid_from <= {decision_sql} "
            f"AND ({alias}.valid_to IS NULL OR {decision_sql} < {alias}.valid_to))")


def summarize_disclosure(
    con: duckdb.DuckDBPyConnection,
    *,
    disclosure_table: str = DISCLOSURE_TABLE,
    intervals_table: str = INTERVALS_TABLE,
) -> dict[str, Any]:
    """Counts a coverage report needs: bases, flags, foreign-form filers without us-gaap facts."""
    disclosure = _identifier(disclosure_table)
    intervals = _identifier(intervals_table)
    by_basis = {
        f"{basis}|{'foreign_form' if foreign else 'no_foreign_form'}": count
        for basis, foreign, count in con.execute(
            f"SELECT reporting_basis, foreign_form_filer, count(*) FROM {disclosure} GROUP BY ALL ORDER BY ALL"
        ).fetchall()
    }
    row = con.execute(f"""
        SELECT count(*),
               count(*) FILTER (WHERE reason_code IS NOT NULL),
               count(*) FILTER (WHERE foreign_form_filer),
               count(*) FILTER (WHERE foreign_form_filer AND coalesce(us_gaap_facts, 0) = 0),
               count(*) FILTER (WHERE foreign_form_filer AND reporting_basis = 'us_gaap'),
               count(*) FILTER (WHERE foreign_form_filer AND reason_code IS NOT NULL),
               count(*) FILTER (WHERE NOT foreign_form_filer AND reason_code IS NOT NULL),
               count(*) FILTER (WHERE reason_code IS NOT NULL AND reason_valid_from IS NULL),
               count(*) FILTER (WHERE submissions_foreign_form),
               count(*) FILTER (WHERE companyfacts_foreign_form)
        FROM {disclosure}
    """).fetchone()
    interval_row = con.execute(f"""
        SELECT count(*), count(DISTINCT cik), count(*) FILTER (WHERE valid_to IS NULL),
               (SELECT count(*) FROM (SELECT cik FROM {intervals} GROUP BY cik HAVING count(*) > 1))
        FROM {intervals}
    """).fetchone()
    assert row is not None and interval_row is not None
    return {
        "disclosure_ciks": row[0],
        "flagged_ciks": row[1],
        "foreign_form_filers": row[2],
        "foreign_form_filers_zero_us_gaap_facts": row[3],
        "foreign_form_filers_us_gaap_kept": row[4],
        "foreign_form_filers_flagged": row[5],
        "flagged_without_foreign_form": row[6],
        "flagged_without_reason_interval": row[7],
        "submissions_foreign_form_filers": row[8],
        "companyfacts_foreign_form_filers": row[9],
        "reporting_basis_by_form_evidence": by_basis,
        "reason_intervals": interval_row[0],
        "reason_interval_ciks": interval_row[1],
        "open_reason_intervals": interval_row[2],
        "ciks_with_multiple_ifrs_intervals": interval_row[3],
    }
