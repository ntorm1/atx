"""Pinned original SEC document reader and independent full-row replay.

The caller supplies the independently reviewed expected contract/code pins,
never a receipt produced by this parser as its own admission authority. There
is no HTTP or filesystem source access. HTML-only records use retrieval floors;
SEC acceptance is available only from matched native full-submission headers.
This reader admits document identity/clock and parser semantics, not financial
party/class/price basis or certified exact terminal proceeds.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
from dataclasses import dataclass, fields
from html.parser import HTMLParser
from pathlib import Path
from typing import Mapping
from zoneinfo import ZoneInfo

from . import mna_vintages_v2 as V
from . import terminal_mna_v2 as T


@dataclass(frozen=True)
class NativePin:
    source_id: str
    sha256: str
    accession: str
    cik: int
    fetched_at: dt.datetime
    available_at: dt.datetime
    clock_basis: str


@dataclass(frozen=True)
class BulkClockPin:
    source_id: str
    sha256: str
    cik: int
    accession: str
    available_at: dt.datetime
    fetched_at: dt.datetime


def read_bulk_clock(blob: bytes, pin: BulkClockPin) -> dt.datetime:
    """Native SEC submissions recent-entry clock with exact issuer/accession.

    Offset-bearing acceptanceDateTime is mandatory; filing/report dates or
    accession-derived timestamps never substitute. Archived arrays lacking
    native issuer identity are unsupported until their issuer binding is admitted.
    """
    if not pin.source_id or hashlib.sha256(blob).hexdigest() != pin.sha256:
        raise ValueError("native bulk clock identity mismatch")
    value = json.loads(blob)
    if int(value.get("cik", 0)) != pin.cik:
        raise ValueError("native bulk issuer mismatch")
    recent = value.get("filings", {}).get("recent", {})
    accessions = recent.get("accessionNumber", [])
    clocks = recent.get("acceptanceDateTime", [])
    if len(accessions) != len(clocks) or accessions.count(pin.accession) != 1:
        raise ValueError("unique native bulk accession/clock arrays required")
    raw = clocks[accessions.index(pin.accession)]
    if not isinstance(raw, str):
        raise ValueError("native acceptance clock missing")
    clock = T.utc(dt.datetime.fromisoformat(raw.replace("Z", "+00:00")))
    if clock != T.utc(pin.available_at) or clock > T.utc(pin.fetched_at):
        raise ValueError("native bulk acceptance clock mismatch")
    return clock


class _Text(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.hidden = 0

    def handle_starttag(self, tag, attrs):
        if tag.lower() in ("script", "style"):
            self.hidden += 1
        self.parts.append(" ")

    def handle_endtag(self, tag):
        if tag.lower() in ("script", "style"):
            self.hidden = max(0, self.hidden - 1)
        self.parts.append(" ")

    def handle_data(self, data):
        if not self.hidden:
            self.parts.append(data)


def code_pins() -> dict[str, str]:
    """Exact local static reader, parser and qualification code identity."""
    return {name: hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()
            for name, module in (("mna_native_v2", __import__(__name__, fromlist=["x"])),
                                 ("mna_vintages_v2", V), ("terminal_mna_v2", T))}


def read_native(blob: bytes, pin: NativePin, admission_id: str,
                bulk_clock: tuple[bytes, BulkClockPin] | None = None) -> V.DealVintage:
    if not admission_id or hashlib.sha256(blob).hexdigest() != pin.sha256:
        raise ValueError("independent native object identity mismatch")
    if not pin.source_id or not pin.accession or pin.cik <= 0:
        raise ValueError("nonnull source/accession/CIK required")
    fetched, available = T.utc(pin.fetched_at), T.utc(pin.available_at)
    text = blob.decode("utf-8", errors="strict")
    if pin.clock_basis == "native_sec_acceptance":
        # Match issuer CIK only in the native FILER section, not an unrelated
        # subject-company header or an HTML quoted accession.
        accession = re.search(r"(?m)^ACCESSION NUMBER:\s*(\d{10}-\d{2}-\d{6})\s*$", text)
        filer = re.search(r"(?ms)^FILER:\s*\n(.*?)(?=^FILER:|^SUBJECT COMPANY:|^</SEC-HEADER>|^<DOCUMENT>)", text)
        cik = re.search(r"CENTRAL INDEX KEY:\s*(\d+)", filer.group(1)) if filer else None
        acceptance = re.search(r"(?m)^<ACCEPTANCE-DATETIME>(\d{14})\s*$", text)
        if not accession or not cik or not acceptance:
            raise ValueError("native submission accession/filer/acceptance missing")
        clock = dt.datetime.strptime(acceptance.group(1), "%Y%m%d%H%M%S").replace(
            tzinfo=ZoneInfo("America/New_York")).astimezone(T.UTC)
        if accession.group(1) != pin.accession or int(cik.group(1)) != pin.cik or clock != available:
            raise ValueError("native submission identity/clock mismatch")
        if available > fetched:
            raise ValueError("acceptance after retrieval")
        documents = re.findall(r"(?s)<DOCUMENT>(.*?)</DOCUMENT>", text)
        # A full submission can contain many unrelated exhibits; selection
        # requires its own pinned document contract. Do not parse aggregate text.
        if len(documents) != 1:
            raise ValueError("ambiguous native submission document inventory")
        text = documents[0]
    elif pin.clock_basis == "native_sec_bulk_acceptance":
        if bulk_clock is None:
            raise ValueError("independently pinned original SEC bulk clock required")
        clock_blob, clock_pin = bulk_clock
        if clock_pin.cik != pin.cik or clock_pin.accession != pin.accession:
            raise ValueError("native document/bulk clock issuer/accession mismatch")
        if read_bulk_clock(clock_blob, clock_pin) != available or available > fetched:
            raise ValueError("native document/bulk acceptance mismatch")
    elif pin.clock_basis == "retrieval_floor":
        if available != fetched:
            raise ValueError("HTML missing clock must retain retrieval floor")
    else:
        raise ValueError("unsupported native clock basis")
    parser = _Text()
    parser.feed(text)
    parser.close()
    evidence = T.Evidence(pin.source_id, pin.sha256, available, admission_id)
    return V.parse_native_candidate(pin.accession, " ".join(parser.parts), evidence)


def replay_native(objects: Mapping[str, bytes], pins: tuple[NativePin, ...], *,
                  admission_id: str, expected_code: Mapping[str, str],
                  bulk_clocks: Mapping[str, tuple[bytes, BulkClockPin]] | None = None) -> list[V.DealVintage]:
    """Compute with code bound before, all objects pinned, and checked afterward."""
    before = code_pins()
    if dict(expected_code) != before:
        raise ValueError("independently pinned complete reader/code identity mismatch")
    ids = [pin.source_id for pin in pins]
    if len(ids) != len(set(ids)) or set(ids) != set(objects):
        raise ValueError("complete unique native object inventory required")
    bulk_clocks = bulk_clocks or {}
    if set(bulk_clocks) != {pin.source_id for pin in pins if pin.clock_basis == "native_sec_bulk_acceptance"}:
        raise ValueError("complete exact native clock object inventory required")
    rows = [read_native(objects[pin.source_id], pin, admission_id, bulk_clocks.get(pin.source_id)) for pin in pins]
    V.validate_vintages(rows)
    if code_pins() != before:
        raise ValueError("reader/code changed during native replay")
    return rows


def validate_native_rows(rows: list[dict], objects: Mapping[str, bytes], pins: tuple[NativePin, ...], *,
                         admission_id: str, expected_code: Mapping[str, str],
                         bulk_clocks: Mapping[str, tuple[bytes, BulkClockPin]] | None = None) -> None:
    """Read-only public admission: full schema, keys, inventory and native replay."""
    names = {field.name for field in fields(V.DealVintage)}
    if any(set(row) != names for row in rows):
        raise ValueError("full exact public schema required")
    expected = replay_native(objects, pins, admission_id=admission_id, expected_code=expected_code,
                             bulk_clocks=bulk_clocks)
    actual = [V.DealVintage(**row) for row in rows]
    V.validate_vintages(actual)
    key = lambda row: (row.accession, row.source_id, row.available_at)
    if sorted(actual, key=key) != sorted(expected, key=key):
        raise ValueError("complete native output replay mismatch")
