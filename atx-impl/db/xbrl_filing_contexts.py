from __future__ import annotations

import datetime as dt
import hashlib
import re
from dataclasses import dataclass
from typing import Any

import pandas as pd
from lxml import etree

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT, sec_session
from .warehouse import insert_frame, json_dumps, now_utc_naive, quality_check, record_source_file, symbol_key


SOURCE_NAME = "SEC filing inline XBRL"
SEC_ARCHIVE_BASE_URL = "https://www.sec.gov/Archives/edgar/data"


@dataclass(frozen=True)
class XbrlFilingContextOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    forms: tuple[str, ...] = ("10-K", "10-Q")
    accession_numbers: tuple[str, ...] | None = None
    max_filings: int = 3
    max_filings_per_symbol: int | None = None
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None


def archive_primary_document_url(cik: str, accession_number: str, primary_document: str) -> str:
    cik_path = str(int(str(cik)))
    accession_path = str(accession_number).replace("-", "")
    document = str(primary_document).strip().lstrip("/")
    return f"{SEC_ARCHIVE_BASE_URL}/{cik_path}/{accession_path}/{document}"


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _qname_parts(value: str | None) -> tuple[str | None, str | None]:
    if value is None:
        return None, None
    cleaned = str(value).strip()
    if not cleaned:
        return None, None
    if cleaned.startswith("{") and "}" in cleaned:
        namespace, local = cleaned[1:].split("}", 1)
        return namespace, local
    if ":" in cleaned:
        prefix, local = cleaned.split(":", 1)
        return prefix, local
    return None, cleaned


def _local_name(element: etree._Element | None) -> str | None:
    if element is None:
        return None
    return etree.QName(element).localname


def _is_hidden(element: etree._Element) -> bool:
    parent = element.getparent()
    while parent is not None:
        if _local_name(parent) == "hidden":
            return True
        parent = parent.getparent()
    return False


def _first_text(element: etree._Element, xpath: str) -> str | None:
    values = element.xpath(xpath)
    for value in values:
        text = str(value).strip()
        if text:
            return text
    return None


def _element_text(element: etree._Element) -> str:
    pieces: list[str] = []

    def walk(node: etree._Element) -> None:
        if node.text and _local_name(node) != "exclude":
            pieces.append(node.text)
        for child in node:
            if _local_name(child) != "exclude":
                walk(child)
            if child.tail:
                pieces.append(child.tail)

    walk(element)
    return " ".join("".join(pieces).split())


def _continuation_text(root: etree._Element, continued_at: str | None, seen: set[str] | None = None) -> str:
    if not continued_at:
        return ""
    seen = seen or set()
    if continued_at in seen:
        return ""
    seen.add(continued_at)
    matches = root.xpath(f'//*[local-name()="continuation" and @id=$continued_at]', continued_at=continued_at)
    if not matches:
        return ""
    continuation = matches[0]
    text = _element_text(continuation)
    next_id = continuation.get("continuedAt")
    next_text = _continuation_text(root, next_id, seen)
    return " ".join(part for part in (text, next_text) if part)


def _fact_text(root: etree._Element, element: etree._Element) -> str:
    text = _element_text(element)
    continuation = _continuation_text(root, element.get("continuedAt"))
    return " ".join(part for part in (text, continuation) if part)


def _date(value: str | None) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def _context_element(member: etree._Element) -> str:
    parent = member.getparent()
    while parent is not None:
        local = _local_name(parent)
        if local in {"segment", "scenario"}:
            return local
        if local == "context":
            break
        parent = parent.getparent()
    return "unknown"


def _period_fields(context: etree._Element) -> dict[str, Any]:
    start = _date(_first_text(context, './*[local-name()="period"]/*[local-name()="startDate"]/text()'))
    end = _date(_first_text(context, './*[local-name()="period"]/*[local-name()="endDate"]/text()'))
    instant = _date(_first_text(context, './*[local-name()="period"]/*[local-name()="instant"]/text()'))
    has_forever = bool(context.xpath('./*[local-name()="period"]/*[local-name()="forever"]'))
    if instant is not None:
        period_type = "instant"
    elif start is not None or end is not None:
        period_type = "duration"
    elif has_forever:
        period_type = "forever"
    else:
        period_type = "unknown"
    return {
        "period_type": period_type,
        "period_start": start,
        "period_end": end,
        "instant_date": instant,
    }


def _int_or_none(value: str | None) -> int | None:
    if value in (None, ""):
        return None
    try:
        return int(str(value))
    except ValueError:
        return None


def _unit_payloads(root: etree._Element) -> dict[str, dict[str, str]]:
    units: dict[str, dict[str, str]] = {}
    for unit in root.xpath('//*[local-name()="unit"]'):
        unit_id = unit.get("id")
        if not unit_id:
            continue
        measures = ["".join(measure.itertext()).strip() for measure in unit.xpath('./*[local-name()="measure"]')]
        numerator = [
            "".join(measure.itertext()).strip()
            for measure in unit.xpath('./*[local-name()="divide"]/*[local-name()="unitNumerator"]/*[local-name()="measure"]')
        ]
        denominator = [
            "".join(measure.itertext()).strip()
            for measure in unit.xpath('./*[local-name()="divide"]/*[local-name()="unitDenominator"]/*[local-name()="measure"]')
        ]
        units[str(unit_id)] = {
            "unit_measures_json": json_dumps([value for value in measures if value]),
            "unit_numerator_measures_json": json_dumps([value for value in numerator if value]),
            "unit_denominator_measures_json": json_dumps([value for value in denominator if value]),
        }
    return units


def _numeric_value(raw_value: str | None, *, scale: int | None, sign: str | None) -> float | None:
    if raw_value in (None, ""):
        return None
    text = str(raw_value).strip()
    if not text:
        return None
    negative = False
    if text.startswith("(") and text.endswith(")"):
        negative = True
        text = text[1:-1]
    if sign and str(sign).strip() == "-":
        negative = not negative
    cleaned = text.replace(",", "").replace("$", "").replace("%", "")
    cleaned = cleaned.replace("\u2212", "-").replace("\u2013", "-").replace("\u2014", "-")
    cleaned = re.sub(r"\s+", "", cleaned)
    if cleaned in {"", "-", "--", "N/A", "n/a"}:
        return None
    if not re.fullmatch(r"[-+]?\d+(\.\d+)?", cleaned):
        return None
    try:
        value = float(cleaned)
    except ValueError:
        return None
    if negative and value > 0:
        value = -value
    if scale is not None:
        value *= 10**scale
    return value


def _fact_rows(
    *,
    root: etree._Element,
    filing: dict[str, Any],
    source_url: str,
    run_id: str | None,
    source_loaded_at: dt.datetime,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    units = _unit_payloads(root)
    fact_elements = root.xpath('//*[local-name()="nonFraction" or local-name()="nonNumeric" or local-name()="fraction"]')
    for ordinal, element in enumerate(fact_elements, start=1):
        qname = element.get("name")
        context_ref = element.get("contextRef")
        if not qname or not context_ref:
            continue
        taxonomy, concept = _qname_parts(qname)
        filing_context_id = _sha256_text(
            "|".join(
                [
                    "xbrl_filing_context",
                    str(filing["security_id"]),
                    str(filing["accession_number"]),
                    str(filing["primary_document"]),
                    str(context_ref),
                ]
            )
        )
        fact_kind = str(_local_name(element) or "")
        raw_value = _fact_text(root, element)
        scale = _int_or_none(element.get("scale"))
        unit_ref = element.get("unitRef")
        unit_payload = units.get(str(unit_ref), {}) if unit_ref else {}
        numeric_value = _numeric_value(raw_value, scale=scale, sign=element.get("sign")) if fact_kind != "nonNumeric" else None
        row_key = "|".join(
            [
                str(filing["security_id"]),
                str(filing["accession_number"]),
                str(filing["primary_document"]),
                str(ordinal),
                str(element.get("id") or ""),
                str(qname),
                str(context_ref),
                str(unit_ref or ""),
            ]
        )
        rows.append(
            {
                "filing_fact_id": _sha256_text(f"xbrl_filing_fact|{row_key}"),
                "filing_context_id": filing_context_id,
                "security_id": filing["security_id"],
                "cik": filing["cik"],
                "accession_number": filing["accession_number"],
                "form": filing["form"],
                "filing_date": filing["filing_date"],
                "acceptance_datetime": filing["acceptance_datetime"],
                "primary_document": filing["primary_document"],
                "fact_ordinal": ordinal,
                "fact_kind": fact_kind,
                "ix_id": element.get("id"),
                "qname": qname,
                "taxonomy": taxonomy,
                "concept": concept,
                "context_ref": context_ref,
                "unit_ref": unit_ref,
                "unit_measures_json": unit_payload.get("unit_measures_json", "[]"),
                "unit_numerator_measures_json": unit_payload.get("unit_numerator_measures_json", "[]"),
                "unit_denominator_measures_json": unit_payload.get("unit_denominator_measures_json", "[]"),
                "decimals": element.get("decimals"),
                "precision": element.get("precision"),
                "scale": scale,
                "sign": element.get("sign"),
                "format": element.get("format"),
                "continued_at": element.get("continuedAt"),
                "is_hidden": _is_hidden(element),
                "raw_value": raw_value,
                "normalized_value": raw_value,
                "numeric_value": numeric_value,
                "is_numeric": fact_kind in {"nonFraction", "fraction"},
                "source_line": element.sourceline,
                "source_url": source_url,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )
    return rows


def _dimension_rows(
    *,
    filing_context_id: str,
    context: etree._Element,
    filing: dict[str, Any],
    source_url: str,
    run_id: str | None,
    source_loaded_at: dt.datetime,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    ordinal = 0
    context_id = str(context.get("id") or "")
    for member in context.xpath('.//*[local-name()="explicitMember"]'):
        ordinal += 1
        dimension_qname = member.get("dimension")
        member_qname = "".join(member.itertext()).strip()
        dimension_taxonomy, dimension_concept = _qname_parts(dimension_qname)
        member_taxonomy, member_concept = _qname_parts(member_qname)
        row_key = "|".join(
            [
                filing_context_id,
                str(ordinal),
                "explicit",
                str(dimension_qname or ""),
                str(member_qname or ""),
            ]
        )
        rows.append(
            {
                "filing_dimension_id": _sha256_text(f"xbrl_filing_dimension|{row_key}"),
                "filing_context_id": filing_context_id,
                "security_id": filing["security_id"],
                "cik": filing["cik"],
                "accession_number": filing["accession_number"],
                "form": filing["form"],
                "filing_date": filing["filing_date"],
                "acceptance_datetime": filing["acceptance_datetime"],
                "primary_document": filing["primary_document"],
                "context_id": context_id,
                "context_element": _context_element(member),
                "member_kind": "explicit",
                "dimension_qname": dimension_qname,
                "dimension_taxonomy": dimension_taxonomy,
                "dimension_concept": dimension_concept,
                "member_qname": member_qname,
                "member_taxonomy": member_taxonomy,
                "member_concept": member_concept,
                "typed_member_value": None,
                "member_text": member_qname,
                "ordinal": ordinal,
                "source_url": source_url,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )

    for member in context.xpath('.//*[local-name()="typedMember"]'):
        ordinal += 1
        dimension_qname = member.get("dimension")
        dimension_taxonomy, dimension_concept = _qname_parts(dimension_qname)
        typed_value = " ".join("".join(member.itertext()).split())
        typed_child = next(iter(member), None)
        typed_taxonomy, typed_concept = _qname_parts(typed_child.tag if typed_child is not None else None)
        row_key = "|".join(
            [
                filing_context_id,
                str(ordinal),
                "typed",
                str(dimension_qname or ""),
                typed_value,
            ]
        )
        rows.append(
            {
                "filing_dimension_id": _sha256_text(f"xbrl_filing_dimension|{row_key}"),
                "filing_context_id": filing_context_id,
                "security_id": filing["security_id"],
                "cik": filing["cik"],
                "accession_number": filing["accession_number"],
                "form": filing["form"],
                "filing_date": filing["filing_date"],
                "acceptance_datetime": filing["acceptance_datetime"],
                "primary_document": filing["primary_document"],
                "context_id": context_id,
                "context_element": _context_element(member),
                "member_kind": "typed",
                "dimension_qname": dimension_qname,
                "dimension_taxonomy": dimension_taxonomy,
                "dimension_concept": dimension_concept,
                "member_qname": None if typed_concept is None else typed_child.tag,
                "member_taxonomy": typed_taxonomy,
                "member_concept": typed_concept,
                "typed_member_value": typed_value,
                "member_text": typed_value,
                "ordinal": ordinal,
                "source_url": source_url,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )
    return rows


def parse_inline_xbrl_contexts(
    content: bytes,
    *,
    filing: dict[str, Any],
    source_url: str,
    run_id: str | None,
    source_loaded_at: dt.datetime | None = None,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    contexts, dimensions, _facts = parse_inline_xbrl_filing(
        content,
        filing=filing,
        source_url=source_url,
        run_id=run_id,
        source_loaded_at=source_loaded_at,
    )
    return contexts, dimensions


def parse_inline_xbrl_filing(
    content: bytes,
    *,
    filing: dict[str, Any],
    source_url: str,
    run_id: str | None,
    source_loaded_at: dt.datetime | None = None,
) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    source_loaded_at = source_loaded_at or now_utc_naive()
    parser = etree.XMLParser(recover=True, huge_tree=True)
    root = etree.fromstring(content, parser=parser)
    contexts = root.xpath('//*[local-name()="context"]')

    context_rows: list[dict[str, Any]] = []
    dimension_rows: list[dict[str, Any]] = []
    for context in contexts:
        context_id = str(context.get("id") or "")
        if not context_id:
            continue
        filing_context_id = _sha256_text(
            "|".join(
                [
                    "xbrl_filing_context",
                    str(filing["security_id"]),
                    str(filing["accession_number"]),
                    str(filing["primary_document"]),
                    context_id,
                ]
            )
        )
        identifier = context.xpath('./*[local-name()="entity"]/*[local-name()="identifier"]')
        identifier_element = identifier[0] if identifier else None
        explicit_count = len(context.xpath('.//*[local-name()="explicitMember"]'))
        typed_count = len(context.xpath('.//*[local-name()="typedMember"]'))
        period = _period_fields(context)
        context_rows.append(
            {
                "filing_context_id": filing_context_id,
                "security_id": filing["security_id"],
                "cik": filing["cik"],
                "accession_number": filing["accession_number"],
                "form": filing["form"],
                "filing_date": filing["filing_date"],
                "report_date": filing["report_date"],
                "acceptance_datetime": filing["acceptance_datetime"],
                "primary_document": filing["primary_document"],
                "context_id": context_id,
                "entity_identifier_scheme": None if identifier_element is None else identifier_element.get("scheme"),
                "entity_identifier": None if identifier_element is None else "".join(identifier_element.itertext()).strip(),
                "period_type": period["period_type"],
                "period_start": period["period_start"],
                "period_end": period["period_end"],
                "instant_date": period["instant_date"],
                "has_segment": bool(context.xpath('.//*[local-name()="segment"]')),
                "has_scenario": bool(context.xpath('.//*[local-name()="scenario"]')),
                "explicit_member_count": explicit_count,
                "typed_member_count": typed_count,
                "dimension_count": explicit_count + typed_count,
                "context_hash": hashlib.sha256(etree.tostring(context)).hexdigest(),
                "source_url": source_url,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )
        dimension_rows.extend(
            _dimension_rows(
                filing_context_id=filing_context_id,
                context=context,
                filing=filing,
                source_url=source_url,
                run_id=run_id,
                source_loaded_at=source_loaded_at,
            )
        )

    return pd.DataFrame(context_rows), pd.DataFrame(dimension_rows), pd.DataFrame(
        _fact_rows(root=root, filing=filing, source_url=source_url, run_id=run_id, source_loaded_at=source_loaded_at)
    )


def _target_filings(store: DuckDBStore, options: XbrlFilingContextOptions) -> pd.DataFrame:
    filters: list[str] = [
        "s.primary_document IS NOT NULL",
        "s.primary_document <> ''",
        "coalesce(s.is_inline_xbrl, false)",
    ]
    params: list[Any] = [options.max_filings_per_symbol, options.max_filings_per_symbol, options.max_filings]
    if options.forms:
        filters.append("s.form IN (SELECT form FROM xbrl_context_form_filter)")
    if options.accession_numbers:
        filters.append("s.accession_number IN (SELECT accession_number FROM xbrl_context_accession_filter)")
    if options.symbols:
        filters.append("s.security_id IN (SELECT security_id FROM xbrl_context_symbol_filter)")

    if options.forms:
        store.con.register("xbrl_context_form_filter", pd.DataFrame({"form": [str(form).upper() for form in options.forms]}))
    if options.accession_numbers:
        store.con.register(
            "xbrl_context_accession_filter",
            pd.DataFrame({"accession_number": [str(value).strip() for value in options.accession_numbers]}),
        )
    if options.symbols:
        symbols = sorted({symbol_key(symbol) for symbol in options.symbols})
        store.con.register("xbrl_context_symbol_input", pd.DataFrame({"ticker": symbols}))
        store.con.execute(
            """
            CREATE TEMPORARY TABLE xbrl_context_symbol_filter AS
            SELECT DISTINCT t.security_id
            FROM xbrl_context_symbol_input i
            JOIN sec_company_tickers t ON t.ticker = i.ticker
            """
        )
    try:
        frame = store.con.execute(
            f"""
            WITH ranked AS (
            SELECT
                s.security_id,
                s.cik,
                s.accession_number,
                s.form,
                s.filing_date,
                s.report_date,
                s.acceptance_datetime,
                s.primary_document,
                row_number() OVER (
                    PARTITION BY s.security_id
                    ORDER BY
                        s.filing_date DESC NULLS LAST,
                        s.acceptance_datetime DESC NULLS LAST,
                        s.accession_number DESC,
                        s.primary_document
                ) AS filing_rank
            FROM sec_submissions s
            WHERE {" AND ".join(filters)}
            )
            SELECT
                security_id,
                cik,
                accession_number,
                form,
                filing_date,
                report_date,
                acceptance_datetime,
                primary_document
            FROM ranked
            WHERE (? IS NULL OR filing_rank <= ?)
            ORDER BY
                filing_date DESC NULLS LAST,
                acceptance_datetime DESC NULLS LAST,
                accession_number DESC,
                primary_document
            LIMIT ?
            """,
            params,
        ).df()
    finally:
        for relation in (
            "xbrl_context_form_filter",
            "xbrl_context_accession_filter",
            "xbrl_context_symbol_input",
            "xbrl_context_symbol_filter",
        ):
            try:
                store.con.unregister(relation)
            except Exception:
                try:
                    store.con.execute(f"DROP TABLE IF EXISTS {relation}")
                except Exception:
                    pass
    return frame


class XbrlFilingContextDataset(Dataset):
    dataset_id = "xbrl_filing_contexts"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: XbrlFilingContextOptions) -> DatasetLoadResult:
        if options.max_filings < 1:
            raise ValueError("max_filings must be positive")
        if options.max_filings_per_symbol is not None and options.max_filings_per_symbol < 1:
            raise ValueError("max_filings_per_symbol must be positive when provided")
        filings = _target_filings(store, options)
        if filings.empty:
            raise RuntimeError("No inline XBRL SEC submissions matched the filing context options")

        session = sec_session(options.user_agent)
        context_frames: list[pd.DataFrame] = []
        dimension_frames: list[pd.DataFrame] = []
        fact_frames: list[pd.DataFrame] = []
        filing_keys: list[dict[str, Any]] = []
        source_loaded_at = now_utc_naive()
        for filing_row in filings.to_dict("records"):
            source_url = archive_primary_document_url(
                str(filing_row["cik"]),
                str(filing_row["accession_number"]),
                str(filing_row["primary_document"]),
            )
            response = session.get(source_url, timeout=options.request_timeout)
            response.raise_for_status()
            content = response.content
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=source_url,
                status="fetched",
                metadata={
                    "security_id": filing_row["security_id"],
                    "cik": filing_row["cik"],
                    "accession_number": filing_row["accession_number"],
                    "primary_document": filing_row["primary_document"],
                    "byte_count": len(content),
                },
                sha256=hashlib.sha256(content).hexdigest(),
            )
            contexts, dimensions, facts = parse_inline_xbrl_filing(
                content,
                filing=filing_row,
                source_url=source_url,
                run_id=options.run_id,
                source_loaded_at=source_loaded_at,
            )
            context_frames.append(contexts)
            dimension_frames.append(dimensions)
            fact_frames.append(facts)
            filing_keys.append(
                {
                    "security_id": filing_row["security_id"],
                    "accession_number": filing_row["accession_number"],
                    "primary_document": filing_row["primary_document"],
                }
            )

        context_frame = pd.concat([frame for frame in context_frames if not frame.empty], ignore_index=True) if context_frames else pd.DataFrame()
        dimension_frame = pd.concat([frame for frame in dimension_frames if not frame.empty], ignore_index=True) if dimension_frames else pd.DataFrame()
        fact_frame = pd.concat([frame for frame in fact_frames if not frame.empty], ignore_index=True) if fact_frames else pd.DataFrame()
        loaded_contexts, loaded_dimensions, loaded_facts = self._replace_rows(
            store,
            pd.DataFrame(filing_keys),
            context_frame,
            dimension_frame,
            fact_frame,
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="xbrl_filing_contexts",
            check_name="rows_loaded",
            status="passed" if loaded_contexts > 0 else "warning",
            observed_value=float(loaded_contexts),
            threshold_value=1.0,
            details={
                "symbols": options.symbols,
                "forms": options.forms,
                "accession_numbers": options.accession_numbers,
                "max_filings": options.max_filings,
                "max_filings_per_symbol": options.max_filings_per_symbol,
                "filings": len(filing_keys),
                "dimensions": loaded_dimensions,
                "facts": loaded_facts,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=loaded_contexts + loaded_facts,
            source=SOURCE_NAME,
            details={
                "symbols": options.symbols,
                "forms": options.forms,
                "accession_numbers": options.accession_numbers,
                "max_filings": options.max_filings,
                "max_filings_per_symbol": options.max_filings_per_symbol,
                "filings": len(filing_keys),
                "contexts": loaded_contexts,
                "dimensions": loaded_dimensions,
                "facts": loaded_facts,
            },
        )

    def _replace_rows(
        self,
        store: DuckDBStore,
        filing_keys: pd.DataFrame,
        context_frame: pd.DataFrame,
        dimension_frame: pd.DataFrame,
        fact_frame: pd.DataFrame,
    ) -> tuple[int, int, int]:
        if filing_keys.empty:
            return 0, 0, 0
        with store.transaction():
            store.con.register("xbrl_filing_context_keys_load", filing_keys)
            try:
                store.con.execute(
                    """
                    CREATE TEMPORARY TABLE xbrl_filing_context_ids_delete AS
                    SELECT c.filing_context_id
                    FROM xbrl_filing_contexts c
                    JOIN xbrl_filing_context_keys_load k
                      ON k.security_id = c.security_id
                     AND k.accession_number = c.accession_number
                     AND k.primary_document = c.primary_document
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM xbrl_filing_facts
                    WHERE filing_context_id IN (
                        SELECT filing_context_id FROM xbrl_filing_context_ids_delete
                    )
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM xbrl_filing_dimensions
                    WHERE filing_context_id IN (
                        SELECT filing_context_id FROM xbrl_filing_context_ids_delete
                    )
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM xbrl_filing_contexts
                    WHERE filing_context_id IN (
                        SELECT filing_context_id FROM xbrl_filing_context_ids_delete
                    )
                    """
                )
                if not context_frame.empty:
                    insert_frame(store, context_frame, "xbrl_filing_contexts", "xbrl_filing_contexts_insert")
                if not dimension_frame.empty:
                    insert_frame(store, dimension_frame, "xbrl_filing_dimensions", "xbrl_filing_dimensions_insert")
                if not fact_frame.empty:
                    insert_frame(store, fact_frame, "xbrl_filing_facts", "xbrl_filing_facts_insert")
            finally:
                try:
                    store.con.unregister("xbrl_filing_context_keys_load")
                except Exception:
                    pass
                store.con.execute("DROP TABLE IF EXISTS xbrl_filing_context_ids_delete")
        return int(len(context_frame)), int(len(dimension_frame)), int(len(fact_frame))
