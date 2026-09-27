"""Bounded, event-based accounting histories shared by F.1 and 2.9.

The raw source remains authoritative. Fiscal slots come from then-visible filing
contexts, duration starts require evidence, and every arithmetic edge is retained.
No warehouse, price, label or return reader is imported here.
"""
from __future__ import annotations

import collections
import datetime as dt
import itertools
import json
from decimal import Decimal, InvalidOperation, localcontext
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

from .fundamental_sources import canonical_sha256, file_sha256, parquet_relation

ENGINE_VERSION = "item_vintages_v2"
PERIOD_POLICY = {"quarter_days": [70, 112], "half_year_days": [150, 215],
                 "nine_month_days": [240, 310], "year_days": [350, 380],
                 "quarter_anchor_age_days": 200, "annual_anchor_age_days": 400,
                 "fiscal_slots": "reported_fy_times_4_plus_reported_quarter",
                 "missing": "null", "weighted_shares": "nonadditive_direct_only",
                 "conflict": "semantic_before_provenance", "snapshot_date": "2026-09-20"}


def _decimal(value: Any) -> Decimal | None:
    try:
        number = Decimal(str(value))
        return number if number.is_finite() else None
    except (InvalidOperation, ValueError):
        return None


def _clock(value: dt.datetime) -> dt.datetime:
    return value.replace(tzinfo=None) if value.tzinfo is None else value.astimezone(dt.UTC).replace(tzinfo=None)


def _fact_key(row: dict[str, Any]) -> tuple:
    return tuple(row[k] for k in ("accession", "taxonomy", "concept", "unit", "period_end", "qtrs")) + (_decimal(row["value_exact"]),)


def corroborate(rows: list[dict[str, Any]]) -> None:
    """Borrow clocks only for a unique exact fact/duration, never just an accession.

    FSDS encodes duration as qtrs. A unique same-value CF start in that duration
    class supplies the otherwise absent start; ambiguous starts lend no clock.
    """
    groups = collections.defaultdict(list)
    for row in rows:
        row["dependencies"] = [row["candidate_id"]]
        if row["available_at"] is not None:
            row["available_at"] = _clock(row["available_at"])
        if row["source_status"] == "candidate" and _decimal(row["value_exact"]) is not None:
            groups[_fact_key(row)].append(row)
    for group in groups.values():
        fsds = [r for r in group if r["source_kind"] == "fsds"]
        cf = [r for r in group if r["source_kind"] == "cf"]
        if not fsds or not cf:
            continue
        clocks = {r["available_at"] for r in fsds}
        starts = {r["period_start"] for r in cf}
        if len(clocks) != 1 or len(starts) != 1 or (None in starts and group[0]["qtrs"] != 0):
            continue
        start = next(iter(starts))
        for row in group:
            peers = cf if row["source_kind"] == "fsds" else fsds
            row["dependencies"] += sorted({r["candidate_id"] for r in peers})
            row["period_start"] = start
            row["available_at"] = next(iter(clocks))
            row["clock_basis"] = "fsds_same_fact_corroborated"
            row["period_basis"] = "same_fact_cf_start" if start else "instant"


def _grid(rows: list[dict[str, Any]]) -> tuple[dict, dict]:
    # Current reported period for CF is the largest duration endpoint in the same
    # filing. Cover-page stocks are excluded. FSDS supplies its explicit SUB period.
    current = {}
    for row in rows:
        if row["source_status"] == "candidate" and row["qtrs"] and row["period_end"]:
            key = row["accession"]
            current[key] = max(current.get(key, row["period_end"]), row["period_end"])
    claims = collections.defaultdict(dict)
    for row in rows:
        if row["source_status"] != "candidate" or row["available_at"] is None:
            continue
        q = {"Q1": 1, "Q2": 2, "Q3": 3, "FY": 4, "Q4": 4}.get(row["reported_fp"])
        fy = row["reported_fy"]
        end = row["filing_period"] or current.get(row["accession"])
        if q is None or fy is None or end is None:
            continue
        if row["period_end"] != end and row["source_kind"] != "fsds":
            continue
        key, point = (end, int(fy), q), (row["available_at"], row["candidate_id"])
        if key not in claims[end] or point < claims[end][key]:
            claims[end][key] = point
    by_end, by_slot = {}, collections.defaultdict(list)
    for end, records in claims.items():
        ordered = sorted((clock, candidate, fy, q) for (_, fy, q), (clock, candidate) in records.items())
        clock, candidate, fy, q = ordered[0]
        point = {"end": end, "fy": fy, "q": q, "clock": clock, "input": candidate,
                 "invalid_at": ordered[1][0] if len(ordered) > 1 else None}
        by_end[end] = point
        by_slot[fy*4+q].append(point)
    for points in by_slot.values():
        if len(points) > 1:
            ordered = sorted(points, key=lambda p: (p["clock"], p["end"]))
            conflict = ordered[1]["clock"]
            for point in points:
                point["invalid_at"] = min(point["invalid_at"] or conflict, conflict)
    return by_end, by_slot


def _period(row: dict[str, Any], by_end: dict, by_slot: dict) -> dict[str, Any] | None:
    end, qtrs = row["period_end"], row["qtrs"]
    point = by_end.get(end)
    if point is None or qtrs is None:
        return None
    available = max(row["available_at"], point["clock"])
    invalid = point["invalid_at"]
    start, basis = row["period_start"], row.get("period_basis", "cf_reported_start")
    dependencies = row["dependencies"] + [point["input"]]
    if qtrs == 0:
        if row["period_kind"] != "instant" or row["taxonomy"] == "dei":
            return None
        start, freq, basis = None, "instant", "filing_fiscal_endpoint"
    else:
        if row["period_kind"] != "duration" or qtrs not in {1, 2, 3, 4}:
            return None
        if qtrs > 1 and point["q"] != qtrs:
            return None
        freq = "q" if qtrs == 1 else "fy" if qtrs == 4 else "ytd"
        if start is None:
            previous_slot = point["fy"]*4 + (point["q"]-1 if qtrs == 1 else 0)
            predecessors = by_slot.get(previous_slot, [])
            if len(predecessors) != 1:
                return None
            previous = predecessors[0]
            start = previous["end"] + dt.timedelta(days=1)
            available = max(available, previous["clock"])
            dependencies.append(previous["input"])
            if previous["invalid_at"] is not None:
                invalid = min(invalid or previous["invalid_at"], previous["invalid_at"])
            basis = "fiscal_grid_inferred"
        days = (end-start).days+1
        low, high = {1: (70, 112), 2: (150, 215), 3: (240, 310), 4: (350, 380)}[qtrs]
        if not low <= days <= high:
            return None
    if available.date() > dt.date(2026, 9, 20):
        return None
    return {"period_start": start, "period_end": end, "freq": freq,
            "fiscal_year": point["fy"], "fiscal_qtr": point["q"],
            "available_at": available, "invalid_at": invalid, "period_basis": basis,
            "dependencies": sorted(set(dependencies))}


def _event(template: dict[str, Any], *, clock: dt.datetime, value: Decimal | None,
           status: str, inputs: Sequence[str], rule: str, **extra) -> dict[str, Any]:
    row = {k: template[k] for k in ("owner_id", "cik", "item", "freq", "period_start", "period_end",
                                   "fiscal_year", "fiscal_qtr", "unit", "clock_basis", "period_basis", "accession")}
    row.update(value=None if value is None else float(value), value_exact=None if value is None else str(value),
               available_at=clock, valid_until=None, status=status, rule_id=rule,
               source_kind=template.get("source_kind", "derived"), inputs=sorted(set(inputs)), **extra)
    row.setdefault("event_stage", "derived")
    row["year"] = row["fiscal_year"]
    row["vintage_id"] = canonical_sha256(row)
    row["lineage_id"] = row["vintage_id"]
    return row


def _series_key(row: dict[str, Any]) -> tuple:
    return tuple(row[k] for k in ("item", "freq", "period_start", "period_end", "fiscal_year", "fiscal_qtr"))


def _merge_series(inputs: Sequence[Sequence[dict]], template: dict, rule: str,
                  operation, *, start_at: dt.datetime | None = None) -> list[dict]:
    """Evaluate each input event, including nullification. No nullable arg_max."""
    events = collections.defaultdict(list)
    for position, series in enumerate(inputs):
        for row in series:
            events[row["available_at"]].append((position, row))
    current, output = [None]*len(inputs), []
    for clock in sorted(events):
        for position, row in events[clock]:
            current[position] = row
        if start_at is not None and clock < start_at:
            continue
        outcome = operation(current)
        value, status = outcome[:2]
        dependencies = [r["vintage_id"] for r in current if r is not None]
        mixed = dict(template)
        mixed["clock_basis"] = "+".join(sorted({r["clock_basis"] for r in current if r is not None}))
        mixed["accession"] = "+".join(sorted({r["accession"] or "" for r in current if r is not None}))
        if rule == "slot:duration_reconcile":
            visible = [r for r in current if r is not None]
            mixed.update({k: visible[0][k] for k in ("period_start", "period_end")})
        output.append(_event(mixed, clock=clock, value=value, status=status, inputs=dependencies, rule=rule,
                             missing_value_basis=outcome[2] if len(outcome) > 2 else None))
    return output


def _arithmetic(current, signs):
    if any(row is None for row in current):
        return None, "missing_input"
    if any(row["value_exact"] is None for row in current):
        return None, "invalid_operand"
    with localcontext() as context:
        context.prec = 40
        return sum((_decimal(row["value_exact"])*sign for row, sign in zip(current, signs)), Decimal(0)), "valid"


def build_owner(rows: list[dict[str, Any]], mapping: dict[str, Any]) -> tuple[list[dict], list[dict]]:
    """Build one CIK's complete historical prefix, bounded by the caller's streaming group."""
    if not rows:
        return [], []
    owner = rows[0]["cik"]
    corroborate(rows)
    for row in rows:
        row["candidate_effective_at"] = row["available_at"]
        row["candidate_clock_basis"] = row["clock_basis"]
    by_end, by_slot = _grid(rows)
    groups, mapping_dispositions = collections.defaultdict(list), []
    for row in rows:
        disposition = {"candidate_id": row["candidate_id"], "cik": owner, "item_id": row["item_id"],
                       "reason": row["source_status"], "year": row["period_end"].year if row["period_end"] else 0}
        expected = {"monetary": "USD", "quantity": "shares", "per_share": "USD/shares"}.get(row["unit_type"])
        end = row["period_end"]
        rejected_stock = (row["item_id"] is not None and row["period_kind"] == "instant"
                          and row["available_at"] is not None and row["source_status"] in
                          {"unsupported_duration", "missing_tag_context", "abstract_tag", "conflicting_sub"})
        if row["source_status"] != "candidate" and not rejected_stock:
            mapping_dispositions.append(disposition)
            continue
        if row["item_id"] is None:
            disposition["reason"] = "unmapped_concept"
            mapping_dispositions.append(disposition)
            continue
        versions = row.get("taxonomy_versions")
        if versions and row["taxonomy_version"] not in versions:
            disposition["reason"] = "unverified_taxonomy_version"
            mapping_dispositions.append(disposition)
            continue
        forced_status = (row["source_status"] if rejected_stock else
                         "unit_mismatch" if expected is None or row["unit"] != expected else None)
        if (str(end) < row["alias_valid_from"] or
                row["alias_valid_to"] and str(end) >= row["alias_valid_to"] or
                str(end) < row["rule_valid_from"] or
                row["rule_valid_to"] and str(end) >= row["rule_valid_to"]):
            disposition["reason"] = "alias_outside_validity"
            mapping_dispositions.append(disposition)
            continue
        period = _period(row, by_end, by_slot)
        if period is None:
            disposition["reason"] = "period_ambiguous"
            point = by_end.get(end)
            if row["period_kind"] != "instant" or point is None or row["available_at"] is None:
                mapping_dispositions.append(disposition)
                continue
            # The filing is visible and its stock endpoint is known. Preserve
            # the rejected observation as a NULL revision instead of presenting
            # it to downstream BE as ordinary non-reporting.
            period = {"period_start": None, "period_end": end, "freq": "instant",
                      "fiscal_year": point["fy"], "fiscal_qtr": point["q"],
                      "available_at": max(row["available_at"], point["clock"]),
                      "invalid_at": point["invalid_at"], "period_basis": "rejected_stock_context",
                      "dependencies": sorted(set(row["dependencies"]+[point["input"]]))}
            forced_status = forced_status or "period_ambiguous"
        row.update(period, owner_id=owner, item=str(row["item_id"]), unit=expected)
        raw_value = _decimal(row["value_exact"]) if forced_status is None else None
        row["forced_status"] = forced_status
        row["normalized_exact"] = None if raw_value is None else raw_value*Decimal(str(row["multiplier"]))
        if row["normalized_exact"] is not None:
            if row["sign_rule"] == "invert":
                row["normalized_exact"] *= -1
            elif row["sign_rule"] == "absolute":
                row["normalized_exact"] = abs(row["normalized_exact"])
            row["normalized_exact"] *= {"identity": 1, "thousands": 1000, "millions": 1000000}[row["scale_rule"]]
        groups[_series_key(row)].append(row)
        disposition["reason"] = forced_status or "mapped_candidate"
        mapping_dispositions.append(disposition)
    raw_series = {}
    for key, group in groups.items():
        by_clock = collections.defaultdict(list)
        for row in group:
            by_clock[row["available_at"]].append(row)
        output = []
        for clock, choices in sorted(by_clock.items()):
            precedence = max((r["filed_date"], r["accession"], -r["priority"]) for r in choices)
            tied = [r for r in choices if (r["filed_date"], r["accession"], -r["priority"]) == precedence]
            values = {r["normalized_exact"] for r in tied}
            # C-104: conflict is decided before source/candidate ID representative selection.
            representative = min(tied, key=lambda r: (r["source_kind"] != "fsds", r["candidate_id"]))
            status = "semantic_conflict" if len(values) > 1 else "valid" if next(iter(values)) is not None else "invalid_value"
            if len(values) == 1 and next(iter(values)) is None:
                status = representative.get("forced_status") or status
            invalid = representative["invalid_at"]
            if invalid is not None and invalid <= clock:
                status = "fiscal_grid_conflict"
            value = next(iter(values)) if status == "valid" else None
            inputs = list(itertools.chain.from_iterable(r["dependencies"] for r in tied))
            output.append(_event(representative, clock=clock, value=value, status=status,
                                 inputs=inputs, rule=representative["rule_id"], event_stage="source"))
            if invalid is not None and invalid > clock:
                output.append(_event(representative, clock=invalid, value=None, status="fiscal_grid_conflict",
                                     inputs=inputs, rule="fiscal_grid_invalidation", event_stage="source"))
        # Grid invalidation and a filing revision can occur at one clock. Neither
        # a hash nor list order may select the surviving economic observation.
        raw_series[key] = _coalesce_events(output, "raw:clock_reconcile")
    # Canonical seed compositions preserve raw versus output dependencies. A null
    # direct alias is authoritative; it cannot be replaced by a fallback operand.
    canonical = dict(raw_series)
    rule_map = {(r["item_id"], r["basis"]): r for r in mapping["rules"]}
    identities = sorted({key[1:] for key in raw_series}, key=str)
    done = set()

    def canonical_item(item_id, identity, visiting=()):
        key = (str(item_id), *identity)
        if key in done:
            return canonical.get(key, [])
        if item_id in visiting:
            raise ValueError("cyclic canonical rule dependency")
        freq = identity[0]
        basis = "instant" if freq == "instant" else "annual" if freq == "fy" else "quarterly"
        rule = rule_map.get((item_id, basis))
        direct = raw_series.get(key, [])
        if rule is None or not rule["source_item_ids"]:
            done.add(key)
            return direct
        inputs = []
        for source_id, kind in zip(rule["source_item_ids"], rule["input_kinds"]):
            inputs.append(canonical_item(source_id, identity, (*visiting, item_id)) if kind == "output"
                          else raw_series.get((str(source_id), *identity), []))
        active = direct or next((s for s in inputs if s), [])
        if active:
            template = {**active[0], "item": str(item_id), "source_kind": "derived"}
            signs = [1] + ([-1]*(len(inputs)-1) if "difference" in rule["combination_rule"] else [1]*(len(inputs)-1))

            def operation(current, rule=rule, signs=signs):
                if rule["combination_rule"] == "book_equity_jkp":
                    return _book_equity(current[1:])
                if current[0] is not None:
                    return _decimal(current[0]["value_exact"]), current[0]["status"]
                if rule.get("composition_refusal"):
                    return None, rule["composition_refusal"]
                # Seed zero-fill is not sufficient historical evidence of absence.
                return _arithmetic(current[1:], signs)

            canonical[key] = _merge_series([direct, *inputs], template, rule["rule_id"], operation)
        done.add(key)
        return canonical.get(key, [])

    for identity in identities:
        for item_id in sorted({r["item_id"] for r in mapping["rules"]}):
            canonical_item(item_id, identity)
    mnemonic = {}
    intermediates = []
    for name, chain in mapping["chains"].items():
        for identity in identities:
            inputs = [canonical.get((str(i), *identity), []) for i in chain["item_ids"]]
            active = next((s for s in inputs if s), [])
            if not active:
                continue
            template = {**active[0], "item": name}

            def choose(current, chain=chain):
                row = next((r for r in current if r is not None), None)
                if row is None:
                    return None, "missing_input"
                number = _decimal(row["value_exact"])
                if number is not None and chain["magnitude"]:
                    number = -number
                return number, row["status"], row.get("missing_value_basis")

            mnemonic[(name, *identity)] = _merge_series(inputs, template, "mnemonic:"+name, choose)
    intermediates.extend(itertools.chain.from_iterable(mnemonic.values()))
    # Each arithmetic operator works on explicit intervals. Incomplete operand sets
    # produce null events; annual weighted-share/EPS values are never subtracted.
    quarterly = collections.defaultdict(dict)
    for key, series in mnemonic.items():
        name, freq, start, end, fy, q = key
        quarterly[name, fy].setdefault((freq, q), []).append(series)
    for (name, fy), periods in list(quarterly.items()):
        if not mapping["chains"][name]["additive"]:
            continue
        for q in (2, 3, 4):
            left = periods.get(("fy" if q == 4 else "ytd", q), [])
            right = periods.get(("q" if q == 2 else "ytd", q-1), [])
            if len(left) != 1 or len(right) != 1:
                continue
            a, b = left[0], right[0]
            if a[0]["period_start"] != b[0]["period_start"]:
                continue
            start, end = b[0]["period_end"]+dt.timedelta(days=1), a[0]["period_end"]
            if not 70 <= (end-start).days+1 <= 112:
                continue
            template = {**a[0], "freq": "q", "period_start": start, "period_basis": "ytd_difference"}
            derived = _merge_series([a, b], template, "quarter:ytd_difference", lambda c: _arithmetic(c, [1, -1]),
                                    start_at=a[0]["available_at"])
            intermediates.extend(derived)
            key = _series_key(template)
            direct = mnemonic.get(key, [])
            if direct:
                def reconcile(current):
                    d, calculation = current
                    if d is None:
                        return (None, "missing_input") if calculation is None else (_decimal(calculation["value_exact"]), calculation["status"])
                    if d["value_exact"] is None:
                        return None, d["status"]
                    if calculation is not None and calculation["value_exact"] is not None:
                        x, y = _decimal(d["value_exact"]), _decimal(calculation["value_exact"])
                        if abs(x-y) > max(Decimal("0.0001"), abs(x)*Decimal("0.005")):
                            return None, "direct_derived_conflict"
                    return _decimal(d["value_exact"]), d["status"]
                mnemonic[key] = _merge_series([direct, derived], template, "quarter:reconciled", reconcile)
            else:
                mnemonic[key] = derived
    discrete = collections.defaultdict(list)
    for key, series in mnemonic.items():
        if key[1] == "q":
            discrete[key[0], key[4]*4+key[5]].append(series)
    for (name, slot), last in sorted(discrete.items()):
        if not mapping["chains"][name]["additive"]:
            continue
        parts = [discrete.get((name, slot-offset), []) for offset in (3, 2, 1, 0)]
        if any(len(p) != 1 for p in parts):
            continue
        series = [p[0] for p in parts]
        if any(series[i][0]["period_end"]+dt.timedelta(days=1) != series[i+1][0]["period_start"] for i in range(3)):
            continue
        template = {**series[-1][0], "freq": "ttm", "period_start": series[0][0]["period_start"],
                    "period_basis": "four_contiguous_quarters"}
        if not 350 <= (template["period_end"]-template["period_start"]).days+1 <= 380:
            continue
        mnemonic[_series_key(template)] = _merge_series(series, template, "ttm:four_quarters", lambda c: _arithmetic(c, [1, 1, 1, 1]),
                                                      start_at=series[-1][0]["available_at"])
    # An exact annual interval is a TTM at its own endpoint, never at a newer date.
    for key, series in list(mnemonic.items()):
        if key[1] != "fy" or not mapping["chains"][key[0]]["additive"]:
            continue
        template = {**series[0], "freq": "ttm", "period_basis": "exact_full_year"}
        target = _series_key(template)
        direct = [_event(template, clock=r["available_at"], value=_decimal(r["value_exact"]),
                         status=r["status"], inputs=[r["vintage_id"]], rule="ttm:exact_full_year") for r in series]
        prior = mnemonic.get(target, [])
        if prior:
            intermediates.extend(prior)
            intermediates.extend(direct)
            mnemonic[target] = _merge_series([prior, direct], template, "ttm:fy_reconciled", _ttm_choice)
        else:
            mnemonic[target] = direct
    # Multiple starts/durations for one fiscal slot are a serving ambiguity even
    # when their exact-period keys differ. Persist a NULL instead of allowing the
    # reader's provenance hash to choose an economic value.
    slots = collections.defaultdict(list)
    for key, series in mnemonic.items():
        slots[key[0], key[1], key[4], key[5]].append(series)
    for slot, series in slots.items():
        if len(series) <= 1:
            continue
        for source_series in series:
            intermediates.extend(source_series)
        template = min((s[0] for s in series), key=lambda r: (r["period_end"], r["period_start"] or dt.date.min))
        for key in [k for k in mnemonic if (k[0], k[1], k[4], k[5]) == slot]:
            del mnemonic[key]
        mnemonic[_series_key(template)] = _merge_series(series, template, "slot:duration_reconcile", _duration_choice)
    output = list(itertools.chain.from_iterable(raw_series.values()))
    output += list(itertools.chain.from_iterable(canonical.values()))
    output += intermediates
    output += list(itertools.chain.from_iterable(mnemonic.values()))
    # Keep the complete dependency graph, including intermediate raw canonical nodes.
    unique = {r["vintage_id"]: r for r in output}
    timelines = collections.defaultdict(list)
    serving_ids = {r["vintage_id"] for series in mnemonic.values() for r in series}
    for row in unique.values():
        row["is_public"] = row["vintage_id"] in serving_ids
        private_basis = None if row["is_public"] else (row["rule_id"], row["event_stage"])
        timelines[(*_series_key(row), row["is_public"], private_basis)].append(row)
    for timeline in timelines.values():
        timeline.sort(key=lambda r: (r["available_at"], r["vintage_id"]))
        for row, following in zip(timeline, timeline[1:]):
            row["valid_until"] = following["available_at"]
    decisions = collections.defaultdict(list)
    for decision in mapping_dispositions:
        decisions[decision["candidate_id"]].append(decision)
    raw = {r["candidate_id"]: r for r in rows}
    used = {dependency for row in unique.values() for dependency in row["inputs"] if dependency in raw}
    conflicting = {dependency for row in unique.values() if row["status"] == "semantic_conflict"
                   for dependency in row["inputs"] if dependency in raw}
    dispositions = []
    for candidate_id, choices in sorted(decisions.items()):
        reasons = sorted({r["reason"] for r in choices})
        reason = ("semantic_conflict" if candidate_id in conflicting else "used" if candidate_id in used
                  else "superseded_alias" if "mapped_candidate" in reasons else "+".join(reasons))
        source = raw[candidate_id]
        dispositions.append({"candidate_id": candidate_id, "cik": owner, "reason": reason,
                             "mapping_outcomes": json.dumps(sorted(choices, key=lambda r: (r["item_id"] or 0, r["reason"])), sort_keys=True),
                             "source_occurrences": source.get("source_occurrences", 1),
                             "effective_at": source["candidate_effective_at"],
                             "clock_basis": source["candidate_clock_basis"],
                             "year": source["period_end"].year if source["period_end"] else 0})
    return sorted(unique.values(), key=lambda r: (r["year"], r["item"], r["freq"], r["period_end"], r["available_at"], r["vintage_id"])), dispositions


def _coalesce_events(rows, rule):
    grouped = collections.defaultdict(list)
    for row in rows:
        grouped[row["available_at"]].append(row)
    output = []
    for clock, events in sorted(grouped.items()):
        distinct = {r["vintage_id"]: r for r in events}
        events = list(distinct.values())
        if len(events) == 1:
            output.append(events[0])
            continue
        invalid = any(r["status"] == "fiscal_grid_conflict" for r in events)
        values = {_decimal(r["value_exact"]) for r in events}
        status = "fiscal_grid_conflict" if invalid else "semantic_conflict" if len(values) > 1 else events[0]["status"]
        output.append(_event(events[0], clock=clock,
                             value=next(iter(values)) if status == "valid" else None,
                             status=status, inputs=[i for r in events for i in r["inputs"]], rule=rule,
                             event_stage=events[0]["event_stage"]))
    return output


def _duration_choice(current):
    present = [r for r in current if r is not None]
    if not present:
        return None, "missing_input"
    if len({(r["period_start"], r["period_end"]) for r in present}) > 1:
        return None, "period_duration_conflict"
    latest_clock = max(r["available_at"] for r in present)
    latest = [r for r in present if r["available_at"] == latest_clock]
    values = {_decimal(r["value_exact"]) for r in latest}
    if len(values) > 1:
        return None, "semantic_conflict"
    return next(iter(values)), latest[0]["status"]


def _ttm_choice(current):
    quarterly, annual = current
    if annual is None:
        return (None, "missing_input") if quarterly is None else (_decimal(quarterly["value_exact"]), quarterly["status"])
    if quarterly is None or quarterly["status"] == "missing_input":
        return _decimal(annual["value_exact"]), annual["status"]
    # Explicit nullifications are authoritative at and after their event; older
    # annual values cannot resurrect a subsequently nullified quarter.
    if annual["value_exact"] is None or quarterly["value_exact"] is None:
        newest = max((annual, quarterly), key=lambda r: r["available_at"])
        if annual["available_at"] == quarterly["available_at"]:
            return None, "invalid_operand"
        return _decimal(newest["value_exact"]), newest["status"]
    a, q = _decimal(annual["value_exact"]), _decimal(quarterly["value_exact"])
    if abs(a-q) > max(Decimal("0.0001"), abs(a)*Decimal("0.005")):
        return None, "annual_quarter_conflict"
    return a, "valid"


def _book_equity(current):
    stockholders, common, preferred, assets, liabilities, tax = current
    if common is not None and common["status"] == "missing_input":
        common = None  # An uncomputable derived CEQ is absence, not an observed NULL.
    convention = []
    if stockholders is not None:
        equity = _decimal(stockholders["value_exact"])
        if equity is None:
            return None, "invalid_operand"
    elif common is not None and common["value_exact"] is None:
        return None, "invalid_operand"
    elif common is not None and preferred is not None:
        equity, status = _arithmetic([common, preferred], [1, 1])
        if equity is None:
            return None, status
    else:
        equity, status = _arithmetic([assets, liabilities], [1, -1])
        if equity is None:
            return None, status
    extra = []
    for item, row in (("TXDITC", tax), ("PSTK", preferred)):
        if row is None:
            extra.append(Decimal(0))
            convention.append(item+"_ordinary_absence_zero")
        elif row["value_exact"] is None:
            return None, "invalid_operand"
        else:
            extra.append(_decimal(row["value_exact"]))
    return equity+extra[0]-extra[1], "valid", "+".join(convention) or None


def schemas():
    import pyarrow as pa
    text = pa.string()
    vintage = pa.schema([(k, text) for k in ("owner_id", "cik", "item", "freq")]
                        + [("period_start", pa.date32()), ("period_end", pa.date32()),
                           ("fiscal_year", pa.int32()), ("fiscal_qtr", pa.int32()),
                           ("value", pa.float64()), ("available_at", pa.timestamp("us")),
                           ("valid_until", pa.timestamp("us"))]
                        + [(k, text) for k in ("accession", "rule_id", "vintage_id", "source_kind",
                                              "clock_basis", "period_basis", "value_exact", "unit", "status", "lineage_id")]
                        + [("missing_value_basis", text), ("event_stage", text),
                           ("year", pa.int32()), ("is_public", pa.bool_())])
    lineage = pa.schema([(k, text) for k in ("vintage_id", "input_candidate_or_vintage_id", "input_role",
                                             "mapping_sha256", "build_sha256")] + [("year", pa.int32())])
    disposition = pa.schema([("candidate_id", text), ("cik", text), ("reason", text),
                             ("mapping_outcomes", text), ("source_occurrences", pa.int64()), ("year", pa.int32())])
    disposition = disposition.append(pa.field("effective_at", pa.timestamp("us"))).append(pa.field("clock_basis", text))
    return {"item_vintages": vintage, "item_lineage": lineage, "item_dispositions": disposition}


def iter_owner_candidates(files: Sequence[Path | str], mapping: dict, *, root: Path):
    import pyarrow as pa
    from .research_lake import connect_bounded
    con = connect_bounded(None, root=root, memory_limit="256MB", threads=1)
    alias_rows = []
    for alias in mapping["aliases"]:
        alias_rows.append({**alias, "taxonomy_versions": alias.get("taxonomy_versions", []),
                           "alias_valid_from": alias["valid_from"], "alias_valid_to": alias["valid_to"]})
    aliases = pa.Table.from_pylist(alias_rows)
    con.register("item_aliases", aliases)
    try:
        query = f"""WITH raw_counted AS (
          SELECT *,count(*) OVER(PARTITION BY candidate_id) source_occurrences,
                 row_number() OVER(PARTITION BY candidate_id ORDER BY candidate_id) occurrence
          FROM {parquet_relation(list(files))}), raw AS (
          SELECT * EXCLUDE(occurrence) FROM raw_counted WHERE occurrence=1)
          SELECT r.*,a.item_id,a.priority,a.unit_type,a.period_kind,a.multiplier,a.rule_id,
             a.alias_valid_from,a.alias_valid_to,a.rule_valid_from,a.rule_valid_to,a.sign_rule,a.scale_rule,a.taxonomy_versions
          FROM raw r LEFT JOIN item_aliases a
            ON r.taxonomy=a.taxonomy AND r.concept=a.concept
           AND (a.basis=CASE WHEN r.qtrs=0 THEN 'instant' WHEN r.qtrs=4 THEN 'annual' ELSE 'quarterly' END
                OR a.basis='instant' AND a.period_kind='instant')
          ORDER BY r.cik NULLS FIRST,r.available_at,r.candidate_id,a.item_id"""
        sentinel = object()
        current, owner_rows = sentinel, []
        for batch in con.execute(query).to_arrow_reader(batch_size=4096):
            for row in batch.to_pylist():
                if current is not sentinel and row["cik"] != current:
                    yield owner_rows
                    owner_rows = []
                current = row["cik"]
                owner_rows.append(row)
        if owner_rows:
            yield owner_rows
    finally:
        con.close()


def validate_original_plan(plan: dict, *, output: Path, root: Path) -> dict:
    """Bind storage operations to the actual immutable computational plan."""
    path = output.parent / "plan.json"
    original = json.loads(path.read_text())
    payload = {k: v for k, v in original.items() if k not in {"build_sha256", "build_id"}}
    digest = canonical_sha256(payload)
    if (original != plan or original["build_sha256"] != digest or
            original["build_id"] != "fund-"+digest[:20]):
        raise ValueError("original computational plan content/identity differs")
    if (output.parent.resolve() != (Path(original["work"]) / original["build_id"]).resolve() or
            root.resolve() != Path(original["root"]).resolve()):
        raise ValueError("storage operation is outside original plan locations")
    return {"path": path.resolve().as_posix(), "sha256": file_sha256(path),
            "payload_sha256": canonical_sha256(original)}


def bucket_materializations(output: Path, *, root: Path, prefer_sealed: bool = False) -> list[dict]:
    """Resolve pinned originals or a separately verified immutable relocation.

    ``prefer_sealed`` measures the destination reader without removing originals.
    A present corrupt original always fails, including in this diagnostic mode.
    """
    from .fundamental_sources import check_file
    from .research_lake import verify_lake_snapshot
    complete_path = output / "complete.json"
    complete = json.loads(complete_path.read_text())
    missing = []
    for entry in complete["files"]:
        if Path(entry["path"]).exists():
            check_file(entry)
        else:
            missing.append(entry)
    if not missing and not prefer_sealed:
        return complete["files"]
    path = output / "storage-relocation-v2.json"
    if not path.is_file():
        raise ValueError("missing item intermediates without verified relocation receipt")
    proof = json.loads(path.read_text())
    digest = proof.pop("receipt_sha256", None)
    if digest != canonical_sha256(proof) or proof.get("schema") != "fundamental_item_relocation_v2":
        raise ValueError("invalid item relocation receipt")
    plan = json.loads((output.parent / "plan.json").read_text())
    if proof["original_plan"] != validate_original_plan(plan, output=output, root=root):
        raise ValueError("relocation original computational plan drift")
    if (proof["build_sha256"] != complete["build_sha256"] or
            proof["mapping_sha256"] != complete["mapping_sha256"] or
            proof["original_files"] != complete["files"] or
            proof["build_sha256"] != plan["build_sha256"] or
            proof["mapping_sha256"] != plan["mapping"]["sha256"] or
            proof["bucket"] != complete["bucket"] or output.name != f"b{complete['bucket']:03d}"):
        raise ValueError("relocation does not bind original bucket contents")
    if set(proof["original_receipts"]) != {"complete.json", "audit.json", "published.json"}:
        raise ValueError("relocation original receipt scope is incomplete")
    for name, expected_sha in proof["original_receipts"].items():
        if file_sha256(output / name) != expected_sha:
            raise ValueError("relocation original receipt drift")
    published = json.loads((output / "published.json").read_text())
    if proof["snapshots"] != published["slices"]:
        raise ValueError("relocation snapshot scope differs from original publication")
    expected_targets = []
    for snapshot in proof["snapshots"]:
        if file_sha256(snapshot["manifest_path"]) != snapshot["manifest_sha256"]:
            raise ValueError("relocated lake manifest drift")
        if not verify_lake_snapshot(snapshot["snapshot_id"], root=root)["ok"]:
            raise ValueError("relocated lake snapshot verification failed")
        manifest_path = Path(snapshot["manifest_path"])
        manifest = json.loads(manifest_path.read_text())
        for dataset, description in manifest["datasets"].items():
            expected_targets.extend({**entry, "path": (manifest_path.parent / entry["path"]).resolve().as_posix(),
                                      "dataset": dataset, "year": snapshot["year"]}
                                     for entry in description["files"])
    if expected_targets != proof["destination_files"]:
        raise ValueError("relocation destination files differ from sealed manifests")
    for entry in proof["destination_files"]:
        check_file(entry)
    # Select one representation per dataset/year. Never concatenate equivalent
    # original and sealed files, which would duplicate all rows.
    groups = collections.defaultdict(list)
    for entry in complete["files"]:
        groups[entry["dataset"], entry["year"]].append(entry)
    comparisons = {(r["dataset"], r["year"]): r for r in proof["comparisons"]}
    if len(comparisons) != len(proof["comparisons"]) or set(comparisons) != set(groups):
        raise ValueError("relocation semantic proof scope differs")
    for key, entries in groups.items():
        comparison = comparisons[key]
        if (comparison["rows"] != sum(e["rows"] for e in entries) or
                comparison["missing_rows"] != 0 or comparison["extra_rows"] != 0):
            raise ValueError("relocation semantic comparison was not successful")
    resolved = []
    for key, entries in groups.items():
        if not prefer_sealed and all(Path(e["path"]).is_file() for e in entries):
            resolved.extend(entries)
        else:
            targets = [e for e in proof["destination_files"] if (e["dataset"], e["year"]) == key]
            if not targets or sum(e["rows"] for e in targets) != sum(e["rows"] for e in entries):
                raise ValueError("relocation dataset/year row denominator mismatch")
            resolved.extend(targets)
    return resolved


def prepare_bucket_relocation(plan: dict, *, output: Path, root: Path) -> dict:
    """Prove sealed/original equality and pin a no-delete storage transition.

    The computational plan stays frozen. This independent storage operation pins
    its own implementation and never changes computational acceptance or removes
    files. Actual release is deliberately not implemented here.
    """
    import shutil
    import time
    from .fundamental_sources import atomic_json, check_file
    from .research_lake import connect_bounded, snapshot_path, verify_lake_snapshot
    original_plan = validate_original_plan(plan, output=output, root=root)
    target = output / "storage-relocation-v2.json"
    if target.is_file():
        bucket_materializations(output, root=root, prefer_sealed=True)
        return json.loads(target.read_text())
    originals = {name: json.loads((output / name).read_text()) for name in
                 ("complete.json", "audit.json", "published.json")}
    complete, audit, published = (originals[n] for n in ("complete.json", "audit.json", "published.json"))
    pins = {name: file_sha256(output / name) for name in originals}
    if (any(record["build_sha256"] != plan["build_sha256"] for record in originals.values()) or
            complete["mapping_sha256"] != plan["mapping"]["sha256"] or not audit["structural_ok"] or
            audit["failures"] or audit["complete_sha256"] != pins["complete.json"] or
            published["audit_sha256"] != pins["audit.json"]):
        raise ValueError("only explicitly audited published bucket contents can relocate")
    by_group = collections.defaultdict(list)
    for entry in complete["files"]:
        source = check_file(entry).resolve()
        if source.parent != output.resolve() or source.suffix != ".parquet" or Path(entry["path"]).is_symlink():
            raise ValueError("item intermediate is outside its exact owned bucket directory")
        by_group[entry["dataset"], entry["year"]].append(entry)
    destinations, snapshots = [], []
    for snapshot in published["slices"]:
        if file_sha256(snapshot["manifest_path"]) != snapshot["manifest_sha256"]:
            raise ValueError("published manifest drift before relocation")
        verified = verify_lake_snapshot(snapshot["snapshot_id"], root=root)
        if not verified["ok"]:
            raise ValueError("published snapshot failed relocation verification")
        manifest = json.loads(Path(snapshot["manifest_path"]).read_text())
        meta = manifest["source_meta"]
        if (meta["build_sha256"] != plan["build_sha256"] or
                meta["mapping_sha256"] != plan["mapping"]["sha256"] or
                meta["bucket_complete_sha256"] != pins["complete.json"] or
                meta["audit_sha256"] != pins["audit.json"] or meta["bucket"] != complete["bucket"] or
                meta["year"] != snapshot["year"]):
            raise ValueError("published content belongs to a different bucket/audit")
        location = snapshot_path(snapshot["snapshot_id"], root)
        if Path(snapshot["manifest_path"]).resolve().parent != location.resolve():
            raise ValueError("published manifest is outside its declared snapshot")
        for dataset, description in manifest["datasets"].items():
            for entry in description["files"]:
                path = (location / entry["path"]).resolve()
                if not path.is_relative_to(location.resolve()):
                    raise ValueError("relocation destination escaped sealed snapshot")
                destinations.append({**entry, "path": path.as_posix(), "dataset": dataset,
                                     "year": snapshot["year"]})
        snapshots.append(snapshot)
    destination_groups = {(e["dataset"], e["year"]) for e in destinations}
    if destination_groups != set(by_group):
        raise ValueError("relocation destination dataset/year scope differs")
    comparisons = []
    for (dataset, year), entries in sorted(by_group.items()):
        targets = [e for e in destinations if e["dataset"] == dataset and e["year"] == year]
        rows = sum(e["rows"] for e in entries)
        if rows > 2_000_000:
            raise ValueError("relocation slice exceeds two-million-row bound; subdivide first")
        if rows != sum(e["rows"] for e in targets):
            raise ValueError("relocation row denominator differs")
        # EXCEPT ALL may spill. Reserve both file-copy and decompressed-row scale
        # in addition to the standing floor, before each bounded comparison.
        transient = max(4*sum(e["bytes"] for e in entries), rows*256) + 128*1024**2
        if shutil.disk_usage(output).free < 35*1024**3 + transient:
            raise ValueError("insufficient disk headroom for bounded relocation comparison")
        con = connect_bounded(None, root=root, memory_limit="256MB", threads=1)
        start = time.perf_counter()
        try:
            source = parquet_relation([check_file(e) for e in entries])
            destination = parquet_relation([check_file(e) for e in targets])
            columns = con.execute(f"DESCRIBE SELECT * FROM {source}").fetchall()
            other = {row[0]: row[1] for row in con.execute(f"DESCRIBE SELECT * FROM {destination}").fetchall()}
            if set(other) != {row[0] for row in columns}:
                raise ValueError("relocation column scope differs")
            select = []
            for name, kind, *_ in columns:
                if kind != other[name] and not (name == "year" and {kind, other[name]} <= {"INTEGER", "BIGINT"}):
                    raise ValueError("relocation column type differs beyond hive-year representation")
                quoted = '"'+name.replace('"', '""')+'"'
                select.append(f"CAST({quoted} AS BIGINT) AS {quoted}" if name == "year" else quoted)
            projection = ",".join(select)
            left, right = (f"SELECT {projection} FROM {relation}" for relation in (source, destination))
            missing = con.execute(f"SELECT count(*) FROM ({left} EXCEPT ALL {right})").fetchone()[0]
            extra = con.execute(f"SELECT count(*) FROM ({right} EXCEPT ALL {left})").fetchone()[0]
            if missing or extra:
                raise ValueError(f"relocation content differs: missing={missing}, extra={extra}")
            comparisons.append({"dataset": dataset, "year": year, "rows": rows,
                                "missing_rows": missing, "extra_rows": extra,
                                "seconds": round(time.perf_counter()-start, 3)})
        finally:
            con.close()
    proof = {"schema": "fundamental_item_relocation_v2", "build_sha256": plan["build_sha256"],
             "mapping_sha256": plan["mapping"]["sha256"], "bucket": complete["bucket"],
             "original_plan": original_plan,
             "storage_code_sha256": file_sha256(__file__), "original_receipts": pins,
             "original_files": complete["files"], "destination_files": destinations,
             "snapshots": snapshots, "comparisons": comparisons, "originals_removed": False,
             "acceptance_changed": False}
    proof["receipt_sha256"] = canonical_sha256(proof)
    atomic_json(target, proof)
    bucket_materializations(output, root=root, prefer_sealed=True)
    return proof


def verify_bucket_storage(plan: dict, *, output: Path, root: Path) -> dict:
    """Verify a completed bucket without rerunning its historical computation.

    This explicit storage-only route can run under changed code. It requires a
    v2 equality proof and never writes an audit, content, or publication index.
    """
    original_plan = validate_original_plan(plan, output=output, root=root)
    entries = bucket_materializations(output, root=root, prefer_sealed=True)
    path = output / "storage-relocation-v2.json"
    proof = json.loads(path.read_text())
    return {"schema": "fundamental_completed_storage_verification_v1",
            "build_sha256": plan["build_sha256"], "bucket": proof["bucket"],
            "original_plan": original_plan, "original_receipts": proof["original_receipts"],
            "relocation_sha256": file_sha256(path), "verifier_code_sha256": file_sha256(__file__),
            "sealed_files": len(entries), "sealed_rows": sum(e["rows"] for e in entries),
            "computation_repeated": False, "audit_recomputed": False,
            "published_new_content": False, "acceptance_changed": False}


def build_bucket(files: Sequence[Path | str], mapping: dict, *, root: Path, output: Path,
                 build_sha256: str, bucket: int) -> dict:
    """One complete owner bucket, flushed by fiscal year. No whole-history materialization."""
    import os
    import time
    import pyarrow as pa
    import pyarrow.parquet as pq
    from .fundamental_sources import atomic_json, check_file
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / "complete.json"
    if receipt.exists():
        prior = json.loads(receipt.read_text())
        if prior["build_sha256"] != build_sha256:
            raise ValueError("item bucket build/code drift")
        bucket_materializations(output, root=root)
        return {**prior, "resumed": True}
    start = time.perf_counter()
    writers, paths, counts, inventory = {}, {}, collections.Counter(), collections.Counter()
    schema = schemas()
    owners = max_owner_rows = max_owner_vintages = 0
    owner_ids = []

    def write(dataset, rows):
        by_year = collections.defaultdict(list)
        for row in rows:
            by_year[row["year"]].append(row)
        for year, block in by_year.items():
            key = dataset, year
            if key not in writers:
                path = output / f"{dataset}-{year}.parquet"
                paths[key] = path
                writers[key] = pq.ParquetWriter(path.with_suffix(".pending"), schema[dataset], compression="zstd")
            for offset in range(0, len(block), 4096):
                table = pa.Table.from_pylist(block[offset:offset+4096], schema=schema[dataset])
                writers[key].write_table(table, row_group_size=4096)
            counts[key] += len(block)

    try:
        for rows in iter_owner_candidates(files, mapping, root=root):
            owners += 1
            if rows[0]["cik"] is not None:
                owner_ids.append(rows[0]["cik"])
            max_owner_rows = max(max_owner_rows, len(rows))
            vintages, dispositions = build_owner(rows, mapping)
            max_owner_vintages = max(max_owner_vintages, len(vintages))
            lineage = []
            for row in vintages:
                for dependency in row["inputs"]:
                    lineage.append({"vintage_id": row["vintage_id"], "input_candidate_or_vintage_id": dependency,
                                    "input_role": "operand_or_context", "mapping_sha256": mapping["sha256"],
                                    "build_sha256": build_sha256, "year": row["year"]})
                if row["is_public"]:
                    inventory[row["item"], row["freq"], row["year"], row["status"]] += 1
            write("item_vintages", vintages)
            write("item_lineage", lineage)
            write("item_dispositions", dispositions)
        for key, writer in writers.items():
            writer.close()
            os.replace(paths[key].with_suffix(".pending"), paths[key])
        writers.clear()
    finally:
        for writer in writers.values():
            writer.close()
    records = [{"dataset": dataset, "year": year, "path": path.as_posix(), "rows": counts[dataset, year],
                "sha256": file_sha256(path), "bytes": path.stat().st_size}
               for (dataset, year), path in sorted(paths.items())]
    result = {"schema": ENGINE_VERSION, "build_sha256": build_sha256, "mapping_sha256": mapping["sha256"],
              "bucket": bucket, "owners": owners, "max_owner_candidates": max_owner_rows,
              "owner_ids": sorted(set(owner_ids)),
              "max_owner_vintages": max_owner_vintages, "files": records,
              "rows": {dataset: sum(n for (name, _), n in counts.items() if name == dataset) for dataset in schema},
              "coverage": [{"item": k[0], "freq": k[1], "year": k[2], "status": k[3], "rows": n}
                           for k, n in sorted(inventory.items())], "seconds": round(time.perf_counter()-start, 3)}
    atomic_json(receipt, result)
    return result


def audit_bucket(files, plan, *, output: Path, root: Path) -> dict:
    """Read-only bounded structural/clock/numeric evidence; never grants acceptance.

    Final acceptance additionally needs the separately reviewed issuer/raw oracle,
    identity/coverage and real kill-resume gates. Diagnostic scope stays explicit.
    """
    from .fundamental_sources import atomic_json, check_file
    from .research_lake import connect_bounded
    receipt = json.loads((output / "complete.json").read_text())
    if receipt["build_sha256"] != plan["build_sha256"]:
        raise ValueError("audit build drift")
    grouped = collections.defaultdict(list)
    for entry in bucket_materializations(output, root=root):
        grouped[entry["dataset"]].append(check_file(entry))
    con = connect_bounded(None, root=root, memory_limit="256MB", threads=1)
    checks = {}
    try:
        con.execute(f"CREATE VIEW raw AS SELECT * FROM {parquet_relation(list(files))}")
        for alias, dataset in (("v", "item_vintages"), ("l", "item_lineage"), ("d", "item_dispositions")):
            con.execute(f"CREATE VIEW {alias} AS SELECT * FROM {parquet_relation(grouped[dataset])}")
        queries = {
            "raw_rows": "SELECT count(*) FROM raw",
            "raw_candidates": "SELECT count(DISTINCT candidate_id) FROM raw",
            "dispositions": "SELECT count(*) FROM d",
            "disposition_occurrences": "SELECT coalesce(sum(source_occurrences),0) FROM d",
            "duplicate_dispositions": "SELECT count(*)-count(DISTINCT candidate_id) FROM d",
            "unaccounted_candidates": "SELECT count(*) FROM (SELECT DISTINCT candidate_id FROM raw EXCEPT SELECT candidate_id FROM d)",
            "unknown_dispositions": "SELECT count(*) FROM (SELECT candidate_id FROM d EXCEPT SELECT candidate_id FROM raw)",
            "duplicate_vintages": "SELECT count(*)-count(DISTINCT vintage_id) FROM v",
            "public_same_clock": "SELECT count(*) FROM (SELECT owner_id,item,freq,fiscal_year,fiscal_qtr,available_at FROM v WHERE is_public GROUP BY ALL HAVING count(*)>1)",
            "invalid_intervals": "SELECT count(*) FROM v WHERE available_at IS NULL OR (valid_until IS NOT NULL AND valid_until<=available_at)",
            "future_vintages": "SELECT count(*) FROM v WHERE available_at>=TIMESTAMP '2026-09-21' OR period_end>DATE '2026-09-20'",
            "future_period_anchors": "SELECT count(*) FROM v WHERE period_end>available_at::DATE",
            "value_status_mismatch": "SELECT count(*) FROM v WHERE (status='valid')<>(value IS NOT NULL)",
            "orphan_parents": "SELECT count(*) FROM l LEFT JOIN v USING(vintage_id) WHERE v.vintage_id IS NULL",
            "orphan_dependencies": "SELECT count(*) FROM l LEFT JOIN v dep ON l.input_candidate_or_vintage_id=dep.vintage_id LEFT JOIN d ON l.input_candidate_or_vintage_id=d.candidate_id WHERE dep.vintage_id IS NULL AND d.candidate_id IS NULL",
            "future_dependencies": "SELECT count(*) FROM l JOIN v parent ON l.vintage_id=parent.vintage_id LEFT JOIN v dep ON l.input_candidate_or_vintage_id=dep.vintage_id LEFT JOIN d ON l.input_candidate_or_vintage_id=d.candidate_id WHERE coalesce(dep.available_at,d.effective_at)>parent.available_at",
            "empty_lineage": "SELECT count(*) FROM v LEFT JOIN l USING(vintage_id) WHERE l.vintage_id IS NULL",
            "overlap_or_gap": "SELECT count(*) FROM (SELECT *,lead(available_at) OVER(PARTITION BY owner_id,item,freq,period_start,period_end,fiscal_year,fiscal_qtr ORDER BY available_at) next_at FROM v WHERE is_public) WHERE valid_until IS DISTINCT FROM next_at",
            "nonadditive_derived": "SELECT count(*) FROM v WHERE item IN ('WAB','WAD','EPSPI','EPSFI','CSHO') AND (freq='ttm' OR rule_id='quarter:ytd_difference')",
        }
        for name, sql in queries.items():
            checks[name] = con.execute(sql).fetchone()[0]
        # Direct raw-item arithmetic tie-out is independent of the builder's
        # selected value, using persisted candidate lineage and pinned aliases.
        import pyarrow as pa
        con.register("aliases", pa.Table.from_pylist(plan["mapping"]["aliases"]))
        checks["unclocked_BE_components"] = con.execute("""SELECT count(DISTINCT r.candidate_id)
             FROM raw r JOIN aliases a ON r.taxonomy=a.taxonomy AND r.concept=a.concept
             WHERE a.item_id IN (1211,1214,1220,1221) AND r.source_status='missing_clock'
               AND r.period_end>=a.valid_from::DATE AND (a.valid_to IS NULL OR r.period_end<a.valid_to::DATE)""").fetchone()[0]
        checks["unresolved_BE_rejections"] = con.execute("""SELECT count(DISTINCT r.candidate_id)
          FROM raw r JOIN d USING(candidate_id)
          JOIN aliases a ON r.taxonomy=a.taxonomy AND r.concept=a.concept
          WHERE a.item_id IN (1211,1214,1220,1221) AND a.basis='instant' AND r.cik IS NOT NULL
            AND (r.period_end IS NULL OR (r.period_end>=a.valid_from::DATE
                 AND (a.valid_to IS NULL OR r.period_end<a.valid_to::DATE)))
            AND (r.source_status IN ('missing_clock','missing_period','missing_sub','conflicting_sub',
                                      'missing_tag_context','abstract_tag','unsupported_duration')
                 OR EXISTS(SELECT 1 FROM json_each(d.mapping_outcomes) outcome
                      WHERE json_extract_string(outcome.value,'$.reason')='period_ambiguous'
                        AND try_cast(json_extract_string(outcome.value,'$.item_id') AS INTEGER)=a.item_id))
            AND NOT EXISTS(SELECT 1 FROM l JOIN v USING(vintage_id)
                  WHERE l.input_candidate_or_vintage_id=r.candidate_id AND try_cast(v.item AS INTEGER)=a.item_id
                    AND v.value IS NULL AND v.event_stage='source')""").fetchone()[0]
        excluded_unowned = con.execute("""SELECT source_status,count(DISTINCT candidate_id)
            FROM raw WHERE cik IS NULL GROUP BY source_status ORDER BY source_status""").fetchall()
        numeric = con.execute("""WITH comparisons AS (
          SELECT v.vintage_id,v.value,
             r.value*a.multiplier*
             CASE a.sign_rule WHEN 'invert' THEN -1 ELSE 1 END *
             CASE a.scale_rule WHEN 'thousands' THEN 1000 WHEN 'millions' THEN 1000000 ELSE 1 END expected,
             a.sign_rule FROM v JOIN l USING(vintage_id)
          JOIN raw r ON r.candidate_id=l.input_candidate_or_vintage_id
          JOIN aliases a ON a.item_id=try_cast(v.item AS INTEGER)
            AND a.taxonomy=r.taxonomy AND a.concept=r.concept AND a.rule_id=v.rule_id
          WHERE v.status='valid' AND v.source_kind IN ('fsds','cf')),
          scored AS (SELECT vintage_id,bool_or(abs(value-CASE WHEN sign_rule='absolute' THEN abs(expected) ELSE expected END)
               <=greatest(0.0001,abs(expected)*0.005)) agrees FROM comparisons GROUP BY vintage_id)
          SELECT count(*),count(*) FILTER(WHERE agrees) FROM scored""").fetchone()
        checks["direct_numeric_cells"], checks["direct_numeric_agree"] = numeric
        reasons = dict(con.execute("SELECT reason,count(*) FROM d GROUP BY reason ORDER BY reason").fetchall())
        counts = dict(con.execute("SELECT status,count(*) FROM v WHERE is_public GROUP BY status ORDER BY status").fetchall())
    finally:
        con.close()
    nonerrors = {"raw_rows", "raw_candidates", "dispositions", "disposition_occurrences",
                 "direct_numeric_cells", "direct_numeric_agree"}
    failures = {name: value for name, value in checks.items() if name not in nonerrors and value}
    if checks["raw_rows"] != checks["disposition_occurrences"]:
        failures["raw_occurrence_reconciliation"] = True
    if checks["raw_candidates"] != checks["dispositions"]:
        failures["raw_candidate_reconciliation"] = True
    result = {"schema": "fundamental_bucket_audit_v1", "build_sha256": plan["build_sha256"],
              "bucket": receipt["bucket"], "checks": checks, "failures": failures,
              "structural_ok": not failures, "accepted": False,
              "source_scope_complete": plan["scope_complete"], "disposition_reasons": reasons,
              "unowned_source_quarantines": dict(excluded_unowned),
              "public_statuses": counts, "complete_sha256": file_sha256(output / "complete.json"),
              "remaining_acceptance": ["10000-cell mapping/numeric oracle", "A=L+E >=99%",
                  "50 issuer fiscal cases", "50 owners x12 raw-PIT oracle", "2013..2023 exhaustive formation lineage",
                  "real kill/resume equality", "accepted identity projection", ">=3000 owners/month"]}
    atomic_json(output / "audit.json", result)
    if failures:
        raise ValueError(f"item audit failures: {failures}")
    return result


def publish_bucket(files, plan, *, output: Path, root: Path) -> dict:
    """Seal bounded bucket/year lake slices only after checked structural evidence."""
    from .fundamental_sources import atomic_json, check_file
    from .research_lake import MANIFEST_NAME, export_lake_snapshot, snapshot_path, verify_lake_snapshot
    receipt = json.loads((output / "complete.json").read_text())
    audit_path = output / "audit.json"
    if not audit_path.exists():
        raise ValueError("explicit audit is required before publication")
    audit = json.loads(audit_path.read_text())
    if (not audit["structural_ok"] or audit["build_sha256"] != plan["build_sha256"] or
            audit["complete_sha256"] != file_sha256(output / "complete.json")):
        raise ValueError("item audit is stale or failed")
    by_year = collections.defaultdict(lambda: collections.defaultdict(list))
    for entry in bucket_materializations(output, root=root):
        path = check_file(entry)
        by_year[entry["year"]][entry["dataset"]].append(path)
    slices = []
    for year, paths in sorted(by_year.items()):
        datasets = {name: f"SELECT * FROM {parquet_relation(files)}" for name, files in paths.items()}
        # Raw candidates are retained in their source files and explicitly indexed
        # in the top manifest; each terminal disposition carries their exact IDs.
        snapshot_id = f"{plan['build_id']}-b{receipt['bucket']:03d}-y{year:04d}"
        location = snapshot_path(snapshot_id, root)
        source_meta = {"build_sha256": plan["build_sha256"], "mapping_sha256": plan["mapping"]["sha256"],
                       "bucket": receipt["bucket"], "year": year,
                       "bucket_complete_sha256": file_sha256(output / "complete.json"),
                       "audit_sha256": file_sha256(audit_path)}
        if location.exists():
            existing = json.loads((location / MANIFEST_NAME).read_text())
            if existing["source_meta"] != source_meta:
                raise ValueError("existing sealed item slice has different provenance")
        else:
            export_lake_snapshot(None, snapshot_id, datasets, root=root, memory_limit="256MB", threads=1,
                                 source_meta=source_meta)
        verification = verify_lake_snapshot(snapshot_id, root=root)
        if not verification["ok"]:
            raise ValueError(f"sealed item slice corrupt: {verification['problems']}")
        manifest_path = location / MANIFEST_NAME
        slices.append({"bucket": receipt["bucket"], "year": year, "snapshot_id": snapshot_id,
                       "manifest_path": manifest_path.resolve().as_posix(), "manifest_sha256": file_sha256(manifest_path),
                       "datasets": sorted(datasets)})
    result = {"build_sha256": plan["build_sha256"], "bucket": receipt["bucket"], "slices": slices,
              "owner_ids": receipt["owner_ids"], "audit_sha256": file_sha256(audit_path)}
    atomic_json(output / "published.json", result)
    return result


def publish_index(plan, work: Path, *, root: Path, diagnostic_buckets=None) -> dict:
    from .fundamental_sources import atomic_json
    slices, owners, audits = [], set(), []
    included = list(range(plan["buckets"])) if diagnostic_buckets is None else sorted(set(diagnostic_buckets))
    if diagnostic_buckets is not None and not plan["diagnostic"]:
        raise ValueError("partial bucket index requires an explicit diagnostic plan")
    for bucket in included:
        path = work / f"b{bucket:03d}" / "published.json"
        published = json.loads(path.read_text())
        if published["build_sha256"] != plan["build_sha256"] or published["bucket"] != bucket:
            raise ValueError("published bucket identity mismatch")
        slices.extend(published["slices"])
        owners.update(published["owner_ids"])
        audits.append({"bucket": bucket, "sha256": published["audit_sha256"]})
    manifest = {"schema": "fundamental_build_index_v1", "build_id": plan["build_id"],
                "build_sha256": plan["build_sha256"], "accepted": False,
                "diagnostic": plan["diagnostic"], "scope_complete": plan["scope_complete"] and len(included) == plan["buckets"],
                "published_buckets": included,
                "snapshot_date": plan["snapshot_date"], "mapping": plan["mapping"], "code": plan["code"],
                "source_receipts": plan["source_receipts"], "period_policy": plan["period_policy"],
                "buckets": plan["buckets"], "owner_ids": sorted(owners), "slices": slices, "audits": audits}
    name = "manifest.json" if diagnostic_buckets is None else "diagnostic-"+"-".join(f"b{b:03d}" for b in included)+".json"
    target = root / "fundamentals" / plan["build_id"] / name
    if target.exists():
        if json.loads(target.read_text()) != manifest:
            raise ValueError("immutable item index already exists with different content")
    else:
        atomic_json(target, manifest)
    return {"manifest": target.as_posix(), "sha256": file_sha256(target), "slices": len(slices),
            "owners": len(owners), "accepted": False}


class ItemVintageStore:
    """Explicit immutable lake-index reader; diagnostic data needs an explicit opt-in."""
    def __init__(self, root: Path | str, build_manifest: Path | str, *, allow_diagnostic: bool = False):
        from .research_lake import lake_files, verify_lake_snapshot
        self.root, self.path = Path(root), Path(build_manifest)
        self.manifest = json.loads(self.path.read_text(encoding="utf-8"))
        if self.manifest.get("schema") != "fundamental_build_index_v1":
            raise ValueError("not a completed fundamental lake index")
        if not self.manifest.get("accepted") and not allow_diagnostic:
            raise ValueError("item build has not passed all acceptance gates; diagnostic opt-in required")
        self.files = collections.defaultdict(list)
        for entry in self.manifest["slices"]:
            if file_sha256(entry["manifest_path"]) != entry["manifest_sha256"]:
                raise ValueError("item slice manifest changed")
            verification = verify_lake_snapshot(entry["snapshot_id"], root=self.root)
            if not verification["ok"]:
                raise ValueError(f"item snapshot failed verification: {verification['problems']}")
            if "item_vintages" in entry["datasets"]:
                self.files[entry["bucket"]].extend(lake_files(entry["snapshot_id"], "item_vintages", root=self.root))

    def items_asof(self, formations: Sequence[dt.date], items: Sequence[str],
                   lags: Mapping[str, Sequence[int]]) -> Iterator[Any]:
        import pyarrow as pa
        from atx_db.calendar import expected_month_end_session, decision_cutoff_utc
        from .research_lake import connect_bounded, sql_text
        unknown = set(items)-set(self.manifest["mapping"]["chains"])
        if unknown or any(lag < 0 for item in items for lag in lags.get(item, [0])):
            raise ValueError(f"unknown items or negative fiscal lags: {unknown}")
        frequencies = ("instant", "q", "ytd", "fy", "ttm")
        fields = [("eom", pa.date32()), ("owner_id", pa.string())]
        for item in items:
            for freq in frequencies:
                for lag in lags.get(item, [0]):
                    name = f"{item}_{freq}_l{lag}"
                    fields += [(name, pa.float64()), (name+"_reason", pa.string()),
                               (name+"_available_at", pa.timestamp("us"))]
        fields.append(("max_input_clock", pa.timestamp("us")))
        schema = pa.schema(fields)
        owners = self.manifest["owner_ids"]
        for formation in sorted(formations):
            cutoff = _clock(decision_cutoff_utc(expected_month_end_session(formation.year, formation.month)))
            for bucket, files in sorted(self.files.items()):
                con = connect_bounded(None, root=self.root, memory_limit="256MB", threads=1)
                try:
                    candidates = con.execute(f"""SELECT * EXCLUDE(pick) FROM (
                       SELECT *,row_number() OVER(PARTITION BY owner_id,item,freq,fiscal_year,fiscal_qtr
                         ORDER BY available_at DESC,period_end DESC,vintage_id DESC) pick
                       FROM {parquet_relation(files)} WHERE is_public AND available_at<=?
                         AND item IN ({','.join(sql_text(i) for i in items)})) WHERE pick=1
                       ORDER BY owner_id,item,freq,period_end""", [cutoff]).to_arrow_reader(batch_size=4096)
                    values = collections.defaultdict(dict)
                    anchors = {}
                    for batch in candidates:
                        for row in batch.to_pylist():
                            key = row["owner_id"], row["item"], row["freq"]
                            slot = row["fiscal_year"]*4+row["fiscal_qtr"]
                            values[key][slot] = row
                            if key not in anchors or row["period_end"] > anchors[key]["period_end"]:
                                anchors[key] = row
                finally:
                    con.close()
                output = []
                for owner in owners:
                    if int(owner) % self.manifest["buckets"] != bucket:
                        continue
                    result = {"eom": formation, "owner_id": owner, "max_input_clock": None}
                    clocks = []
                    for item in items:
                        for freq in frequencies:
                            key = owner, item, freq
                            anchor = anchors.get(key)
                            age_limit = 400 if freq == "fy" or anchor and anchor["period_basis"] == "exact_full_year" else 200
                            stale = anchor is not None and (cutoff.date()-anchor["period_end"]).days > age_limit
                            for lag in lags.get(item, [0]):
                                name = f"{item}_{freq}_l{lag}"
                                selected = None if anchor is None else values[key].get(anchor["fiscal_year"]*4+anchor["fiscal_qtr"]-lag)
                                result[name] = None
                                result[name+"_reason"] = "stale_input" if stale else "missing_input"
                                result[name+"_available_at"] = None
                                if selected is not None:
                                    clocks.append(selected["available_at"])
                                    result[name+"_available_at"] = selected["available_at"]
                                    if not stale:
                                        result[name] = selected["value"]
                                        result[name+"_reason"] = selected["status"]
                    result["max_input_clock"] = max(clocks) if clocks else None
                    output.append(result)
                if output:
                    yield pa.RecordBatch.from_pylist(output, schema=schema)


def items_asof(formations, items, lags, *, store: ItemVintageStore):
    """2.9 compatibility entry point; the caller must supply an explicitly pinned store."""
    yield from store.items_asof(formations, items, lags)
