"""D3 sealed filing clocks and true entity-share events; no warehouse/network IO.

Acceptance is not public dissemination. Only separately evidenced publication
plus qualified fact vintage earns observed-public admission. The explicit
modeled policy uses a declared acceptance lag or filed-midnight UTC +46h.
CompanyFacts is entity-level: duplicate DEI rows are never summed as classes.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

UTC = dt.timezone.utc
SEAL = dt.datetime(2020, 1, 1, tzinfo=UTC)
NS = 10**9
CLOCK_KEYS = ("filed_ns", "clock_kind", "accepted_ns", "published_ns",
              "revision_available_ns", "fact_vintage_qualified", "knowledge_clock_qualified",
              "clock_policy")
DEI = "EntityCommonStockSharesOutstanding"
ACTIVE_LOOKBACK_DAYS = 8 * 366  # explicit V4 fiscal-history bound, also bounds qualification


def ns(value: dt.datetime | dt.date) -> int:
    if not isinstance(value, dt.datetime):
        value = dt.datetime.combine(value, dt.time(), UTC)
    if value.tzinfo is None or value.utcoffset() is None:
        raise ValueError("naive timestamp cannot establish a filing clock")
    delta = value.astimezone(UTC) - dt.datetime(1970, 1, 1, tzinfo=UTC)
    return (delta.days * 86400 + delta.seconds) * NS + delta.microseconds * 1000


def stamp(value: str | None) -> int:
    if not value:
        return 0
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    number = ns(parsed)
    if number <= 0 or number >= ns(SEAL):
        raise ValueError("filing evidence timestamp outside the pre-2020 seal")
    return number


def cik_key(value: str | int) -> str:
    value = str(value)
    if not re.fullmatch(r"[0-9]{1,10}", value) or int(value) == 0:
        raise ValueError("invalid issuer CIK")
    return value.zfill(10)


def accession_key(value: str) -> str:
    if not re.fullmatch(r"[0-9]{10}-[0-9]{2}-[0-9]{6}", value):
        raise ValueError("invalid accession")
    return value


def valid_hash(value: str) -> bool:
    return bool(re.fullmatch(r"[0-9a-f]{64}", value))


@dataclass(frozen=True)
class FilingEvidence:
    cik: str
    accession: str
    filed: dt.date
    accepted_ns: int
    published_ns: int
    revision_ns: int
    vintage_qualified: bool
    source_sha256: str
    source_locator: str
    accepted_raw: str
    revision_status: str

    @classmethod
    def parse(cls, row: dict) -> "FilingEvidence":
        filed = dt.date.fromisoformat(row["filed"])
        if ns(filed) <= 0 or filed >= SEAL.date():
            raise ValueError("filing date reaches sealed era")
        accepted, published, revision = (stamp(row.get(k)) for k in
            ("accepted_at", "published_at", "revision_available_at"))
        status = row.get("revision_status", "unverified")
        if status not in {"original-confirmed", "revision-confirmed", "unverified"}:
            raise ValueError("unknown fact vintage status")
        if ((published and accepted and published < accepted) or
                (revision and accepted and revision < accepted) or
                (revision and published and revision < published)):
            raise ValueError("publication/revision precedes observed acceptance")
        if status == "revision-confirmed" and not revision:
            raise ValueError("confirmed revision lacks its availability clock")
        if status == "original-confirmed" and revision:
            raise ValueError("original vintage contradicts revision availability")
        source_hash, locator = row.get("source_sha256", ""), row.get("source_locator", "")
        if not valid_hash(source_hash) or not locator or len(locator) > 2048:
            raise ValueError("filing clock requires source hash/locator")
        return cls(cik_key(row["cik"]), accession_key(row["accession"]), filed,
            accepted, published, revision, status != "unverified", source_hash, locator,
            str(row.get("accepted_raw", "")), status)


def load_projection(payload: Path, manifest_path: Path, seal: dt.date) -> tuple[dict, dict]:
    """Read only a separately sealed, hash-bound clock projection, never a DB."""
    if payload.stat().st_size > 64 * 1024 * 1024 or manifest_path.stat().st_size > 1024 * 1024:
        raise ValueError("filing clock projection exceeds bounded input size")
    proof = json.loads(manifest_path.read_text(encoding="utf-8"))
    raw = payload.read_bytes()
    if (proof.get("schema") != "atx.sec-filing-clocks-sealed/v1" or
            dt.date.fromisoformat(proof["seal_exclusive"]) > seal or
            hashlib.sha256(raw).hexdigest() != proof.get("payload_sha256") or
            not valid_hash(proof.get("facts_payload_sha256", ""))):
        raise ValueError("unqualified sealed filing-clock projection")
    body = json.loads(raw)
    if body.get("schema") != "atx.sec-filing-clocks/v1" or not isinstance(body.get("records"), list):
        raise ValueError("invalid filing-clock payload schema")
    if len(body["records"]) > 250_000:
        raise ValueError("too many filing-clock records")
    output = {}
    for row in body["records"]:
        item = FilingEvidence.parse(row)
        if item.filed >= seal or any(v >= ns(seal) for v in
                (item.accepted_ns, item.published_ns, item.revision_ns)):
            raise ValueError("projection row exceeds declared seal")
        key = (item.cik, item.accession)
        if key in output and output[key] != item:
            raise ValueError("conflicting issuer/accession filing clocks")
        output[key] = item
    return output, proof


def resolve_clock(cik: str, accession: str, filed: dt.date, evidence: dict,
                  acceptance_delay_seconds: int) -> dict:
    if not isinstance(acceptance_delay_seconds, int) or not 0 <= acceptance_delay_seconds <= 7 * 86400:
        raise ValueError("acceptance dissemination delay must be an explicit bounded integer")
    item = evidence.get((cik_key(cik), accession_key(accession)))
    if item is not None and item.filed != filed:
        raise ValueError("fact and accession evidence disagree on filed date")
    accepted = item.accepted_ns if item else 0
    published = item.published_ns if item else 0
    revision = item.revision_ns if item else 0
    if accepted and published:
        kind, policy, base = 1, "accepted-public-v2", max(accepted, published)
    elif accepted:
        kind, policy = 2, f"acceptance-plus{acceptance_delay_seconds}s-modeled-v2"
        base = accepted + acceptance_delay_seconds * NS
    else:
        kind, policy, base = 3, "filed-plus46h-modeled-v2", ns(filed) + 46 * 3600 * NS
    return dict(filed_ns=ns(filed), clock_kind=kind, accepted_ns=accepted,
        published_ns=published, revision_available_ns=revision,
        fact_vintage_qualified=int(bool(item and item.vintage_qualified)),
        knowledge_clock_qualified=int(kind == 1), clock_policy=policy,
        available_ns=max(base, published, revision),
        clock_source_sha256=item.source_sha256 if item else "",
        clock_source_locator=item.source_locator if item else "",
        accepted_raw=item.accepted_raw if item else "",
        revision_status=item.revision_status if item else "unverified")


@dataclass(frozen=True)
class Fact:
    concept: str
    start: dt.date | None
    end: dt.date
    val: float
    filed: dt.date
    form: str
    accn: str
    taxonomy: str
    unit: str


def parse_facts(doc: dict, accounting_concepts: set[str], event_forms: set[str]) -> tuple[list[Fact], list[dict]]:
    """Strict new-route parse; exact duplicates collapse, conflicting values remain for audit."""
    rows, seen, rejected = [], set(), []
    for taxonomy, concepts in doc.get("facts", {}).items():
        for concept, body in concepts.items():
            if not ((taxonomy == "dei" and concept == DEI) or
                    (taxonomy == "us-gaap" and concept in accounting_concepts)):
                continue
            expected_unit = "shares" if "Shares" in concept else ("USD/shares" if "PerShare" in concept else "USD")
            for unit, values in body.get("units", {}).items():
                for value in values:
                    try:
                        filed = dt.date.fromisoformat(value["filed"])
                        end = dt.date.fromisoformat(value["end"])
                        start = dt.date.fromisoformat(value["start"]) if value.get("start") else None
                        accn = accession_key(value.get("accn", "")); number = float(value["val"])
                        form = value.get("form", "")
                        if unit != expected_unit or not math.isfinite(number) or ns(filed) <= 0 or ns(end) <= 0 or end > filed or (start and (ns(start) <= 0 or start > end)):
                            raise ValueError("unit/value/period")
                        if concept == DEI and (start is not None or number <= 0):
                            raise ValueError("DEI must be a positive instant shares fact")
                        if concept != DEI and form not in event_forms:
                            continue
                    except (KeyError, TypeError, ValueError, OverflowError) as exc:
                        rejected.append({"concept": concept, "accession": value.get("accn", ""), "reason": str(exc)})
                        continue
                    row = Fact(concept, start, end, number, filed, form, accn, taxonomy, unit)
                    if row not in seen:
                        rows.append(row); seen.add(row)
    rows.sort(key=lambda f: (f.filed, f.accn, f.concept, f.end, f.start or dt.date.min, f.val))
    return rows, rejected


def snapshot_events(facts: list[Fact], cik: str, clocks: dict, seal: dt.date, delay_seconds: int,
                    *, knowledge_factory: Callable, snapshot_fn: Callable,
                    event_forms: set[str]) -> tuple[list[dict], list[dict], list[dict]]:
    """Resolve all accession clocks before applying chronologically grouped knowledge.

    Qualification conservatively covers all ACTIVE retained concept/period cells,
    not only exact arithmetic dependencies. This can understate qualified coverage;
    an observed current event never launders older modeled/unverified inputs.
    """
    groups = {}
    for fact in facts:
        if fact.filed >= seal:
            continue
        groups.setdefault((fact.accn, fact.filed), []).append(fact)
    events, audit = [], []
    for (accn, filed), group in sorted(groups.items()):
        clock = resolve_clock(cik, accn, filed, clocks, delay_seconds)
        if clock["available_ns"] >= ns(seal):
            continue
        if any(ns(f.end) > clock["available_ns"] for f in group):
            raise ValueError("fact period follows resolved availability")
        events.append((clock["available_ns"], accn, filed, clock, group))
    events.sort(key=lambda row: (row[0], row[1]))
    knowledge, qualifiers, snapshots, shares = knowledge_factory(), {}, [], []
    index = 0
    while index < len(events):
        stop = index + 1
        while stop < len(events) and events[stop][0] == events[index][0]:
            stop += 1
        same_clock = events[index:stop]
        event_facts, possible_clocks, accounting = [], [], {}
        for _, accn, filed, clock, group in same_clock:
            audit.append(dict(cik=cik, accession=accn, filed=filed.isoformat(), **clock))
            if any(f.concept != DEI for f in group):
                possible_clocks.append(clock)
            share_values = {}
            for fact in group:
                if fact.concept == DEI:
                    share_values.setdefault(fact.end, set()).add(fact.val)
                else:
                    accounting.setdefault((fact.concept, fact.start, fact.end), []).append((fact, clock))
            for end, values in sorted(share_values.items()):
                qualified = bool(clock["fact_vintage_qualified"]) and len(values) == 1
                reason = ("conflicting_entity_share_values" if len(values) != 1 else
                          "unqualified_fact_vintage" if not qualified else "")
                shares.append(dict(cik=cik, accession=accn, filed=filed.isoformat(),
                    shares_observation_date=end.isoformat(),
                    true_entity_shares=next(iter(values)) if qualified else None,
                    share_scope="entity;not-a-security-class", split_basis="observation-date-unrebased",
                    reason=reason, observed_public_admissible=qualified and clock["clock_kind"] == 1,
                    **clock))
        apply = []
        for key, entries in accounting.items():
            values = {row.val for row, _ in entries}
            chosen = entries[0][0]
            if len(values) != 1:
                # No lexical accession winner at an indistinguishable knowledge clock.
                chosen = Fact(chosen.concept, chosen.start, chosen.end, math.nan,
                    chosen.filed, chosen.form, chosen.accn, chosen.taxonomy, chosen.unit)
                audit.append(dict(cik=cik, accessions=sorted({f.accn for f, _ in entries}),
                    concept=key[0], reason="conflicting_simultaneous_fact"))
            apply.append(chosen)
            qualifiers[key] = (len(values) == 1 and all(c["fact_vintage_qualified"] for _, c in entries),
                               all(c["clock_kind"] == 1 for _, c in entries))
        knowledge.apply(apply)
        event_facts.extend(f for f in apply if f.form in event_forms)
        if qualifiers:
            cutoff = max(key[2] for key in qualifiers) - dt.timedelta(days=ACTIVE_LOOKBACK_DAYS)
            expired = [key for key in qualifiers if key[2] < cutoff]
            for concept, start, end in expired:
                del qualifiers[(concept, start, end)]
                if start is None:
                    knowledge.instants.get(concept, {}).pop(end, None)
                else:
                    knowledge.durations.get(concept, {}).pop((start, end), None)
        if event_facts:
            # Preserve NaN tombstones and their qualification in the authoritative
            # state, but do not feed them to legacy statistics.stdev (which can
            # raise on NaN). The finite calculation view cannot grant admission:
            # all active tombstones still make the row vintage-unqualified.
            finite = knowledge_factory()
            finite.instants = {c: {k: v for k, v in rows.items() if math.isfinite(v)}
                               for c, rows in knowledge.instants.items()}
            finite.durations = {c: {k: v for k, v in rows.items() if math.isfinite(v)}
                                for c, rows in knowledge.durations.items()}
            period, values = snapshot_fn(finite, finite_statistics=True)
            if period is None and qualifiers:
                period = max(key[2] for key in qualifiers)
            if period is not None:
                # All clocks in this simultaneous group have the same availability.
                clock = max(possible_clocks, key=lambda c: (c["clock_kind"], c["filed_ns"]))
                main = max(event_facts, key=lambda f: (f.end, f.form.startswith("10-K"), f.accn))
                snapshot = dict(filed=main.filed, available_date=dt.datetime.fromtimestamp(
                    clock["available_ns"] // NS, UTC).date(), period_end=period,
                    form=main.form, accn=main.accn, **values, **clock)
                snapshot["fact_vintage_qualified"] = int(all(q[0] for q in qualifiers.values()))
                snapshot["knowledge_clock_qualified"] = int(all(q[1] for q in qualifiers.values()))
                snapshots.append(snapshot)
                audit.append(dict(cik=cik, available_ns=clock["available_ns"],
                    active_cells=len(qualifiers), expired_cells=len(expired) if qualifiers else 0,
                    unqualified_vintage_cells=sum(not q[0] for q in qualifiers.values()),
                    modeled_clock_cells=sum(not q[1] for q in qualifiers.values()),
                    numeric_withheld=not bool(snapshot["fact_vintage_qualified"]),
                    reason="snapshot_active_knowledge_qualification"))
        index = stop
    return snapshots, shares, audit
