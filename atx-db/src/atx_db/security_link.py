"""D1 bitemporal issuer links on the original SpiderRock security-line axis.

No database, network or current-ticker lookup. Inputs are independently sealed
evidence projections. Whole-interval bracketing is retrospective audit only.
The prospective V2 rule requires two already-public agreeing observations and
never backfills earlier decisions. Acceptance alone is not proof of first public
availability or of the vintage of a later-corrected identity field.
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import json
import re
from collections import defaultdict
from collections.abc import Iterable, Mapping
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TextIO

UTC = dt.timezone.utc
SEAL = dt.date(2020, 1, 1)
SCHEMA = "atx.security-link/v3"
PROSPECTIVE_SCHEMA = "atx.security-link/v2"
LEGACY_SCHEMA = "atx.security-link/v1"


def utc(value: dt.datetime) -> dt.datetime:
    if value.tzinfo is None or value.utcoffset() is None:
        raise ValueError("identity clock requires an explicit timezone")
    return value.astimezone(UTC)


def cik_key(value: str | int) -> str:
    text = str(value).strip()
    if not re.fullmatch(r"[0-9]{1,10}", text) or int(text) == 0:
        raise ValueError("invalid issuer CIK")
    return text.zfill(10)


def symbol_key(value: str) -> str:
    # Punctuation is meaningful for share classes. Never equate BRK.B and BRK-B
    # without explicit dated evidence describing that alias.
    symbol = value.strip().upper()
    if not symbol or any(c.isspace() for c in symbol):
        raise ValueError("empty or whitespace-bearing ticker")
    return symbol


def _hash(value: str) -> None:
    if not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("source SHA256 must be 64 lowercase hexadecimal characters")


def _window(begin: dt.date, end: dt.date | None) -> None:
    if begin >= SEAL or (end is not None and (end <= begin or end > SEAL)):
        raise ValueError("identity window must be nonempty and sealed before 2020")


@dataclass(frozen=True)
class FilingEvidence:
    evidence_id: str
    accession: str
    cik: str
    ticker: str
    effective_date: dt.date
    accepted_at: dt.datetime | None
    source_sha256: str
    source_locator: str
    observed_at: dt.datetime
    published_at: dt.datetime | None = None
    revision_available_at: dt.datetime | None = None
    revision_status: str = "unverified"
    evidence_kind: str = "dated-issuer-symbol"
    accepted_raw: str = ""

    def __post_init__(self) -> None:
        if not self.evidence_id or not self.accession or not self.source_locator:
            raise ValueError("filing identity and source locator are required")
        cik_key(self.cik)
        symbol_key(self.ticker)
        _window(self.effective_date, None)
        _hash(self.source_sha256)
        utc(self.observed_at)
        for clock in (self.accepted_at, self.published_at, self.revision_available_at):
            if clock is not None and utc(clock).date() >= SEAL:
                raise ValueError("filing evidence clock reaches the sealed era")
        if self.revision_status not in {"original-confirmed", "revision-confirmed", "unverified"}:
            raise ValueError("unknown revision status")

    @property
    def reason(self) -> str:
        if self.evidence_kind != "dated-issuer-symbol":
            return "symbol_candidate_not_dated_issuer_evidence"
        if self.accepted_at is None:
            return "missing_acceptance"
        if self.revision_status == "unverified":
            return "unverified_identity_vintage"
        if self.revision_status == "revision-confirmed" and self.revision_available_at is None:
            return "missing_revision_clock"
        if self.published_at is None:
            return "acceptance_only_public_availability_unverified"
        return ""

    @property
    def available_at(self) -> dt.datetime | None:
        if self.reason:
            return None
        return max(utc(c) for c in (self.accepted_at, self.published_at, self.revision_available_at) if c)


@dataclass(frozen=True)
class VendorInterval:
    """All fields, including a finite endpoint, belong to the known-at payload.

    For a line known open at admission, retain valid_to=None and publish a
    separate VendorExpiry later; do not silently amend this row with a future end.
    """
    interval_id: str
    sr_id: str
    ticker: str
    valid_from: dt.date
    valid_to: dt.date | None
    available_at: dt.datetime
    source_sha256: str
    source_locator: str
    availability_verified: bool = False

    def __post_init__(self) -> None:
        if not self.interval_id or not re.fullmatch(r"[1-9][0-9]*", self.sr_id):
            raise ValueError("original positive SpiderRock securityID required")
        symbol_key(self.ticker)
        _window(self.valid_from, self.valid_to)
        _hash(self.source_sha256)
        if not self.source_locator or utc(self.available_at).date() >= SEAL:
            raise ValueError("missing vendor provenance or unsealed availability")


@dataclass(frozen=True)
class SecurityLink:
    link_id: str
    sr_id: str
    cik: str
    ticker: str
    valid_from: dt.date
    valid_to: dt.date
    accepted_at: dt.datetime | None
    available_at: dt.datetime
    evidence_ids: tuple[str, ...]
    method: str = "bracketed-filings-v1"
    reviewer: str = ""
    reason: str = ""
    end_kind: str = "source-expiry"  # seal-bound does not assert an economic end
    retired_from: dt.date | None = None
    retired_available_at: dt.datetime | None = None

    def __post_init__(self) -> None:
        _window(self.valid_from, self.valid_to)
        if self.cik != cik_key(self.cik) or self.ticker != symbol_key(self.ticker):
            raise ValueError("published CIK/ticker must use canonical identity keys")
        if not re.fullmatch(r"[1-9][0-9]*", self.sr_id) or not self.link_id or not self.evidence_ids:
            raise ValueError("invalid link identity/evidence")
        if ((self.accepted_at is not None and utc(self.available_at) < utc(self.accepted_at))
                or utc(self.available_at).date() >= SEAL):
            raise ValueError("link availability precedes acceptance or reaches seal")
        if self.method not in {"bracketed-filings-v1", "dated-override-v1", "conflict-v1",
                               "prospective-two-filings-v2", "prospective-conflict-v2", "prospective-expiry-v2"}:
            raise ValueError("unknown link method")
        if self.accepted_at is None and self.method != "prospective-expiry-v2":
            raise ValueError("filing-backed link requires observed acceptance")
        if self.end_kind not in {"source-expiry", "seal-bound"}:
            raise ValueError("unknown link end kind")
        if self.end_kind == "seal-bound" and self.valid_to != SEAL:
            raise ValueError("open economic interval must terminate at the artifact seal")
        if (self.retired_from is None) != (self.retired_available_at is None):
            raise ValueError("conflict retirement requires both effective and knowledge clocks")
        if self.retired_from is not None:
            _window(self.retired_from, None)
            if (self.method != "prospective-conflict-v2" or
                    utc(self.retired_available_at).date() >= SEAL):
                raise ValueError("invalid conflict retirement")
        if self.method == "dated-override-v1" and (not self.reviewer or not self.reason):
            raise ValueError("override requires reviewer and reason")

    @property
    def security_id(self) -> str:
        return f"spiderrock.securityID:{self.sr_id}"

    @property
    def owner_id(self) -> str:
        prefix = "CONFLICT:" if self.is_marker else ""
        return f"{prefix}SEC-CIK-{cik_key(self.cik)}"

    @property
    def is_marker(self) -> bool:
        return self.method in {"conflict-v1", "prospective-conflict-v2", "prospective-expiry-v2"}

    @property
    def live_eligible(self) -> bool:
        return self.method not in {"bracketed-filings-v1", "conflict-v1"}


@dataclass(frozen=True)
class VendorExpiry:
    """A separately dated closure event for a previously open vendor line.

    An economic end learned later changes only subsequent decisions. The event
    must carry its own original-publication/vintage evidence, not a modeled lag.
    """
    evidence_id: str
    interval_id: str
    effective_date: dt.date
    available_at: dt.datetime
    source_sha256: str
    source_locator: str
    availability_verified: bool = False

    def __post_init__(self) -> None:
        if not self.evidence_id or not self.interval_id or not self.source_locator:
            raise ValueError("expiry event requires original interval/provenance identity")
        _window(self.effective_date, None)
        _hash(self.source_sha256)
        if utc(self.available_at).date() >= SEAL:
            raise ValueError("expiry event reaches sealed era")


@dataclass(frozen=True)
class LinkDecision:
    link: SecurityLink | None
    reason: str
    candidate_ids: tuple[str, ...] = ()


def _id(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, default=str).encode()).hexdigest()


def build_links(intervals: Iterable[VendorInterval], filings: Iterable[FilingEvidence]
                ) -> tuple[list[SecurityLink], list[dict]]:
    """Retrospective whole-interval V1 audit, never a live security master.

    Both different accessions must bracket the *entire* vendor interval. Thus
    confirmed historical history cannot be used before its right-hand proof.
    Unknown/open ends remain unresolved; they are not inferred from future bars.
    The live resolver/exporter excludes these audit-only methods. Use the
    separately versioned build_prospective_links for causal corroboration.
    """
    by_symbol: dict[str, list[FilingEvidence]] = defaultdict(list)
    evidence_by_id: dict[str, FilingEvidence] = {}
    for evidence in filings:
        old = evidence_by_id.setdefault(evidence.evidence_id, evidence)
        if old != evidence:
            raise ValueError("conflicting duplicate filing evidence id")
    for evidence in evidence_by_id.values():
        by_symbol[symbol_key(evidence.ticker)].append(evidence)
    links, unresolved = [], []
    seen_intervals: dict[str, VendorInterval] = {}
    for interval in intervals:
        if interval.interval_id in evidence_by_id:
            raise ValueError("vendor and filing proof ids must occupy distinct namespaces")
        if interval.interval_id in seen_intervals:
            if seen_intervals[interval.interval_id] != interval:
                raise ValueError("conflicting duplicate vendor interval id")
            continue
        seen_intervals[interval.interval_id] = interval
    vendor_rows = sorted(seen_intervals.values(), key=lambda i: i.interval_id)
    for interval in vendor_rows:
        rows = by_symbol[symbol_key(interval.ticker)]
        base = {"interval_id": interval.interval_id, "sr_id": interval.sr_id,
                "ticker": interval.ticker, "valid_from": str(interval.valid_from),
                "valid_to": str(interval.valid_to) if interval.valid_to else None}
        if interval.valid_to is None or not interval.availability_verified:
            unresolved.append(dict(base, reason="unknown_interval_end" if interval.valid_to is None
                                   else "vendor_availability_unverified"))
            continue
        ciks = sorted({cik_key(e.cik) for e in rows})
        if not ciks:
            unresolved.append(dict(base, reason="no_dated_symbol_evidence"))
        for cik in ciks:
            group = [e for e in rows if cik_key(e.cik) == cik]
            left = [e for e in group if e.effective_date <= interval.valid_from]
            right = [e for e in group if e.effective_date >= interval.valid_to - dt.timedelta(days=1)]
            pairs = [(a, b) for a in left for b in right if a.accession != b.accession]
            valid = [(a, b) for a, b in pairs if not a.reason and not b.reason]
            if not valid:
                reasons = sorted({e.reason for pair in pairs for e in pair if e.reason})
                unresolved.append(dict(base, cik=cik, reason=";".join(reasons) if pairs
                                       else "missing_distinct_bracketing_filings"))
                continue
            # Earliest causal confirmation, deterministic under input permutation.
            a, b = min(valid, key=lambda p: (max(p[0].available_at, p[1].available_at),
                                            p[0].evidence_id, p[1].evidence_id))
            accepted = max(utc(a.accepted_at), utc(b.accepted_at))
            available = max(utc(interval.available_at), a.available_at, b.available_at)
            ids = (interval.interval_id, a.evidence_id, b.evidence_id)
            identity = [base, cik, ids, str(accepted), str(available)]
            links.append(SecurityLink(_id(identity), interval.sr_id, cik, symbol_key(interval.ticker),
                                      interval.valid_from, interval.valid_to, accepted, available, ids))
            # Later contradictory evidence changes only later knowledge states.
            # Markers carry no accounting facts and cannot certify an alternative.
            for rival in rows:
                if (cik_key(rival.cik) != cik and not rival.reason and
                        interval.valid_from <= rival.effective_date < interval.valid_to):
                    proof = (interval.interval_id, rival.evidence_id)
                    links.append(SecurityLink(_id([identity, proof, "conflict"]), interval.sr_id,
                        cik_key(rival.cik), symbol_key(interval.ticker), interval.valid_from, interval.valid_to,
                        utc(rival.accepted_at), max(utc(interval.available_at), rival.available_at),
                        proof, "conflict-v1", reason="contradicting_dated_issuer"))
                    unresolved.append(dict(base, reason="contradicting_dated_issuer",
                        available_at=str(max(utc(interval.available_at), rival.available_at)),
                        evidence_ids=proof))
            for other in vendor_rows:
                if (other.availability_verified and other.sr_id != interval.sr_id
                        and symbol_key(other.ticker) == symbol_key(interval.ticker)
                        and other.valid_from < interval.valid_to and
                        (other.valid_to is None or other.valid_to > interval.valid_from)):
                    proof = (interval.interval_id, other.interval_id)
                    links.append(SecurityLink(_id([identity, proof, "line-conflict"]), interval.sr_id,
                        cik, symbol_key(interval.ticker), max(interval.valid_from, other.valid_from),
                        min(interval.valid_to, other.valid_to or interval.valid_to), accepted,
                        max(available, utc(other.available_at)), proof, "conflict-v1",
                        reason="ambiguous_overlapping_vendor_lines"))
                    unresolved.append(dict(base, reason="ambiguous_overlapping_vendor_lines",
                        available_at=str(max(available, utc(other.available_at))), evidence_ids=proof))
    return sorted(links, key=lambda x: x.link_id), unresolved


def build_prospective_links(intervals: Iterable[VendorInterval], filings: Iterable[FilingEvidence],
                            expiries: Iterable[VendorExpiry] = ()) -> tuple[list[SecurityLink], list[dict]]:
    """Causal two-observation V2, distinct from retrospective whole-interval proof.

    Both agreeing accessions must already be public and have distinct effective
    dates inside the vendor line. Eligibility starts only after corroboration.
    A verified finite vendor row means its entire stated interval was known at
    available_at. Otherwise supply an open row and separately clocked expiries;
    never replace the open row retrospectively with a later-known finite row.
    """
    vendors, evidence, closures = {}, {}, {}
    for value in intervals:
        key = value.interval_id
        if key in vendors and vendors[key] != value:
            raise ValueError("conflicting duplicate vendor interval id")
        vendors[key] = value
    for value in filings:
        if value.evidence_id in vendors:
            raise ValueError("proof ids must occupy distinct namespaces")
        if value.evidence_id in evidence and evidence[value.evidence_id] != value:
            raise ValueError("conflicting duplicate filing evidence id")
        evidence[value.evidence_id] = value
    for value in expiries:
        if value.evidence_id in vendors or value.evidence_id in evidence:
            raise ValueError("proof ids must occupy distinct namespaces")
        if value.evidence_id in closures and closures[value.evidence_id] != value:
            raise ValueError("conflicting duplicate expiry evidence id")
        closures[value.evidence_id] = value
    links, unresolved = [], []
    known_expiries: dict[str, VendorExpiry] = {}
    for event in closures.values():
        if event.interval_id not in vendors:
            unresolved.append({"evidence_id": event.evidence_id, "reason": "expiry_interval_unresolved"})
        elif event.availability_verified:
            if event.effective_date < vendors[event.interval_id].valid_from:
                raise ValueError("expiry event precedes vendor line")
            old = known_expiries.get(event.interval_id)
            if old is not None and old.effective_date != event.effective_date:
                raise ValueError("conflicting verified economic expiry dates require adjudication")
            if old is None or (utc(event.available_at), event.evidence_id) < (utc(old.available_at), old.evidence_id):
                known_expiries[event.interval_id] = event
    for interval in sorted(vendors.values(), key=lambda v: v.interval_id):
        base = {"interval_id": interval.interval_id, "sr_id": interval.sr_id,
                "rule": "prospective-two-filings-v2"}
        if not interval.availability_verified:
            unresolved.append(dict(base, reason="vendor_availability_unverified"))
            continue
        end = interval.valid_to or SEAL
        end_kind = "source-expiry" if interval.valid_to else "seal-bound"
        rows = [e for e in evidence.values() if symbol_key(e.ticker) == symbol_key(interval.ticker)
                and interval.valid_from <= e.effective_date < end]
        for e in rows:
            if e.reason:
                unresolved.append(dict(base, evidence_id=e.evidence_id, reason=e.reason))
        qualified = [e for e in rows if not e.reason]
        if not qualified:
            unresolved.append(dict(base, reason="no_qualified_in_interval_symbol_observation"))
        for cik in sorted({cik_key(e.cik) for e in qualified}):
            group = sorted((e for e in qualified if cik_key(e.cik) == cik),
                           key=lambda e: (e.available_at, e.evidence_id))
            pair = None
            for offset, b in enumerate(group):
                a = next((a for a in group[:offset] if a.accession != b.accession
                          and a.effective_date != b.effective_date), None)
                if a is not None:
                    pair = a, b
                    break
            if pair is None:
                unresolved.append(dict(base, cik=cik, reason="needs_two_independent_public_observations"))
                continue
            a, b = pair
            available = max(utc(interval.available_at), a.available_at, b.available_at)
            if available.date() >= end:
                unresolved.append(dict(base, cik=cik, reason="corroboration_after_known_expiry"))
                continue
            begin = max(interval.valid_from, a.effective_date, b.effective_date)
            accepted = max(utc(a.accepted_at), utc(b.accepted_at))
            ids = (interval.interval_id, a.evidence_id, b.evidence_id)
            identity = [base, cik, ids, str(begin), str(end), str(available)]
            links.append(SecurityLink(_id(identity), interval.sr_id, cik, symbol_key(interval.ticker),
                begin, end, accepted, available, ids, "prospective-two-filings-v2", end_kind=end_kind))
            for rival in qualified:
                if cik_key(rival.cik) == cik:
                    continue
                conflict_begin = max(begin, rival.effective_date)
                proof = (interval.interval_id, rival.evidence_id)
                known = max(utc(interval.available_at), rival.available_at)
                links.append(SecurityLink(_id([identity, proof, "conflict"]), interval.sr_id,
                    cik_key(rival.cik), symbol_key(interval.ticker), conflict_begin, end,
                    utc(rival.accepted_at), known, proof, "prospective-conflict-v2",
                    reason="contradicting_dated_issuer", end_kind=end_kind))
                unresolved.append(dict(base, reason="contradicting_dated_issuer", available_at=str(known),
                                       evidence_ids=proof))
            for other in vendors.values():
                other_end = other.valid_to or SEAL
                conflict_begin, conflict_end = max(begin, other.valid_from), min(end, other_end)
                if (other.sr_id == interval.sr_id or not other.availability_verified or
                        symbol_key(other.ticker) != symbol_key(interval.ticker) or conflict_begin >= conflict_end):
                    continue
                proof = (interval.interval_id, other.interval_id)
                known = max(available, utc(other.available_at))
                expiry = known_expiries.get(other.interval_id)
                retired_from = expiry.effective_date if expiry else None
                retired_at = max(utc(other.available_at), utc(expiry.available_at)) if expiry else None
                if expiry:
                    proof += (expiry.evidence_id,)
                links.append(SecurityLink(_id([identity, proof, "line-conflict", str(conflict_begin),
                    str(conflict_end), str(known), str(retired_from), str(retired_at)]), interval.sr_id, cik,
                    symbol_key(interval.ticker), conflict_begin, conflict_end, accepted, known, proof,
                    "prospective-conflict-v2", reason="ambiguous_overlapping_vendor_lines",
                    end_kind="seal-bound" if conflict_end == SEAL else "source-expiry",
                    retired_from=retired_from, retired_available_at=retired_at))
                unresolved.append(dict(base, reason="ambiguous_overlapping_vendor_lines", available_at=str(known),
                                       evidence_ids=proof))
            for event in closures.values():
                if event.interval_id != interval.interval_id:
                    continue
                if event.effective_date < interval.valid_from:
                    raise ValueError("expiry event precedes vendor line")
                if not event.availability_verified:
                    unresolved.append(dict(base, evidence_id=event.evidence_id, reason="expiry_availability_unverified"))
                    continue
                if event.effective_date >= end:
                    continue
                proof = (interval.interval_id, event.evidence_id)
                known = max(utc(interval.available_at), utc(event.available_at))
                links.append(SecurityLink(_id([identity, proof, "expiry"]), interval.sr_id, cik,
                    symbol_key(interval.ticker), max(begin, event.effective_date), end, None, known, proof,
                    "prospective-expiry-v2", reason="dated_vendor_expiry", end_kind=end_kind))
    return sorted(links, key=lambda x: x.link_id), unresolved


def resolve_link(links: Iterable[SecurityLink], sr_id: str, session: dt.date,
                 decision_at: dt.datetime, *, retrospective_audit: bool = False) -> LinkDecision:
    """Point-in-time selection: future competitors never remove an earlier decision."""
    clock = utc(decision_at)
    if session >= SEAL or clock.date() >= SEAL:
        raise ValueError("link decision reaches sealed era")
    eligible = [x for x in links if x.sr_id == sr_id and x.valid_from <= session < x.valid_to
                and utc(x.available_at) < clock and (x.live_eligible or retrospective_audit)
                and not (x.retired_from is not None and session >= x.retired_from
                         and utc(x.retired_available_at) < clock)]
    if not eligible:
        return LinkDecision(None, "no_available_covering_link")
    overrides = [x for x in eligible if x.method == "dated-override-v1"]
    if overrides:
        eligible = overrides
    ids = tuple(sorted(x.link_id for x in eligible))
    if any(x.is_marker for x in eligible):
        return LinkDecision(None, "conflicting_available_identity_evidence", ids)
    if len({cik_key(x.cik) for x in eligible}) != 1:
        return LinkDecision(None, "conflicting_available_issuers", ids)
    return LinkDecision(min(eligible, key=lambda x: (utc(x.available_at), x.link_id)), "linked", ids)


def read_overrides(stream: TextIO, evidence: Mapping[str, FilingEvidence]) -> list[SecurityLink]:
    """CSV overrides require original qualified evidence AND a dated adjudication.

    Columns: sr_id,cik,ticker,valid_from,valid_to,evidence_ids (semicolon list),
    reviewed_at (offset ISO time),reviewer,reason. Today's adjudication cannot be
    backdated by assigning it an old filing's acceptance timestamp.
    """
    result = []
    for row in csv.DictReader(stream):
        ids = tuple(sorted(set(row["evidence_ids"].split(";"))))
        proof = [evidence[key] for key in ids]
        if not proof or any(e.reason for e in proof):
            raise ValueError("override lacks qualified original identity evidence")
        if any(cik_key(e.cik) != cik_key(row["cik"]) or symbol_key(e.ticker) != symbol_key(row["ticker"])
               for e in proof):
            raise ValueError("override evidence issuer/symbol mismatch")
        reviewed = utc(dt.datetime.fromisoformat(row["reviewed_at"]))
        accepted = max(utc(e.accepted_at) for e in proof)
        available = max(reviewed, *(e.available_at for e in proof))
        result.append(SecurityLink(_id(row), row["sr_id"], cik_key(row["cik"]), symbol_key(row["ticker"]),
            dt.date.fromisoformat(row["valid_from"]), dt.date.fromisoformat(row["valid_to"]),
            accepted, available, ids, "dated-override-v1", row["reviewer"], row["reason"]))
    return result


def coverage_report(links: Iterable[SecurityLink], requests: Iterable[tuple[str, dt.date, dt.datetime]]) -> dict:
    """Requested identity coverage, not a surrogate for the plan's company/type gate."""
    frozen = tuple(links)
    unresolved, total, linked = [], 0, 0
    for sr_id, session, decision_at in requests:
        result = resolve_link(frozen, sr_id, session, decision_at)
        total += 1
        linked += result.link is not None
        if result.link is None:
            unresolved.append({"sr_id": sr_id, "session": str(session),
                               "decision_at": utc(decision_at).isoformat(), "reason": result.reason})
    return {"scope": "requested_decisions_only", "requests": total, "linked": linked,
            "rate": linked / total if total else None, "unresolved": unresolved,
            "plan_coverage_qualified": False,
            "pending": ["operating-company denominator and exclusions", "survivorship strata",
                        "2013-2019 t1000/t3000 coverage", "recovered 82-case fixture"]}


def write_link_artifact(directory: Path, links: Iterable[SecurityLink], unresolved: Iterable[dict],
                        sources: Mapping[str, str]) -> str:
    """Exclusive immutable versioned table. Manifest is renamed into place LAST."""
    for digest in sources.values():
        _hash(digest)
    rows = sorted(links, key=lambda x: x.link_id)
    if any(key not in sources for row in rows for key in row.evidence_ids):
        raise ValueError("every referenced proof must have a source hash binding")
    if len({x.link_id for x in rows}) != len(rows):
        raise ValueError("duplicate published link id")
    directory.mkdir(parents=False, exist_ok=False)
    files = {}
    for name, content in (("links.jsonl", (asdict(x) for x in rows)),
                          ("unresolved.jsonl", unresolved)):
        data = "".join(json.dumps(x, sort_keys=True, default=str) + "\n" for x in content).encode()
        (directory / name).write_bytes(data)
        files[name] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    manifest = {"schema": SCHEMA, "seal_exclusive": str(SEAL), "files": files,
                "sources": dict(sorted(sources.items())), "rows": len(rows),
                "security_id_namespace": "spiderrock.securityID",
                "interval_end": "exclusive", "availability": "max-required-proof-clock",
                "live_clock_comparison": "available_at < decision",
                "rules": sorted({x.method for x in rows}),
                "retrospective_v1": "audit-only; excluded by live resolver/exporter",
                "plan_coverage_qualified": False}
    data = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
    part = directory / "manifest.json.part"
    part.write_bytes(data)
    part.rename(directory / "manifest.json")
    return hashlib.sha256(data).hexdigest()


def read_link_artifact(directory: Path, expected_manifest_sha256: str = "") -> tuple[list[SecurityLink], dict]:
    path = directory / "manifest.json"
    if path.is_symlink() or path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("invalid link manifest")
    data = path.read_bytes()
    if expected_manifest_sha256 and hashlib.sha256(data).hexdigest() != expected_manifest_sha256:
        raise ValueError("link manifest checksum mismatch")
    manifest = json.loads(data)
    if manifest["schema"] not in {LEGACY_SCHEMA, PROSPECTIVE_SCHEMA, SCHEMA} or manifest["seal_exclusive"] != str(SEAL):
        raise ValueError("unsupported or unsealed link artifact")
    for digest in manifest["sources"].values():
        _hash(digest)
    if set(manifest["files"]) != {"links.jsonl", "unresolved.jsonl"}:
        raise ValueError("unexpected link artifact members")
    payloads = {}
    for name, binding in manifest["files"].items():
        member = directory / name
        if member.is_symlink() or member.stat().st_size != binding["bytes"] or binding["bytes"] > 128 * 1024 * 1024:
            raise ValueError("link artifact byte count/type/budget mismatch")
        payload = member.read_bytes()
        if hashlib.sha256(payload).hexdigest() != binding["sha256"]:
            raise ValueError("link artifact checksum mismatch")
        payloads[name] = payload
    result = []
    for line in payloads["links.jsonl"].splitlines():
        row = json.loads(line)
        if manifest["schema"] != SCHEMA and ("retired_from" in row or "retired_available_at" in row):
            raise ValueError("v3 conflict retirement cannot be labeled as an older artifact")
        if manifest["schema"] == LEGACY_SCHEMA and (
                row["method"] not in {"bracketed-filings-v1", "dated-override-v1", "conflict-v1"}
                or "end_kind" in row):
            raise ValueError("v2 link semantics cannot be labeled as a v1 artifact")
        for key in ("valid_from", "valid_to"):
            row[key] = dt.date.fromisoformat(row[key])
        for key in ("accepted_at", "available_at"):
            row[key] = dt.datetime.fromisoformat(row[key]) if row[key] is not None else None
        if row.get("retired_from") is not None:
            row["retired_from"] = dt.date.fromisoformat(row["retired_from"])
            row["retired_available_at"] = dt.datetime.fromisoformat(row["retired_available_at"])
        row["evidence_ids"] = tuple(row["evidence_ids"])
        result.append(SecurityLink(**row))
    if len(result) != manifest["rows"] or len({x.link_id for x in result}) != len(result):
        raise ValueError("link row count/identity mismatch")
    if manifest["schema"] in {SCHEMA, PROSPECTIVE_SCHEMA} and (
            manifest.get("rules") != sorted({x.method for x in result}) or
            manifest.get("live_clock_comparison") != "available_at < decision"):
        raise ValueError("link rule/clock manifest mismatch")
    if any(key not in manifest["sources"] for row in result for key in row.evidence_ids):
        raise ValueError("unbound identity proof")
    return result, manifest
