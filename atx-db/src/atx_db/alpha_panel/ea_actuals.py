"""Announcement actuals qualified directly from EX-99 quarterly income tables.

Unknown periods, units, currencies, conflicting GAAP rows and acceptance clocks
are missing, with reasons. No later filing supplies a release's value or period.
SUE is a seasonal random walk scaled by eight *prior* visible surprises.
Reaction returns are published separately, at window completion, before 2023.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C
from . import events_sources as ES
from . import ea_publication as P

STAGE = "ea_actuals"
SCHEMA = "atx.alpha-panel.ea-actuals/v1"
VERSION = "ex99-qualified-quarter-v4"
HOLDOUT = dt.date(2023, 1, 1)
FIELDS = ("saleq", "epsfiq", "niq")
MONTHS = "January February March April May June July August September October November December".split()
DATE = re.compile(r"\b(" + "|".join(MONTHS) + r")\s+(\d{1,2}),?\s+(20\d{2})\b", re.I)
BAD_BASIS = re.compile(r"adjusted|non.?gaap|pro.?forma|constant.currency|guidance|outlook|segment", re.I)


def dates(text: str) -> list[dt.date]:
    result = []
    for month, day, year in DATE.findall(text):
        try:
            result.append(dt.date(int(year), [m.lower() for m in MONTHS].index(month.lower()) + 1, int(day)))
        except ValueError:
            pass
    return result


def number(text: str) -> float | None:
    text = text.strip().replace("\u2212", "-").replace("\xa0", " ")
    if not re.fullmatch(r"\(?-?\s*\$?\s*\d[\d,]*(?:\.\d+)?\s*\)?", text):
        return None  # dashes, footnote numbers, percent and invented zero excluded
    negative = text.startswith("(") or text.startswith("-")
    value = float(re.sub(r"[(),$\s-]", "", text))
    return -value if negative else value


def metric(label: str) -> str | None:
    label = re.sub(r"\s+", " ", label).strip().lower().rstrip(":")
    if BAD_BASIS.search(label):
        return None
    if re.fullmatch(r"(?:total |net )?(?:revenues?|sales)(?:,? net)?", label):
        return "saleq"
    if re.fullmatch(r"(?:net income|net loss|net income \(loss\)|net \(loss\) income)(?: attributable to (?:the )?(?:company|parent))?", label):
        return "niq"
    if re.fullmatch(r"(?:diluted (?:earnings|loss|earnings \(loss\)) per (?:common )?share|(?:earnings|loss|earnings \(loss\)) per (?:common )?share[- ,:]*(?:assuming dilution|diluted))", label):
        return "epsfiq"
    return None


def release_period(text: str, filing_date: dt.date) -> tuple[dt.date | None, str]:
    """Source-only current reporting period, independent of value extraction.

    Bind explicit end dates to a quarter/year results statement. Comparatives
    farther than 150 days before filing cannot substitute for a current period.
    The broad fiscal-quarter/year labels require a qualified historical calendar
    and are intentionally excluded here.
    """
    current = set()
    word = r"(?:quarter|three[- ]months?|3[- ]months?|fiscal year|year|six[- ]months?|nine[- ]months?)"
    for match in re.finditer(word + r"\s+(?:period\s+)?(?:ended|ending)\s+(?:on\s+)?(" + DATE.pattern + r")", text, re.I):
        for date in dates(match.group()):
            if 0 <= (filing_date-date).days <= 150:
                current.add(date)
    if len(current) == 1:
        return next(iter(current)), "source_explicit_end_date"
    return None, "source_period_ambiguous" if current else "source_period_not_explicit"


def fiscal_identity(text: str,period_end: dt.date | None=None) -> dict[str, Any]:
    """Only an explicit fiscal quarter/year and fiscal-year-end declaration qualify."""
    labels = {(int(year), {"first":1,"second":2,"third":3,"fourth":4}[quarter.lower()])
              for quarter,year in re.findall(r"\b(first|second|third|fourth) quarter(?: of)?(?: fiscal)?(?: year)?\s+(20\d{2})\b",text[:1500],re.I)}
    ends = {(month.lower(),int(day)) for month,day in re.findall(
        r"fiscal year (?:ends|ending|end is)\s+("+'|'.join(MONTHS)+r")\s+(\d{1,2})",text,re.I)}
    if len(labels)==1 and len(ends)==1:
        year,quarter=next(iter(labels)); month,day=next(iter(ends))
        end_month=MONTHS.index(month.capitalize())+1
        expected_month=(end_month-3*(4-quarter)-1)%12+1
        if period_end is None or period_end.month!=expected_month:
            return {"fiscal_year_label":None,"fiscal_quarter":None,"fiscal_calendar_id":None,
                    "quarter_identity_reason":"source_quarter_calendar_period_inconsistent"}
        return {"fiscal_year_label":year,"fiscal_quarter":quarter,
                "fiscal_calendar_id":f"{MONTHS.index(month.capitalize())+1:02d}-{day:02d}",
                "quarter_identity_reason":"source_explicit_quarter_year_and_fye"}
    return {"fiscal_year_label":None,"fiscal_quarter":None,"fiscal_calendar_id":None,
            "quarter_identity_reason":"fiscal_quarter_or_calendar_unqualified"}


def quarter_columns(rows: list[list[tuple[int,int,str]]], prefix: str) -> dict[tuple[int,int],dt.date]:
    """Bind source dates to physical quarter-only column bands.

    Split month/day plus year headers need an enclosing duration span. YTD
    spans, overlapping/contradictory labels and percent-change columns never
    supply quarter observations. Rowspan support remains deliberately absent.
    """
    header_rows=[]
    for row in rows:
        if any(metric(txt) for _,_,txt in row):
            break
        header_rows.append(row)
    cells=[c for row in header_rows for c in row]
    text=prefix+' '+' '.join(txt for _,_,txt in cells)
    quarter=re.compile(r'three[- ]months?|3[- ]months?|quarter ended',re.I)
    ytd=re.compile(r'six[- ]months?|nine[- ]months?|twelve[- ]months?|year ended|annual',re.I)
    mixed=bool(ytd.search(text))
    duration=[]
    ytd_bands=[]
    percent_bands=[]
    for lo,hi,txt in cells:
        if quarter.search(txt) and not ytd.search(txt):
            duration.append((lo,hi,txt))
        if ytd.search(txt):
            ytd_bands.append((lo,hi))
        if re.search(r'%|\b(?:percent(?:age)?|change|growth|decline)\b',txt,re.I):
            percent_bands.append((lo,hi))
    def intersects(lo: int,hi: int,a: int,b: int) -> bool:
        return max(lo,a)<min(hi,b)
    # Conflicting duration ancestry makes the grid unqualified, even if a
    # subordinate date cell itself falls outside the crossing part of a span.
    if any(intersects(lo,hi,a,b) for lo,hi,_ in duration for a,b in ytd_bands):
        return {}
    def admissible(lo: int,hi: int) -> bool:
        if any(intersects(lo,hi,a,b) for a,b in (*ytd_bands,*percent_bands)):
            return False
        crossing=[(a,b) for a,b,_ in duration if intersects(lo,hi,a,b) and not (a<=lo and hi<=b)]
        return not crossing
    candidates: dict[tuple[int,int],set[dt.date]]=defaultdict(set)
    for lo,hi,txt in cells:
        if not admissible(lo,hi):
            continue
        parents=[s for a,b,s in duration if a<=lo and hi<=b]
        if mixed and not parents:
            continue
        for date in dates(txt):
            candidates[(lo,hi)].add(date)
        if re.fullmatch(r'20\d{2}',txt):
            for parent in parents:
                md=re.findall(r'\b('+'|'.join(MONTHS)+r')\s+(\d{1,2}),?(?!\d)',parent,re.I)
                if len(md)==1 and not dates(parent):
                    try:
                        candidates[(lo,hi)].add(dt.date(int(txt),MONTHS.index(md[0][0].capitalize())+1,int(md[0][1])))
                    except ValueError:
                        pass
    # Abbott-style 2Q15/2Q14 requires the explicit quarter/date pair in the
    # immediately preceding caption; no fiscal-calendar inference is used.
    caption=re.search(r'\b(first|second|third|fourth) quarter ended\s+('+'|'.join(MONTHS)+r')\s+(\d{1,2}),?\s+(20\d{2})\s+and\s+(20\d{2})\b',prefix,re.I)
    if caption and not mixed:
        ordinal={'first':1,'second':2,'third':3,'fourth':4}[caption[1].lower()]
        years={int(caption[4]),int(caption[5])}
        for lo,hi,txt in cells:
            if not admissible(lo,hi):
                continue
            token=re.fullmatch(r'([1-4])Q(\d{2}|20\d{2})',txt,re.I)
            if token and int(token[1])==ordinal:
                year=int(token[2]); year=year+2000 if year<100 else year
                if year in years:
                    try:
                        candidates[(lo,hi)].add(dt.date(year,MONTHS.index(caption[2].capitalize())+1,int(caption[3])))
                    except ValueError:
                        pass
    if any(len(ds)!=1 for ds in candidates.values()):
        return {}
    result={band:next(iter(ds)) for band,ds in candidates.items()}
    # A broad date span plus subordinate date columns is ambiguous geometry.
    bands=list(result)
    if any(max(a,c)<min(b,d) for i,(a,b) in enumerate(bands) for c,d in bands[i+1:]):
        return {}
    return result


def parse_release(blob: bytes, filing_date: dt.date) -> dict[str, Any]:
    """Strict current-quarter table extraction; conflicts are never rank-selected.

    Header dates must align with physical numeric cells; explicit duration spans
    may separate quarter columns from YTD. Currency/scale must be declared in or
    immediately before that table. EPS bypasses monetary scale only when the
    source explicitly excludes per-share amounts from the monetary scale.
    """
    from lxml import html

    empty = {f: None for f in FIELDS}
    out = {**empty, "period_end": None, "currency": None, "period_days": None,
           "reason": "no_qualified_quarter_table", "evidence": [],"extraction_reasons":[]}
    try:
        root = html.fromstring(blob)
    except (ValueError, TypeError):
        out["reason"] = "unreadable_document"
        return out
    pe, period_reason = release_period(" ".join(root.text_content().split()), filing_date)
    out.update(period_end=pe, period_basis=period_reason)
    out.update(fiscal_identity(' '.join(root.text_content().split()),pe))
    candidates: dict[tuple[dt.date, str], list[tuple[float, str, str]]] = defaultdict(list)
    for table_no, table in enumerate(root.xpath("//table")):
        # Container/layout tables must not leak descendant tables into one grid.
        if table.xpath(".//table"):
            continue
        trs = table.xpath("./tr|./tbody/tr|./thead/tr|./tfoot/tr")
        rows = [[" ".join(c.text_content().split()) for c in tr.xpath("./td|./th")] for tr in trs]
        if not rows:
            continue
        text = " ".join(" ".join(r) for r in rows)
        prefix_nodes = table.xpath("preceding-sibling::*[position()<=4]")
        prefix = " ".join(n.text_content()[-800:] for n in prefix_nodes)
        context = prefix + " " + text[:1800]
        if BAD_BASIS.search(context) or not re.search(r"three months|3 months|quarter ended", context, re.I):
            out["extraction_reasons"].append(f"table_{table_no}:non_gaap_or_quarter_duration_unqualified")
            continue
        physical_rows=[]
        invalid_geometry=False
        for tr,cells in zip(trs,rows):
            physical=[]; offset=0
            for el,txt in zip(tr.xpath('./td|./th'),cells):
                try:
                    span=int(el.get('colspan','1')); rowspan=int(el.get('rowspan','1'))
                except ValueError:
                    invalid_geometry=True; break
                if span<1 or span>30 or rowspan!=1:
                    invalid_geometry=True; break
                physical.append((offset,offset+span,txt)); offset+=span
            physical_rows.append(physical)
        if invalid_geometry:
            out['extraction_reasons'].append(f'table_{table_no}:unsupported_header_geometry')
            continue
        header=quarter_columns(physical_rows,prefix)
        if not header:
            out['extraction_reasons'].append(f'table_{table_no}:quarter_column_geometry_unqualified')
            continue
        currencies = set(re.findall(r"\b(USD|CAD|EUR|GBP|RMB|CNY|JPY)\b", context.upper()))
        if re.search(r"U\.?S\.? dollars|United States dollars", context, re.I):
            currencies.add("USD")
        if len(currencies) != 1:
            out["extraction_reasons"].append(f"table_{table_no}:currency_missing_or_ambiguous")
            continue
        currency = next(iter(currencies))
        scales = set(re.findall(r"in (thousands|millions|billions)", context, re.I))
        scale = {"thousands": 1e3, "millions": 1e6, "billions": 1e9}.get(next(iter(scales)).lower()) if len(scales) == 1 else None
        if scale is None and re.search(r"(?:in |\()dollars(?:,|\)| except)", context, re.I):
            scale = 1.0
        if scale is None:
            out["extraction_reasons"].append(f"table_{table_no}:monetary_scale_unqualified")
            continue
        per_share = bool(re.search(r"except[^.;\n]{0,80}(?:per.share|earnings per share)", context, re.I))
        current = [d for d in header.values() if 0 <= (filing_date - d).days <= 150]
        if not current:
            out["extraction_reasons"].append(f"table_{table_no}:period_column_unqualified")
            continue
        pe = max(current)
        for row_no, (cells, physical) in enumerate(zip(rows, physical_rows)):
            if not cells:
                continue
            field = metric(cells[0])
            if field is None or (field == "epsfiq" and not per_share):
                continue
            for (lo, hi), d in header.items():
                if not (d == pe or 345 <= (pe-d).days <= 385):
                    continue
                band = [txt for begin,end,txt in physical if begin>=lo and end<=hi]
                if sum(bool(re.search(r'\d',txt)) for txt in band)!=1:
                    continue  # split currency/parenthesis cells are allowed, two amounts are not
                cell_text = ''.join(band)
                value = number(cell_text)
                if value is None:
                    continue
                value *= 1.0 if field == "epsfiq" else scale
                evidence = json.dumps({"table": table_no, "row": row_no, "columns": [lo,hi],
                                       "label": cells[0], "cell": cell_text, "period": str(pe),
                                       "scale": scale, "currency": currency}, sort_keys=True)
                candidates[(pe, field if d == pe else field + "_seasonal_comparative")].append((value, currency, evidence))
    out["extraction_reasons"] = out["extraction_reasons"][:12]
    periods = {pe for pe, field in candidates if field in FIELDS}
    if len(periods) != 1:
        out["reason"] = "ambiguous_period" if periods else out["reason"]
        return out
    pe = next(iter(periods))
    out["period_end"] = pe
    out.update(fiscal_identity(' '.join(root.text_content().split()),pe))
    if len({currency for (period,field),values in candidates.items() if period==pe
            for _,currency,_ in values}) != 1:
        out.update(currency=None,reason="mixed_currencies_across_metrics")
        return out
    reasons = []
    for field in (*FIELDS, *(f + "_seasonal_comparative" for f in FIELDS)):
        values = candidates.get((pe, field), [])
        distinct = {(v, c) for v, c, _ in values}
        if len(distinct) == 1:
            value, currency = next(iter(distinct))
            out[field] = value
            out["currency"] = currency
            out["evidence"].extend(e for _, _, e in values)
        else:
            reasons.append(field + (":conflicting_rows" if distinct else ":not_qualified"))
    out["period_basis"] = "explicit_three_months"  # exact start date unknown
    out["eps_split_comparable"] = out.get("epsfiq_seasonal_comparative") is not None
    out["reason"] = ";".join(reasons) or "qualified"
    return out


def merge_extracts(extracts: list[dict[str, Any]]) -> dict[str, Any]:
    """Order-independent evidence union; conflicting inputs never win by position."""
    fields=(*FIELDS,*(f+'_seasonal_comparative' for f in FIELDS))
    out={f:None for f in fields}
    reasons=[]
    basis=('period_end','currency','period_basis','fiscal_year_label','fiscal_quarter',
           'fiscal_calendar_id','quarter_identity_reason','period_days','eps_split_comparable')
    for field in (*basis,*fields):
        values={r[field] for r in extracts if r.get(field) is not None}
        if len(values)==1:
            out[field]=next(iter(values))
        elif values:
            out[field]=None; reasons.append(field+':conflicting_documents')
    if any(r.startswith(('period_end:','currency:','period_basis:','fiscal_')) for r in reasons):
        for field in fields: out[field]=None
    if 'quarter_identity_reason:conflicting_documents' in reasons:
        out.update(fiscal_year_label=None,fiscal_quarter=None,fiscal_calendar_id=None)
    if any(r.get('reason')=='mixed_currencies_across_metrics' for r in extracts):
        out['currency']=None
        for field in fields: out[field]=None
        reasons.append('currency:mixed_document_metrics')
    out['evidence']=sorted({e for r in extracts for e in r.get('evidence',[])})
    out['extraction_reasons']=sorted({e for r in extracts for e in r.get('extraction_reasons',[])})[:12]
    out['reason']=';'.join(reasons) if reasons else ('qualified' if any(out[f] is not None for f in FIELDS)
                      else ';'.join(sorted({r.get('reason','not_qualified') for r in extracts})))
    return out


def qualified_clock(row: dict[str, Any]) -> dt.datetime | None:
    clock = row.get("acceptance_clock") or ""
    risk = row.get("vintage_risk") or ""
    if not clock or "unresolved" in clock or "unresolved" in risk:
        return None
    return row.get("available_at")


def sue_history(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Per issuer, retain every visible vintage. Prior eight surprises exclude current.

    Monetary comparisons require equal currencies. EPS seasonal changes require
    explicit split comparability; the generic EX-99 parser does not certify it.
    """
    history: list[dict[str, Any]] = []
    result = []
    for row in sorted(rows, key=lambda r: (r.get("available_at") or dt.datetime.max, r["accession"])):
        out = dict(row)
        pe, at = row.get("period_end"), row.get("available_at")
        for field, name in (("saleq", "sue_rev_ann"), ("epsfiq", "sue_eps_ann")):
            out[name], out[name + "_reason"] = None, "missing_value_or_clock"
            if pe is None or at is None or row.get(field) is None:
                continue
            identity=(row.get('fiscal_year_label'),row.get('fiscal_quarter'),row.get('fiscal_calendar_id'))
            if any(v is None for v in identity):
                out[name+'_reason']='fiscal_quarter_or_calendar_unqualified'; continue
            if field+'_seasonal_comparative:conflicting' in row.get('reason',''):
                out[name+'_reason']='conflicting_seasonal_comparative'; continue
            visible = [r for r in history if r["available_at"] is not None and r["available_at"] < at
                       and r.get("currency") == row.get("currency")]
            # Latest visible vintage for each period, at this row's cutoff only.
            by_period = {r["period_end"]: r for r in visible if r.get("period_end")}
            prior_year = [r for p, r in by_period.items() if 345 <= (pe-p).days <= 385 and r.get(field) is not None]
            comparative = row.get(field + "_seasonal_comparative")
            if comparative is None and len(prior_year) != 1:
                out[name + "_reason"] = "seasonal_lag_missing_or_ambiguous"
                continue
            if field == "epsfiq" and not row.get("eps_split_comparable", False):
                out[name + "_reason"] = "eps_split_basis_unqualified"
                continue
            if field == "epsfiq" and not row.get("eps_history_basis_verified", False):
                # A restated comparative in this release establishes only its
                # current seasonal pair, not the share basis of eight earlier
                # surprises. A source-backed split bridge is required.
                out[name + "_reason"] = "eight_prior_eps_share_bases_unqualified"
                continue
            surprise = row[field] - (comparative if comparative is not None else prior_year[0][field])
            out[field + "_surprise"] = surprise
            index=identity[0]*4+identity[1]-1
            quarters=defaultdict(list)
            for r in by_period.values():
                if r.get('fiscal_year_label') is not None and r.get('fiscal_quarter') is not None:
                    quarters[r['fiscal_year_label']*4+r['fiscal_quarter']-1].append(r)
            prior=[]
            for expected in range(index-8,index):
                matches=quarters.get(expected,[])
                if len(matches)!=1: break
                prior.append(matches[0])
            sequence=[*prior,row]
            if len(prior)!=8 or any(r.get(field+'_surprise') is None for r in prior):
                out[name + "_reason"] = "missing_preceding_quarter_surprise"
            elif any(r.get('fiscal_calendar_id')!=identity[2] for r in prior) or any(
                    not 75<=(b['period_end']-a['period_end']).days<=105 for a,b in zip(sequence,sequence[1:])):
                out[name+'_reason']='fiscal_calendar_transition_or_nonquarter_gap'
            else:
                scale = statistics.stdev(r[field + "_surprise"] for r in prior)
                if scale > 0 and math.isfinite(scale):
                    out[name], out[name + "_reason"] = surprise / scale, "qualified"
                else:
                    out[name + "_reason"] = "zero_or_invalid_surprise_scale"
        history.append(out)
        result.append(out)
    return result


def reaction_return(dates_: list[dt.date], returns: list[float | None], end_clock: dt.datetime | None
                    ) -> tuple[float | None, dt.datetime | None, str]:
    """Calculate only after full three-session window and strictly before holdout."""
    if len(dates_) != 3 or len(returns) != 3 or sorted(set(dates_)) != dates_:
        return None, None, "incomplete_window"
    if dates_[-1] >= HOLDOUT:
        return None, None, "holdout_window_ends_2023_or_later"
    if end_clock is None:
        return None, None, "missing_price_clock"
    if any(r is None or not math.isfinite(r) or r < -1 for r in returns):
        return None, None, "missing_or_invalid_return"
    return math.prod(1 + r for r in returns) - 1, end_clock, "qualified"


RELEASE_SCHEMA = pa.schema([
    ("cik", pa.int64()), ("accession", pa.string()), ("filing_date", pa.date32()),
    ("available_at", pa.timestamp("us")), ("source_available_at", pa.timestamp("us")),
    ("acceptance_clock", pa.string()), ("clock_reason", pa.string()), ("period_end", pa.date32()),
    ("currency", pa.string()), ("period_basis", pa.string()), ("period_days", pa.int32()),
    ("fiscal_year_label",pa.int32()),("fiscal_quarter",pa.int32()),
    ("fiscal_calendar_id",pa.string()),("quarter_identity_reason",pa.string()),
    *((f, pa.float64()) for f in (*FIELDS, *(f + "_seasonal_comparative" for f in FIELDS))),
    ("eps_split_comparable", pa.bool_()), ("reason", pa.string()), ("evidence", pa.list_(pa.string())),
    ("extraction_reasons",pa.list_(pa.string())),
    ("source_doc", pa.list_(pa.string())), ("doc_sha256", pa.list_(pa.string())),
])
FEATURE_SCHEMA = pa.schema([*RELEASE_SCHEMA,
    *((f, pa.float64()) for f in ("saleq_surprise", "epsfiq_surprise", "sue_rev_ann", "sue_eps_ann")),
    ("sue_rev_ann_reason", pa.string()), ("sue_eps_ann_reason", pa.string())])


def verify_input(stage: str, manifest_name: str = "manifest.json",required_phase: tuple[str,...] | None=None) -> dict[str, str]:
    """Fail closed on stale/partial producer publication, before reading its rows."""
    directory = C.build_root() / stage
    path = directory / manifest_name
    manifest = C.read_json(path)
    before = C.sha256_file(path)
    if manifest.get("status") != "complete" or not manifest.get("files"):
        raise RuntimeError(f"{stage}: incomplete manifest")
    if required_phase is not None and manifest.get('phase') not in required_phase:
        raise RuntimeError(f'{stage}: stale or incompatible publication phase')
    if stage==STAGE and manifest.get('phase') in ('features','validated'):
        alias=directory/'releases_manifest.json'
        if C.sha256_file(alias)!=manifest['parse_lineage']['sha256']:
            raise RuntimeError('feature generation does not match active parse generation')
    for name, record in manifest["files"].items():
        file = directory / name
        if not file.is_file() or file.stat().st_size != record["bytes"] or C.sha256_file(file) != record["sha256"]:
            raise RuntimeError(f"{stage}: source output mismatch {name}")
    if C.sha256_file(path) != before:
        raise RuntimeError(f"{stage}: producer manifest changed during verification")
    return {"path": str(path), "sha256": before}


def _write_rows(dest: Path, rows: list[dict[str, Any]], schema: pa.Schema) -> None:
    import os
    tmp = dest.with_suffix(dest.suffix + ".partial")
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), tmp, compression="zstd")
    os.replace(tmp, dest)


class ParseUnitLimit(RuntimeError):
    """Controlled partial cache pass; no stage publication is performed."""


def cached_extract(blob: bytes, row: dict[str, Any], document: dict[str, Any],
                   cache: Path, run_key: str, counters: dict[str, int],
                   limit: int | None = None) -> dict[str, Any]:
    """Content/code/input keyed complete units survive a guard stop safely."""
    identity = {"run_key":run_key,"cik":row['cik'],"accession":row['accession'],
                "filing_date":str(row['filing_date']),"available_at":str(row.get('source_available_at')),
                "acceptance_clock":row.get('acceptance_clock'),"source_doc":document['source_doc'],
                "doc_sha256":document['doc_sha256']}
    key = hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
    path = cache / key[:2] / (key+'.json')
    if path.is_file():
        try:
            unit = C.read_json(path)
            if unit.get('status')=='complete' and unit.get('identity')==identity:
                result = unit['result']
                if result.get('period_end') is not None:
                    result['period_end'] = dt.date.fromisoformat(result['period_end'])
                counters['cache_hits'] += 1
                return result
        except (ValueError,KeyError,TypeError):
            counters['corrupt_units'] += 1
    if limit is not None and counters['new_units'] >= limit:
        raise ParseUnitLimit("bounded parse cache pass completed")
    result = parse_release(blob,row['filing_date'])
    C.write_json_atomic(path,{'status':'complete','identity':identity,'result':result})
    counters['new_units'] += 1
    return result


def parse_stage(limit_new_documents: int | None = None) -> dict[str, Any]:
    """Read cached documents only. One missing-aware row per original 2.02 accession."""
    inputs = {"sec_filings": verify_input("sec_filings")}
    catalog = ES.catalog_path()
    catalog_sha = C.sha256_file(catalog)
    code = C.code_identity("ea_actuals","ea_publication","events_sources","common")
    run_key = hashlib.sha256(json.dumps({'code':{k:v['sha256_lf'] for k,v in code.items() if isinstance(v,dict)},
        'sec_filings':inputs['sec_filings']['sha256'],'catalog':catalog_sha,'version':VERSION},sort_keys=True).encode()).hexdigest()
    cache = C.build_root() / '_tmp' / 'ea_actuals' / 'parse_units' / run_key
    cache_counts = defaultdict(int)
    con = C.connect(memory="250MB", threads=1)
    root = C.build_root()
    filings = (root / "sec_filings" / "filings.parquet").as_posix()
    items = (root / "sec_filings" / "eight_k_items.parquet").as_posix()
    base = con.execute(f"""SELECT f.cik, f.accession, f.filing_date, f.available_at,
                   f.acceptance_clock, f.vintage_risk
        FROM read_parquet('{filings}') f WHERE NOT f.is_amendment AND f.form='8-K'
          AND f.filing_date >= DATE '2015-01-01'
          AND EXISTS (SELECT 1 FROM read_parquet('{items}') i
                      WHERE i.cik=f.cik AND i.accession=f.accession AND i.item='2.02')
        ORDER BY f.cik, f.available_at, f.accession""").fetchall()
    columns = [d[0] for d in con.description]
    documents: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
    for row in ES.iter_catalog():
        if row["form"] == "8-K" and not row["is_amendment"]:
            documents[(row["cik"], row["accession"])].append(row)
    store = ES.open_store()
    output = []
    counts: dict[str, int] = defaultdict(int)
    for values in base:
        row = dict(zip(columns, values))
        row["source_available_at"] = row["available_at"]
        row["available_at"] = qualified_clock(row)
        row["clock_reason"] = "qualified" if row["available_at"] is not None else "acceptance_clock_missing_or_unresolved"
        extracts = []
        names, hashes = [], []
        for document in documents.get((row["cik"], row["accession"]), []):
            # Duplicate cached copies are evidence once, never conflict votes.
            sha = document["doc_sha256"]
            if sha in hashes:
                continue
            blob = ES.read_document(document, store)
            if blob is None or hashlib.sha256(blob).hexdigest() != sha:
                counts["missing_or_corrupt_object"] += 1
                continue
            names.append(document["source_doc"])
            hashes.append(sha)
            try:
                extracts.append(cached_extract(blob,row,document,cache,run_key,cache_counts,limit_new_documents))
            except ParseUnitLimit:
                con.close()
                if verify_input('sec_filings')['sha256']!=inputs['sec_filings']['sha256'] or C.sha256_file(catalog)!=catalog_sha:
                    raise RuntimeError('source snapshot changed during bounded parsing; no checkpoint receipt admitted')
                receipt = {'status':'partial_cache_only','run_key':run_key,'cache_counts':dict(cache_counts),
                           'published':False,'input_manifests_sha256':inputs,'catalog_sha256':catalog_sha}
                C.write_json_atomic(cache/'progress.json',receipt)
                return receipt
        qualified = [r for r in extracts if any(r.get(f) is not None for f in FIELDS)]
        if qualified:
            row.update(merge_extracts(extracts))
        else:
            row.update(merge_extracts(extracts) if extracts else {'reason':'no_cached_document'})
            row["extraction_reasons"] = [reason for r in extracts for reason in r.get("extraction_reasons",[])][:12]
            periods = {r["period_end"] for r in extracts if r.get("period_end") is not None}
            if len(periods) == 1:
                row.update(period_end=next(iter(periods)), period_basis="source_explicit_end_date")
            elif periods:
                row.update(period_basis="conflicting_document_periods")
        pairs=sorted(zip(names,hashes))
        row.update(source_doc=[p[0] for p in pairs], doc_sha256=[p[1] for p in pairs])
        counts[row["reason"]] += 1
        output.append(row)
    con.close()
    if C.sha256_file(catalog) != catalog_sha:
        raise RuntimeError("catalog changed during parsing")
    dest = P.create(STAGE)
    _write_rows(dest / "releases.parquet", output, RELEASE_SCHEMA)
    # Freeze independently of parser yield, periods, values or later comparators.
    sample = sorted((r for r in output if 2020 <= r["filing_date"].year <= 2025),
                    key=lambda r: hashlib.sha256((str(r['cik'])+'/'+r['accession']+'/t9-precision-200-v1').encode()).hexdigest())[:200]
    _write_rows(dest / "precision_sample.parquet", sample, RELEASE_SCHEMA)
    receipt = {"rows": len(output), "qualified_any": sum(any(r.get(f) is not None for f in FIELDS) for r in output),
               "qualified_all": sum(all(r.get(f) is not None for f in FIELDS) for r in output),
               "reasons": dict(counts), "catalog_sha256": catalog_sha,"parse_units":dict(cache_counts),"run_key":run_key,
               "precision_sample": {"rows": len(sample), "selection": "lowest SHA256(cik/accession/t9-precision-200-v1), all original 2.02 releases 2020-2025, independent of extraction success",
                                    "key_sha256": hashlib.sha256('\n'.join(str(r['cik'])+'/'+r['accession'] for r in sample).encode()).hexdigest(),
                                    "manual_reviewed": 0}}
    # A separate stable parse publication prevents the calendar -> features
    # pipeline from creating a manifest dependency cycle within this stage.
    payload={
        "schema": SCHEMA, "status": "complete", "stage": STAGE,
        "code": C.code_identity("ea_actuals", "ea_publication", "events_sources", "common"),
        "parser_version": VERSION, "phase": "parsed", "receipt": receipt,
        "input_manifests_sha256": inputs, "gate_status": {"precision": "FAIL_unreviewed", "recall": "FAIL_unmeasured"}}
    P.publish(STAGE,dest,'parsed',payload,('releases.parquet','precision_sample.parquet'))
    return receipt


def build_features() -> dict[str, Any]:
    root = C.build_root()
    inputs = {s: verify_input(s) for s in ("earnings_calendar_v2", "sec_filings", "panel", "prices")}
    inputs['calendar.parquet']={'path':str(C.calendar_path()),'sha256':C.sha256_file(C.calendar_path())}
    parse_identity=verify_input(STAGE,"releases_manifest.json",('parsed',))
    prior=C.read_json(root/STAGE/'releases_manifest.json')
    active=C.read_json(root/STAGE/'manifest.json')
    if active['phase']=='parsed' and active['generation']!=prior['generation']:
        raise RuntimeError('parse publication activation incomplete')
    if prior['input_manifests_sha256']['sec_filings']['sha256']!=inputs['sec_filings']['sha256']:
        raise RuntimeError('parse and feature SEC filing snapshots incompatible; reparse required')
    calendar=C.read_json(root/'earnings_calendar_v2'/'manifest.json')
    if calendar['input_manifests_sha256']['ea_actuals/releases_manifest.json']['sha256']!=parse_identity['sha256']:
        raise RuntimeError('calendar and parse generations incompatible')
    parse_identity={'path':str(root/STAGE/prior['generation']/'manifest.json'),'sha256':parse_identity['sha256']}
    inputs['ea_actuals/parse_generation']=parse_identity
    directory=P.create(STAGE)
    release_path=P.file(STAGE,'releases.parquet','releases_manifest.json').as_posix()
    releases = pq.read_table(release_path).to_pylist()
    groups: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in releases:
        groups[row["cik"]].append(row)
    rows = [r for cik in sorted(groups) for r in sue_history(groups[cik])]
    _write_rows(directory / "ann_features.parquet", rows, FEATURE_SCHEMA)
    con = C.connect(memory="250MB", threads=1)
    # lag to the later original 10-Q is a separately clocked row, not backfilled
    # into announcement features using future filing metadata.
    lag_rows = C.copy_to_parquet(con, f"""WITH candidates AS (
        SELECT r.cik, r.accession, r.period_end, r.available_at AS announcement_available_at,
               f.accession AS filing_accession, f.available_at,
               date_diff('day', timezone('America/New_York',timezone('UTC',r.available_at))::DATE,
                         timezone('America/New_York',timezone('UTC',f.available_at))::DATE) AS ea_lag_days,
               row_number() OVER(PARTITION BY r.cik,r.accession ORDER BY f.available_at,f.accession) rn
        FROM read_parquet('{release_path}') r JOIN
             read_parquet('{(root/'sec_filings'/'filings.parquet').as_posix()}') f
             ON r.cik=f.cik AND r.period_end=f.report_date AND f.form IN ('10-Q','10-QT')
                AND NOT f.is_amendment AND f.available_at>=r.available_at
        WHERE r.available_at IS NOT NULL AND f.acceptance_clock NOT LIKE '%unresolved%'
              AND coalesce(f.vintage_risk,'') NOT LIKE '%unresolved%')
        SELECT * EXCLUDE(rn) FROM candidates WHERE rn=1""", directory / "filing_lags.parquet")
    feature_path = (directory / "ann_features.parquet").as_posix()
    recall = con.execute(f"""SELECT year(filing_date),count(*),count(*) FILTER(WHERE saleq IS NOT NULL
             AND epsfiq IS NOT NULL AND niq IS NOT NULL),count(*) FILTER(WHERE available_at IS NULL)
        FROM read_parquet('{feature_path}') WHERE filing_date BETWEEN DATE '2020-01-01' AND DATE '2025-12-31'
        GROUP BY 1 ORDER BY 1""").fetchall()
    receipt = {"rows": len(rows), "filing_lag_rows": lag_rows,
               "recall": {str(y): {"releases": n, "all_actuals": a, "all_actuals_share": a/n,
                                    "unknown_clocks": u} for y,n,a,u in recall}}
    try:
        receipt["reaction_returns"] = build_reactions(con, directory,release_path)
    except BaseException:
        con.close()
        raise
    con.close()
    for stage in ('earnings_calendar_v2','sec_filings','panel','prices'):
        if verify_input(stage)['sha256']!=inputs[stage]['sha256']:
            raise RuntimeError(f'{stage}: changed during feature generation')
    if verify_input(STAGE,'releases_manifest.json',('parsed',))['sha256']!=parse_identity['sha256']:
        raise RuntimeError('parse generation changed during feature build')
    if C.sha256_file(C.calendar_path())!=inputs['calendar.parquet']['sha256']:
        raise RuntimeError('calendar source changed during feature generation')
    P.publish(STAGE,directory,'features',{
        'schema':SCHEMA,'stage':STAGE,'code':C.code_identity('ea_actuals','ea_publication','events_sources','common'),
        'parse_generation':prior['generation'],'parse_lineage':parse_identity,
        'parse_input_manifests_sha256':prior['input_manifests_sha256'],
        "parser_version": VERSION, "phase": "features", "parse_receipt": prior.get("receipt"),
        "receipt": receipt, "input_manifests_sha256": inputs,
        "gate_status": {"precision": "FAIL_unreviewed", "recall": "PASS" if recall and all(a/n>=.85 for _,n,a,_ in recall) else "FAIL",
                        "sue_member_cells": "FAIL_unmeasured", "reaction_returns": "PASS_boundary_enforced"},
        "clock_rule": "announcement actuals/SUE at resolved acceptance; lag at 10-Q acceptance; reaction return at third reaction-session close",
        "missingness": "unknown clocks, currencies, periods, scale, GAAP basis and conflicting cells remain NULL with reason"},
        ('ann_features.parquet','filing_lags.parquet','reaction_returns.parquet'))
    return receipt


def build_reactions(con: Any, directory: Path,release_path: str) -> dict[str, Any]:
    """Three reaction-session total returns, source dates bounded before arithmetic.

    Later reactions are NULL with a holdout reason. Features carry the complete
    window's conservative 22:00 UTC clock. Panel identity is used only at reaction
    session; a release cannot be mapped to a security using a future CIK snapshot.
    """
    root = C.build_root()
    ann = P.file('earnings_calendar_v2','announcements.parquet').as_posix()
    actual = release_path
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    prices = (root / "prices" / "year=*" / "*.parquet").as_posix()
    cal = C.calendar_path().as_posix()
    con.execute(f"""CREATE OR REPLACE TEMP TABLE reaction_keys AS
        SELECT DISTINCT a.cik,a.accession,a.reaction_session,r.available_at,p.security_id
        FROM read_parquet('{ann}') a JOIN read_parquet('{actual}') r USING(cik,accession)
        LEFT JOIN read_parquet('{panel}',hive_partitioning=false) p
          ON p.cik=a.cik AND p.session_date=a.reaction_session
        WHERE r.available_at IS NOT NULL""")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE reaction_calendar AS
        SELECT session_date,row_number() OVER(ORDER BY session_date) idx
        FROM read_parquet('{cal}') WHERE session_date<DATE '{HOLDOUT}'""")
    # Later-year source paths aren't even selected. Filtering individual rows is
    # still required for partial years; no adjusted-price chain anchored in 2026.
    paths = [p.as_posix() for p in sorted((root / "prices").glob("year=*/*.parquet"))
             if int(p.parent.name.split('=')[1]) < 2023]
    if not paths:
        raise RuntimeError("pre-2023 prices unavailable; reaction calculation refused")
    price_list = ','.join("'"+p+"'" for p in paths)
    con.execute(f"""CREATE OR REPLACE TEMP TABLE reaction_prices AS
       SELECT session_date,security_id,ret,ret_guarded FROM read_parquet([{price_list}],hive_partitioning=false)
       WHERE session_date<DATE '{HOLDOUT}'""")
    dest = directory / "reaction_returns.parquet"
    n = C.copy_to_parquet(con, f"""WITH windows AS (
        SELECT k.*,e.session_date AS window_end,s.session_date AS price_date,p.ret,p.ret_guarded
        FROM reaction_keys k LEFT JOIN reaction_calendar b ON k.reaction_session=b.session_date
        LEFT JOIN reaction_calendar e ON e.idx=b.idx+2
        LEFT JOIN reaction_calendar s ON s.idx BETWEEN b.idx AND b.idx+2 AND e.session_date IS NOT NULL
        LEFT JOIN reaction_prices p ON p.session_date=s.session_date AND p.security_id=k.security_id
        WHERE k.reaction_session<DATE '{HOLDOUT}'
    ), grouped AS (
        SELECT cik,accession,security_id,reaction_session,window_end,max(available_at) announcement_available_at,
               count(price_date) n_sessions,
               count(ret) FILTER(WHERE isfinite(ret) AND ret>=-1 AND NOT coalesce(ret_guarded,true)) n_valid,
               count(*) FILTER(WHERE ret=-1) terminal,
               sum(CASE WHEN ret>-1 AND isfinite(ret) AND NOT coalesce(ret_guarded,true) THEN ln(1+ret) END) logs
        FROM windows GROUP BY 1,2,3,4,5)
        SELECT cik,accession,security_id,reaction_session,window_end,announcement_available_at,
               CASE WHEN n_sessions=3 AND n_valid=3 THEN CASE WHEN terminal>0 THEN -1.0 ELSE exp(logs)-1 END END ea_ret_3d,
               CASE WHEN n_sessions=3 AND n_valid=3 THEN greatest(announcement_available_at,
                         CAST(window_end AS TIMESTAMP)+INTERVAL 22 HOUR) END available_at,
               CASE WHEN security_id IS NULL THEN 'unmapped_security_at_reaction'
                    WHEN window_end IS NULL THEN 'holdout_or_incomplete_window'
                    WHEN n_sessions<>3 OR n_valid<>3 THEN 'missing_or_guarded_price_return'
                    ELSE 'qualified' END reason
        FROM grouped
        UNION ALL
        SELECT cik,accession,security_id,reaction_session,NULL,available_at,NULL,NULL,
               CASE WHEN reaction_session>=DATE '{HOLDOUT}' THEN 'holdout_window_ends_2023_or_later'
                    ELSE 'missing_reaction_session' END
        FROM reaction_keys WHERE reaction_session>=DATE '{HOLDOUT}' OR reaction_session IS NULL""", dest)
    stats = dict(con.execute(f"SELECT reason,count(*) FROM read_parquet('{dest.as_posix()}') GROUP BY 1").fetchall())
    violations = con.execute(f"SELECT count(*) FROM read_parquet('{dest.as_posix()}') WHERE ea_ret_3d IS NOT NULL AND (window_end>=DATE '{HOLDOUT}' OR available_at<window_end::TIMESTAMP+INTERVAL 22 HOUR)").fetchone()[0]
    if violations:
        raise RuntimeError("reaction return window/clock violation")
    return {"rows": n, "reasons": stats, "boundary_or_clock_violations": violations,
            "price_source_dates": "strictly_before_2023", "price_clock": "conservative_session_22UTC"}


def audit_stage() -> dict[str, Any]:
    """Final-producer-only GC3 coverage and Task3 rdq refresh. No return reads."""
    from . import compustat_std as CS
    root, directory = C.build_root(), C.stage_dir(STAGE)
    verify_input(STAGE,required_phase=('features','validated'))
    stages = (STAGE, "earnings_calendar_v2", "compustat_std", "universe", "panel")
    inputs = {s: verify_input(s) for s in stages}
    std = C.read_json(root / "compustat_std" / "manifest.json")
    if std.get("fund_stage") != "fundamentals_v11":
        raise RuntimeError("final fundamentals_v11 compustat publication required; stale v10 cannot certify Task3 rdq")
    inputs["fundamentals_v11"] = verify_input("fundamentals_v11")
    con = C.connect(memory="250MB", threads=1)
    CS.member_ciks(con)
    CS.coverage_gate(con, root / "compustat_std")  # preserve Task3 denominator
    ann = P.file('earnings_calendar_v2','announcements.parquet').as_posix()
    con.execute(f"""CREATE OR REPLACE TEMP TABLE t9_ann AS SELECT * FROM read_parquet('{ann}')
             WHERE source='8k_202' AND is_primary AND announcement_utc IS NOT NULL AND available_at IS NOT NULL""")
    fund = (root / "fundamentals_v11" / "events.parquet").as_posix()
    con.execute(f"""CREATE OR REPLACE TEMP TABLE t9_clocks AS SELECT DISTINCT cik,accession,clock_basis
             FROM read_parquet('{fund}')""")
    rdq = con.execute("""WITH j AS (
        SELECT c.*, EXISTS(SELECT 1 FROM t9_ann a WHERE a.cik=c.cik
             AND abs(date_diff('day',a.fiscal_period_end,c.datadate))<=7
             AND timezone('America/New_York',timezone('UTC',a.available_at))::DATE <=
                 CASE WHEN c.clock_utc IS NOT NULL AND fc.clock_basis='fsds_accepted_utc'
                      THEN timezone('America/New_York',timezone('UTC',c.clock_utc))::DATE ELSE c.filed END) same_et_date,
             EXISTS(SELECT 1 FROM t9_ann a WHERE a.cik=c.cik
             AND abs(date_diff('day',a.fiscal_period_end,c.datadate))<=7 AND a.available_at<=c.available_at) strict_timestamp
        FROM cov c LEFT JOIN t9_clocks fc USING(cik,accession) WHERE c.form IN ('10-Q','10-QT'))
        SELECT year,count(*),count(*) FILTER(WHERE same_et_date),count(*) FILTER(WHERE strict_timestamp)
        FROM j GROUP BY 1 ORDER BY 1""").fetchall()
    coverage = {}
    features = P.file(STAGE,'ann_features.parquet').as_posix()
    calendar = C.calendar_path().as_posix()
    con.execute(f"""CREATE OR REPLACE TEMP TABLE t9_cutoffs AS SELECT session_date,
       lag(session_date) OVER(ORDER BY session_date)::TIMESTAMP+INTERVAL 22 HOUR cutoff
       FROM read_parquet('{calendar}')""")
    for stage, flags in (("panel", ("member_equity",)), ("universe", ("univ_all_common", "univ_r3000p"))):
        for y in range(2020, 2026):
            files = sorted((root / stage / f"year={y}").glob("*.parquet"))
            if not files:
                raise RuntimeError(f"{stage}: missing final year {y}")
            paths = ','.join("'"+p.as_posix()+"'" for p in files)
            for flag in flags:
                rec = con.execute(f"""WITH m AS (
                   SELECT p.session_date,p.security_id,p.cik,c.cutoff FROM read_parquet([{paths}],hive_partitioning=false) p
                   JOIN t9_cutoffs c USING(session_date) WHERE {flag}
                ), j AS (SELECT m.*,f.period_end,f.sue_eps_ann,f.sue_rev_ann FROM m ASOF LEFT JOIN
                   read_parquet('{features}') f ON m.cik=f.cik AND m.cutoff>f.available_at)
                SELECT count(*),count(*) FILTER(WHERE sue_eps_ann IS NOT NULL AND period_end<=session_date
                        AND date_diff('day',period_end,session_date)<=400),
                   count(*) FILTER(WHERE sue_rev_ann IS NOT NULL AND period_end<=session_date
                        AND date_diff('day',period_end,session_date)<=400)
                FROM j""").fetchone()
                issuers = con.execute(f"""WITH m AS (SELECT DISTINCT cik FROM read_parquet([{paths}],hive_partitioning=false)
                           WHERE {flag} AND cik IS NOT NULL)
                       SELECT count(*),count(*) FILTER(WHERE EXISTS(SELECT 1 FROM read_parquet('{ann}') a
                                  WHERE a.cik=m.cik AND year(a.session_date)={y})) FROM m""").fetchone()
                n,e,r = rec
                coverage[f"{flag}/{y}"] = {"cells": n,"eps_sue_cells":e,"revenue_sue_cells":r,
                    "eps_sue_share":e/n if n else None,"revenue_sue_share":r/n if n else None,
                    "member_issuers":issuers[0],"calendar_covered_issuers":issuers[1],
                    "calendar_covered_share":issuers[1]/issuers[0] if issuers[0] else None}
    con.close()
    # Verify again after measurements; a producer must remain quiescent.
    for stage in stages:
        if verify_input(stage)["sha256"] != inputs[stage]["sha256"]:
            raise RuntimeError(f"{stage}: changed during final audit")
    result = {"input_integrity":inputs,"coverage":coverage,
       "rdq":{str(y):{"periods":n,"same_et_date":a,"same_et_date_share":a/n if n else None,
                         "strict_timestamp":b,"strict_timestamp_share":b/n if n else None} for y,n,a,b in rdq},
       "rdq_rule":"Task3 first-reported member 10-Q/10-QT denominator; only source-backed primary 8-K 2.02; acceptance <= filing ET date, strict timestamp alongside",
       "gate_status":{"calendar_gc3":"PASS" if coverage and all(r['calendar_covered_share'] is not None and r['calendar_covered_share']>=.95 for r in coverage.values()) else "FAIL",
                      "sue_cells":"PASS" if coverage and all(r['eps_sue_share'] is not None and r['eps_sue_share']>=.9 for r in coverage.values()) else "FAIL",
                      "rdq_same_date":"PASS" if rdq and all(n and a/n>=.95 for _,n,a,b in rdq) else "FAIL",
                      "precision_200_manual":"FAIL_unreviewed"}}
    work=P.create(STAGE)
    import shutil
    for name in ('ann_features.parquet','filing_lags.parquet','reaction_returns.parquet'):
        shutil.copy2(P.file(STAGE,name),work/name)
    C.write_json_atomic(work / "validation.json", result)
    manifest = C.read_json(directory / "manifest.json")
    manifest.update(validation=result,audit_input_manifests_sha256=inputs,
                    audit_code=C.code_identity('ea_actuals','ea_publication','common'))
    manifest['gate_status'].update(result['gate_status'])
    P.publish(STAGE,work,'validated',manifest,('ann_features.parquet','filing_lags.parquet','reaction_returns.parquet','validation.json'))
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--phase", choices=("parse", "build", "audit"), required=True)
    ap.add_argument("--limit-new-documents",type=int,default=None,help="bounded resumable cache pass; no incomplete stage publication")
    args = ap.parse_args(argv)
    command = {"parse": parse_stage, "build": build_features, "audit": audit_stage}[args.phase]
    if args.limit_new_documents is not None and (args.limit_new_documents<0 or args.phase!='parse'):
        ap.error("--limit-new-documents requires parse and a nonnegative limit")
    print(json.dumps(command(args.limit_new_documents) if args.phase=='parse' else command(), default=str), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
