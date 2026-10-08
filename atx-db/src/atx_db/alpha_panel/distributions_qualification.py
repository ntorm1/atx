"""Conditional native semantic readers; immutable input admission remains mandatory.

These functions parse evidence rather than trusting qualified/matches flags. They
do not fetch, search a lake, certify quote currency, or close actual task gates.
"""
from __future__ import annotations
import datetime as dt
import re
from zoneinfo import ZoneInfo
from . import distributions as D


def timezone_paths() -> list:
    """Explicit package TZif and compiled normalizer; ambient TZPATH is unused."""
    import tzdata
    import zoneinfo
    import _zoneinfo
    import zoneinfo._common
    import _strptime
    import sys
    from pathlib import Path
    return [Path(tzdata.__file__).parent / "zoneinfo/America/New_York",
            Path(zoneinfo.__file__), Path(zoneinfo._common.__file__), Path(_zoneinfo.__file__),
            Path(dt.__file__), Path(_strptime.__file__), Path(sys.executable)]


def accepted_clock(blob: bytes, accession: str, *, issuer: int,
                   member_url: str, member_blob: bytes) -> dt.datetime:
    """Narrow authoritative original index/full-submission identity and membership."""
    from lxml import html
    from urllib.parse import urlparse
    import _zoneinfo
    import io
    assert_runtime()
    if ZoneInfo is not _zoneinfo.ZoneInfo:
        raise ValueError("timezone normalizer entrypoint changed")
    if not re.fullmatch(r"\d{10}-\d{2}-\d{6}", accession):
        raise ValueError("native accession format unsupported")
    uri = urlparse(member_url)
    prefix = f"/Archives/edgar/data/{issuer}/{accession.replace('-', '')}/"
    if uri.scheme != "https" or uri.hostname != "www.sec.gov" or not uri.path.startswith(prefix) or uri.query or uri.fragment:
        raise ValueError("native SEC member URI/issuer/accession conflict")
    text = blob.decode("utf-8")
    if "<SEC-DOCUMENT>" in text or "<SEC-HEADER>" in text:
        header = text.split("<DOCUMENT>", 1)[0]
        accessions = re.findall(r"<ACCESSION-NUMBER>\s*([^\s<]+)", header)
        ciks = re.findall(r"CENTRAL INDEX KEY:\s*(\d+)|<CENTRAL-INDEX-KEY>\s*(\d+)", header)
        stamps = re.findall(r"<ACCEPTANCE-DATETIME>(\d{14})", header)
        if accessions != [accession] or len(ciks) != 1 or int(ciks[0][0] or ciks[0][1]) != issuer or len(stamps) != 1:
            raise ValueError("authoritative native accession/issuer/clock conflict")
        documents = re.findall(r"<DOCUMENT>(.*?)</DOCUMENT>", text, re.S)
        matching = [part for part in documents if re.findall(r"<FILENAME>\s*([^\s<]+)", part) == [uri.path.rsplit('/', 1)[-1]]]
        bodies = re.findall(r"<TEXT>(.*?)</TEXT>", matching[0], re.S) if len(matching) == 1 else []
        if len(bodies) != 1 or bodies[0].encode().strip() != member_blob.strip():
            raise ValueError("native submission document membership conflict")
        local = dt.datetime.strptime(stamps[0], "%Y%m%d%H%M%S")
    else:
        try:
            tree = html.fromstring(blob)
        except html.etree.ParserError as error:
            raise ValueError("unsupported native index layout") from error
        headings = tree.xpath("//*[@id='secNum']")
        authoritative = [" ".join(node.text_content().split()) for node in headings]
        cik_links = tree.xpath("//*[contains(concat(' ', normalize-space(@class), ' '), ' companyInfo ')]//a/@href")
        native_ciks = [int(value) for link in cik_links for value in re.findall(r"[?&]CIK=(\d+)(?:&|$)", link)]
        stamps = tree.xpath("//div[normalize-space(.)='Accepted']/following-sibling::div[1]/text()")
        members = tree.xpath("//table[contains(concat(' ', normalize-space(@class), ' '), ' tableFile ')]//a/@href")
        if authoritative != ["SEC Accession No. " + accession] or native_ciks != [issuer] or len(stamps) != 1:
            raise ValueError("authoritative native accession/issuer/clock conflict")
        if members.count(uri.path) + members.count(member_url) != 1:
            raise ValueError("native index document membership conflict")
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", stamps[0].strip()):
            raise ValueError("unsupported native acceptance timestamp")
        local = dt.datetime.fromisoformat(stamps[0].strip())
    zone = ZoneInfo.from_file(io.BytesIO(timezone_paths()[0].read_bytes()), key="America/New_York")
    values = {local.replace(tzinfo=zone, fold=fold).astimezone(dt.timezone.utc)
              for fold in (0, 1) if local.replace(tzinfo=zone, fold=fold).astimezone(dt.timezone.utc).astimezone(zone).replace(tzinfo=None) == local}
    if len(values) != 1:
        raise ValueError("ambiguous/nonexistent original acceptance wall clock")
    assert_runtime()
    return values.pop()


def native_classes(blob: bytes) -> list[dict]:
    """Same-context native DEI title/symbol pairs; no issuer-primary expansion."""
    from lxml import html
    tree = html.fromstring(blob)
    facts = {}
    ciks = set()
    for node in tree.iter():
        name = node.get("name", "").split(":")[-1]
        value = " ".join(node.text_content().split())
        if name == "EntityCentralIndexKey" and value.isdigit():
            ciks.add(int(value))
        if name in {"TradingSymbol", "Security12bTitle", "Security12gTitle"}:
            context = node.get("contextref")
            if context:
                facts.setdefault(context, {}).setdefault(name, set()).add(value)
    if len(ciks) != 1:
        raise ValueError("native issuer CIK missing/ambiguous")
    result = []
    for context, values in facts.items():
        contexts = [node for node in tree.iter() if str(node.tag).split(":")[-1].lower() == "context" and node.get("id") == context]
        if len(contexts) != 1:
            continue
        identifiers = ["".join(node.itertext()).strip() for node in contexts[0].iter()
                       if str(node.tag).split(":")[-1].lower() == "identifier"]
        if len(identifiers) != 1 or not identifiers[0].isdigit() or int(identifiers[0]) != next(iter(ciks)):
            continue
        if any("legalentity" in node.get("dimension", "").lower() for node in contexts[0].iter()):
            continue
        symbols = values.get("TradingSymbol", set())
        titles = values.get("Security12bTitle", set()) | values.get("Security12gTitle", set())
        if len(symbols) == len(titles) == 1:
            title = next(iter(titles))
            if not re.search(r"preferred|depositary|\bADR\b|\bADS\b|warrant|units?|rights?|debt", title, re.I):
                result.append({"cik": next(iter(ciks)), "context": context,
                               "symbol": next(iter(symbols)), "title": title})
    return result


def original_declaration_text(blob: bytes) -> str:
    """Read only complete, transparent original layouts without HTML repair.

    Attributes, semantic wrappers, comments, declarations and malformed markup
    are unsupported. In particular deleted/quoted/hidden text cannot become an
    affirmative declaration merely because a shared normalizer discards markup.
    """
    from lxml import etree
    if re.search(br"<!|<\?", blob):
        raise ValueError("original declaration DOM scope unsupported")
    try:
        tree = etree.fromstring(blob, parser=etree.XMLParser(
            resolve_entities=False, load_dtd=False, no_network=True, recover=False))
    except etree.XMLSyntaxError as error:
        raise ValueError("original declaration DOM layout unsupported") from error
    transparent = {"html", "body", "div", "p", "span", "b", "strong", "i", "em"}
    if tree.tag != "html" or any(
            node.tag not in transparent or node.attrib for node in tree.iter()):
        raise ValueError("original declaration DOM scope unsupported")
    return " ".join("".join(tree.itertext()).split())


def qualify(event: dict, native_text: str, *, declaration_accession: str, declaration_clock: bytes,
            class_document: bytes, class_accession: str, class_clock: bytes,
            line_rows: list[dict], declaration_url: str, declaration_document: bytes, class_url: str) -> dict:
    """Source-backed fixed regular rate on an exact native common class/dated line.

    Caller must independently admit every original blob and complete line rows.
    Output is an event-level primitive; this does not assume indefinite carry.
    """
    candidates = D.extract_declarations(native_text)
    fields = ("amount", "security_class", "currency", "special_flag", "declared_cadence", "evidence")
    if not any(all(row.get(k) == event.get(k) for k in fields) for row in candidates):
        raise ValueError("declaration does not replay original native evidence")
    if event.get("special_flag") is not False or event.get("declared_cadence") not in D.CADENCE or event.get("currency") != "USD":
        raise ValueError("fixed regular cadence/currency not established")
    if re.search(r"variable\s+(?:cash\s+)?dividend|dividend\s+policy.{0,80}(?:net\s+income|profit)|one.third\s+of", native_text, re.I | re.S):
        raise ValueError("variable-income policy cannot create fixed indicated rate")
    # Full original context must belong to a supported affirmative grammar. A
    # substring match or a finite condition blacklist cannot establish scope.
    # Additional prose (including separate qualifications/denials) is unknown.
    supported = (
        r"(?:At the annual meeting,\s+)?The board\s+"
        r"(?:declared|declares|approved)\s+on\s+" + D.DATE +
        r"\s+a\s+(?:regular\s+(?:monthly|quarterly|semiannual|semi-annual|annual)|"
        r"(?:monthly|quarterly|semiannual|semi-annual|annual)\s+regular)"
        r"\s+cash\s+dividend\s+of\s+(?:USD\s*|US\$|U\.S\.\s+dollars?\s*)"
        r"\d+(?:\.\d+)?\s+per\s+share\s+of\s+(?:Class\s+[A-Z]\s+)?common\s+(?:stock|shares)"
        r"(?:,\s+effective\s+through\s+" + D.DATE + r")?\."
    )
    original_text = original_declaration_text(declaration_document)
    if original_text != " ".join(native_text.split()):
        raise ValueError("original declaration DOM text differs from normalized evidence")
    if not re.fullmatch(supported, original_text, re.I):
        raise ValueError("complete affirmative unconditional declaration context unsupported")
    local_cadences = re.findall(r"\b(monthly|quarterly|semiannual|semi-annual|annual)\s+(?:regular\s+)?cash\s+dividend\b", event["evidence"], re.I)
    if len(local_cadences) != 1 or local_cadences[0].lower() != event["declared_cadence"]:
        raise ValueError("dividend-local cadence missing/ambiguous")
    publication = accepted_clock(declaration_clock, declaration_accession, issuer=event["cik"],
                                 member_url=declaration_url, member_blob=declaration_document)
    class_publication = accepted_clock(class_clock, class_accession, issuer=event["cik"],
                                       member_url=class_url, member_blob=class_document)
    classes = [row for row in native_classes(class_document)
               if re.match(re.escape(event["security_class"]) + r"(?:\b|,)", row["title"], re.I)]
    matches = []
    for klass in classes:
        for line in line_rows:
            if (line["cik"] == klass["cik"] and line["ticker"] == klass["symbol"]
                    and line["valid_from"] <= publication.date() <= line["valid_to"]
                    and line["available_at"] <= publication and class_publication <= publication):
                matches.append((klass, line))
    if len(matches) != 1:
        raise ValueError("native class to dated security mapping missing/ambiguous/future")
    klass, line = matches[0]
    if event.get("cik") is not None and event["cik"] != klass["cik"]:
        raise ValueError("declaration/native class issuer conflict")
    available = max(publication, class_publication, line["available_at"])
    if event.get("declare_date") is None or event["declare_date"] > publication.date() or event["declare_date"] > available.date():
        raise ValueError("native acceptance precedes declaration date or chronology is unknown")
    validity = re.search(r"effective\s+through\s+(" + D.DATE + r")", event["evidence"], re.I)
    valid_until = dt.datetime.combine(D._date(validity[1]), dt.time(22), D.UTC) if validity else None
    return {**event, "security_id": line["security_id"], "cik": klass["cik"],
            "available_at": available, "dividend_announcement_available_at": publication,
            "indicated_annual_dividend_rate": event["amount"] * D.CADENCE[event["declared_cadence"]],
            "security_class_currency_basis": event["security_class"] + "/USD",
            "source_qualification": "native_fixed_regular_event", "quote_currency_qualified": False,
            "rate_carry_status": "explicit_validity" if validity else "not_established",
            "source_valid_until": valid_until, "class_context": klass["context"]}


def carry(event: dict, cutoff: dt.datetime) -> float | None:
    """Only source-explicit validity; no arbitrary cadence-based expiry/forever carry.

    The qualifier parses explicit validity from the original declaration evidence.
    """
    source_valid_until = event.get("source_valid_until")
    if (event.get("source_qualification") != "native_fixed_regular_event" or source_valid_until is None
            or not event["available_at"] < cutoff <= source_valid_until):
        return None
    return event["indicated_annual_dividend_rate"]


def policy_state(native_text: str) -> str:
    """Explicit policy/change diagnostics; no fixed rate from silence or paid facts."""
    if re.search(r"will\s+not\s+pay\s+(?:a\s+)?dividend|(?:suspend|cancel|discontinue)\w*.{0,40}dividend", native_text, re.I):
        return "no_pay_or_cancel"
    if re.search(r"variable\s+(?:cash\s+)?dividend|dividend\s+policy.{0,80}(?:net\s+income|profit)|one.third\s+of", native_text, re.I | re.S):
        return "variable"
    rows = D.extract_declarations(native_text)
    if rows and all(row["special_flag"] is False and row["declared_cadence"] in D.CADENCE for row in rows):
        return "fixed_regular_declaration"
    return "unknown"


def rate_asof(events: list[dict], security_id: int, cutoff: dt.datetime) -> float | None:
    """Use only a complete independently admitted inventory with explicit validity.

    Caller must admit all event/clock dependencies through cutoff; this helper
    does not infer completeness from a list or successful document partitions.
    Any latest known unsupported/tombstone event invalidates older carried state.
    """
    visible = [event for event in events if event.get("security_id") == security_id and event["available_at"] < cutoff]
    if not visible:
        return None
    latest_clock = max(event["available_at"] for event in visible)
    latest = [event for event in visible if event["available_at"] == latest_clock]
    if len(latest) != 1 or latest[0].get("policy_state", "unknown") != "fixed_regular_declaration":
        return None
    return carry(latest[0], cutoff)


# Loaded normalizer identities supplement independently pinned on-disk implementations.
import zoneinfo._common as _zoneinfo_common
_COMMON_LOADER = _zoneinfo_common.load_data
_COMMON_LOADER_CODE = _COMMON_LOADER.__code__
_HEADER_LOADER = _zoneinfo_common._TZifHeader.from_file.__func__
_HEADER_LOADER_CODE = _HEADER_LOADER.__code__
_COMPILED_ZONEINFO = ZoneInfo
_CLOCK_CODE = accepted_clock.__code__
_TZPATH_CODE = timezone_paths.__code__


def assert_runtime() -> None:
    import _zoneinfo
    if (_zoneinfo_common.load_data is not _COMMON_LOADER
            or _COMMON_LOADER.__code__ is not _COMMON_LOADER_CODE
            or _zoneinfo_common._TZifHeader.from_file.__func__ is not _HEADER_LOADER
            or _HEADER_LOADER.__code__ is not _HEADER_LOADER_CODE
            or ZoneInfo is not _COMPILED_ZONEINFO or _zoneinfo.ZoneInfo is not _COMPILED_ZONEINFO
            or getattr(accepted_clock, "__code__", None) is not _CLOCK_CODE
            or getattr(timezone_paths, "__code__", None) is not _TZPATH_CODE):
        raise ValueError("timezone normalizer entrypoint changed")


def loaded_code_pins() -> dict[str, str]:
    """Stable loaded TZif implementation identities for independent admission."""
    import hashlib
    import json
    import types
    assert_runtime()
    def identity(value):
        if isinstance(value, types.CodeType):
            return {name: identity(getattr(value, name)) for name in (
                "co_code", "co_consts", "co_names", "co_varnames", "co_freevars", "co_cellvars",
                "co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals",
                "co_stacksize", "co_flags", "co_exceptiontable")}
        if isinstance(value, bytes):
            return ["bytes", value.hex()]
        if isinstance(value, tuple):
            return ["tuple", [identity(item) for item in value]]
        return [type(value).__name__, repr(value)]
    # marshal code-object serialization can change with string interning after
    # first execution. Canonical code fields avoid that process-state artifact.
    return {"runtime:zoneinfo." + name: hashlib.sha256(json.dumps(identity(function.__code__), sort_keys=True).encode()).hexdigest()
            for name, function in (("load_data", _COMMON_LOADER), ("TZifHeader.from_file", _HEADER_LOADER))}
