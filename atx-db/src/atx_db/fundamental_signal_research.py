"""Bounded, run-versioned daily PIT fundamental research panel builder.

The builder publishes one observed market session at a time. A consumer must
filter the manifest to status='complete' before reading any run-scoped rows.

The state-selection, owner-association, lineage and rejection rules are a
shared builder over calendar, cohort and metric relations
(:func:`stage_calendar`, :func:`stage_cohort`, :func:`stage_metric_legs`,
:func:`stage_selected_states`). FQ1 runs it with one session and one metric
at a time (strict basis, unchanged output); the monthly research panel
(``atx_db.research.panel``) runs it with a month-end and a metric batch.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import _derived_annual as annual
from . import _derived_pit as pit
from .connection import DuckDBStore
from .derived_registry import default_derived_definitions

QUERY_VERSION = "fundamental-signal-pit-v1"
SOURCE_IDS = {
    "universe_id": "us_listed_v1",
    "universe_source": "atx-db us-listed universe builder",
    "market_source": "atx-db daily market panel v1",
    "metric_source": "atx-db declarative derived metrics v1",
}
# Reviewed dimensionless outputs. Extending this map requires an explicit
# accounting-unit and lineage review; caller JSON cannot enlarge it.
DIMENSIONLESS = frozenset({
    ("eps_diluted_q_growth_yoy", "q"),
    ("operating_margin_change_yoy", "ttm"),
    ("total_accruals", "ttm"),
    ("net_debt_ebitda", "ttm"),
})
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_BLOCKERS = (
    "modeled_filing_availability_not_exact_delivery_vintage",
    "source_backfill_and_historical_membership_may_be_incomplete",
    "ownership_and_terminal_label_coverage_unverified",
    "transaction_cost_and_borrow_not_modeled",
)


def _run_blockers(sessions: int, common_members: int,
                  unmatched_owners: int, eligible_values: int) -> tuple[str, ...]:
    dynamic = []
    if not sessions:
        dynamic.append("no_observed_decision_sessions")
    if not common_members:
        dynamic.append("no_visible_historical_common_membership")
    if unmatched_owners:
        dynamic.append("unmatched_derived_owner_lineage")
    if not eligible_values:
        dynamic.append("no_eligible_fundamental_signal_scores")
    return (*_BLOCKERS, *dynamic)


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _sha(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _utc_naive(value: dt.datetime, label: str) -> dt.datetime:
    if value.tzinfo is None or value.utcoffset() is None:
        raise ValueError(f"{label} must be timezone-aware UTC")
    if value.utcoffset() != dt.timedelta(0):
        raise ValueError(f"{label} must have UTC offset zero")
    return value.astimezone(dt.UTC).replace(tzinfo=None)


@dataclass(frozen=True)
class FundamentalSignalResearchOptions:
    start_date: dt.date
    end_date: dt.date
    as_of_date: dt.date
    run_at: dt.datetime
    run_id: str
    signals: tuple[dict[str, Any], ...] | None = None
    max_age_days: int = 200
    memory_limit: str = "256MB"
    threads: int = 1


@dataclass(frozen=True)
class FundamentalSignalResearchResult:
    run_id: str
    status: str
    sessions: int
    values: int
    eligible_values: int
    panel_sha256: str
    blockers: tuple[str, ...]


@dataclass(frozen=True)
class FundamentalSignalPanelValidation:
    run_id: str
    panel_sha256: str
    calendar_sha256: str
    signal_ids: tuple[str, ...]
    decision_dates: tuple[dt.date, ...]
    sessions: tuple[tuple[dt.date, dt.date | None], ...]
    specs: tuple[dict[str, Any], ...]
    source_ids: dict[str, str]
    value_count: int
    eligible_count: int


def default_signals() -> tuple[dict[str, Any], ...]:
    singles = (
        ("eps_growth_yoy", "eps_diluted_q_growth_yoy", "q", 1),
        ("operating_margin_change_yoy", "operating_margin_change_yoy", "ttm", 1),
        ("low_total_accruals", "total_accruals", "ttm", -1),
        ("low_net_debt_ebitda", "net_debt_ebitda", "ttm", -1),
    )
    rows = [{"signal_id": sid, "terms": [{"metric_code": code,
             "metric_window": window, "weight": direction, "transform": "identity"}]}
            for sid, code, window, direction in singles]
    rows.append({"signal_id": "fundamental_rank_mix", "terms": [
        {"metric_code": code, "metric_window": window,
         "weight": direction * 0.25, "transform": "cs_rank"}
        for _, code, window, direction in singles]})
    return tuple(rows)


def canonical_signals(signals: Iterable[dict[str, Any]] | None) -> tuple[dict[str, Any], ...]:
    source = default_signals() if signals is None else tuple(signals)
    if not 1 <= len(source) <= 32:
        raise ValueError("signals count must be between 1 and 32")
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for spec in source:
        if not isinstance(spec, dict) or set(spec) != {"signal_id", "terms"}:
            raise ValueError("signal requires only signal_id and terms")
        sid = spec["signal_id"]
        if not isinstance(sid, str) or not _ID.fullmatch(sid) or sid in seen:
            raise ValueError(f"invalid or duplicate signal_id: {sid!r}")
        seen.add(sid)
        terms = spec["terms"]
        if not isinstance(terms, list) or not 1 <= len(terms) <= 8:
            raise ValueError(f"{sid}: 1..8 terms required")
        normalized = []
        term_keys: set[tuple[str, str]] = set()
        for term in terms:
            if not isinstance(term, dict) or set(term) != {
                "metric_code", "metric_window", "weight", "transform"
            }:
                raise ValueError(f"{sid}: term requires code/window/weight/transform")
            key = (term["metric_code"], term["metric_window"])
            if key not in DIMENSIONLESS or key in term_keys:
                raise ValueError(f"{sid}: unsupported or duplicate dimensionless metric {key}")
            term_keys.add(key)
            weight = term["weight"]
            if isinstance(weight, bool) or not isinstance(weight, (int, float)) \
                    or not math.isfinite(weight) or weight == 0:
                raise ValueError(f"{sid}: weight must be finite and nonzero")
            if term["transform"] not in ("identity", "cs_rank"):
                raise ValueError(f"{sid}: unsupported transform")
            normalized.append({"metric_code": key[0], "metric_window": key[1],
                               "weight": float(weight), "transform": term["transform"]})
        normalized.sort(key=lambda x: (x["metric_code"], x["metric_window"]))
        result.append({"signal_id": sid, "terms": normalized})
    return tuple(sorted(result, key=lambda x: x["signal_id"]))


def _validate_options(options: FundamentalSignalResearchOptions) -> tuple[dt.datetime, tuple[dict[str, Any], ...]]:
    if not isinstance(options.run_id, str) or not _ID.fullmatch(options.run_id):
        raise ValueError("run_id must be a unique lower-case identifier")
    if any(type(value) is not dt.date for value in
           (options.start_date, options.end_date, options.as_of_date)):
        raise ValueError("start_date, end_date and as_of_date must be dates")
    if options.start_date > options.end_date or options.end_date > options.as_of_date:
        raise ValueError("require start_date <= end_date <= as_of_date")
    if (options.end_date - options.start_date).days > 3650:
        raise ValueError("requested range exceeds ten years")
    if isinstance(options.max_age_days, bool) or not isinstance(options.max_age_days, int) \
            or options.max_age_days < 1 or options.max_age_days > 730:
        raise ValueError("max_age_days must be 1..730")
    if type(options.threads) is not int or options.threads != 1 \
            or options.memory_limit != "256MB":
        raise ValueError("FQ1 requires memory_limit=256MB and threads=1")
    run_at = _utc_naive(options.run_at, "run_at")
    if run_at.date() < options.as_of_date:
        raise ValueError("run_at precedes as_of_date")
    if run_at < dt.datetime.combine(options.end_date, dt.time(22, 0)):
        raise ValueError("run_at must include the final requested 22:00 UTC decision")
    return run_at, canonical_signals(options.signals)


def _definitions(con: Any, specs: tuple[dict[str, Any], ...]) -> tuple[str, str, dict[tuple[str, str], str]]:
    return resolve_definitions(con, {(term["metric_code"], term["metric_window"])
                                     for spec in specs for term in spec["terms"]})


def resolve_definitions(
    con: Any, roots: Iterable[tuple[str, str]],
) -> tuple[str, str, dict[tuple[str, str], str]]:
    """Resolve roots and dependencies against the seed and the stored registry.

    Returns the canonical definitions manifest, its SHA-256 and the expected
    definition hash per (metric_code, metric_window). A stored definition that
    differs from the seed fails the run instead of mixing definition vintages.
    """
    roots = set(roots)
    registry = {row.metric_code: row for row in default_derived_definitions()}
    needed: set[tuple[str, str]] = set()
    pending = list(roots)
    while pending:
        code, window = pending.pop()
        if (code, window) in needed:
            continue
        definition = registry.get(code)
        if definition is None or definition.window != window:
            raise ValueError(f"missing publisher seed definition: {code}/{window}")
        needed.add((code, window))
        for dependency in definition.metric_inputs:
            dep = registry.get(dependency)
            if dep is None:
                raise ValueError(f"missing publisher dependency: {dependency}")
            pending.append((dep.metric_code, dep.window))
    rows = []
    hashes = {}
    for code, window in sorted(needed):
        definition = registry.get(code)
        if definition is None or definition.window != window:
            raise ValueError(f"missing publisher seed definition: {code}/{window}")
        plan = annual.plan_for(definition, registry)
        expected = pit.definition_hash(definition, plan)
        stored = con.execute("""
            SELECT expression, inputs_json, version FROM derived_metric_definitions
            WHERE metric_code=? AND metric_window=?
        """, [code, window]).fetchall()
        expected_row = (definition.expression, _canonical(list(definition.inputs)), definition.version)
        if len(stored) != 1 or tuple(stored[0]) != expected_row:
            raise RuntimeError(f"publisher definition registry differs from seed: {code}/{window}")
        hashes[(code, window)] = expected
        rows.append({"metric_code": code, "metric_window": window,
                     "expression": definition.expression, "inputs": list(definition.inputs),
                     "version": definition.version, "definition_hash": expected,
                     "annual_alternative": repr(plan.alternative),
                     "annual_weighted_codes": list(plan.weighted_codes)})
    encoded = _canonical(rows)
    return encoded, _sha(encoded), hashes


def _sessions(con: Any, options: FundamentalSignalResearchOptions,
              run_at: dt.datetime) -> list[tuple[dt.date, dt.date | None]]:
    rows = con.execute("""
        SELECT DISTINCT trade_date FROM market_daily_metrics
        WHERE source=? AND trade_date BETWEEN ? AND ?
          AND available_at <= ? AND as_of_date <= ?
          AND available_at <= CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR
          AND as_of_date <= trade_date
          AND close IS NOT NULL AND isfinite(close) AND close>0
        ORDER BY trade_date
    """, [SOURCE_IDS["market_source"], options.start_date, options.as_of_date,
          run_at, options.as_of_date]).fetchall()
    dates = [row[0] for row in rows]
    return [(date, dates[index + 1] if index + 1 < len(dates) else None)
            for index, date in enumerate(dates) if date <= options.end_date]


def _stream_digest(con: Any, query: str, params: list[Any]) -> str:
    digest = hashlib.sha256()
    cursor = con.execute(query, params)
    while batch := cursor.fetchmany(2048):
        for row in batch:
            encoded = _canonical([
                item.isoformat() if isinstance(item, (dt.date, dt.datetime)) else
                {"nonfinite": "nan" if math.isnan(item) else
                 "positive_infinity" if item > 0 else "negative_infinity"}
                if isinstance(item, float) and not math.isfinite(item) else item
                for item in row
            ])
            digest.update(encoded.encode("utf-8"))
            digest.update(b"\n")
    return digest.hexdigest()


def _panel_digest(con: Any, run_id: str, spec_sha: str,
                  definitions_sha: str, code_sha: str, calendar_sha: str) -> str:
    """Version 1 sealing recipe shared by publisher and read-only verifier."""
    value_sha = _stream_digest(con, """
        SELECT signal_id,decision_date,security_id,decision_at,entry_date,score,
               eligible,reason,input_end,input_available_at,cohort_size
        FROM fundamental_signal_values WHERE run_id=?
        ORDER BY signal_id,decision_date,security_id
    """, [run_id])
    input_sha = _stream_digest(con, """
        SELECT signal_id,decision_date,security_id,term_ordinal,metric_code,metric_window,
               weight,derived_value_id,derived_owner_security_id,definition_hash,
               inputs_hash,selected_input_refs_hash,lineage_digest,selected_input_cik,
               lineage_status,lineage_leaf_ids_json,selected_leaf_oldest_end,
               selected_leaf_newest_end,period_end,fiscal_period_start,fiscal_period_end,
               available_at,value_origin,history_status,value_status,raw_value,reason
        FROM fundamental_signal_inputs WHERE run_id=?
        ORDER BY signal_id,decision_date,security_id,term_ordinal
    """, [run_id])
    coverage_sha = _stream_digest(con, """
        SELECT signal_id,decision_date,decision_at,entry_date,status,visible_members,
               common_members,overlap_members,qualified_cik,unmatched_owner_states,
               eligible_scores,excluded_scores,constant_scores,reasons_json
        FROM fundamental_signal_coverage WHERE run_id=?
        ORDER BY signal_id,decision_date
    """, [run_id])
    proof_sha = _stream_digest(con, """
        SELECT derived_value_id,derived_owner_security_id,metric_code,metric_window,
               root_available_at,selected_input_refs_hash,selected_cik,status,reason,
               leaf_ids_json,input_clocks_json,fiscal_ends_json,proof_digest,
               oldest_fiscal_end,newest_fiscal_end,latest_input_clock,proof_cutoff
        FROM fundamental_signal_proofs WHERE run_id=? ORDER BY derived_value_id
    """, [run_id])
    return _sha(_canonical([spec_sha, definitions_sha, code_sha, calendar_sha,
                            value_sha, input_sha, coverage_sha, proof_sha]))


def validate_fundamental_signal_panel(
    con: Any, build_run_id: str,
) -> FundamentalSignalPanelValidation:
    """Reject incomplete or changed v1 research panels before evaluation."""
    row = con.execute("""
        SELECT status,spec_json,spec_sha256,definitions_json,definitions_sha256,
               query_version,code_sha256,source_ids_json,calendar_sha256,
               panel_sha256,diagnostic_json,start_date,end_date,as_of_date,
               run_at,blockers_json,production_eligible
        FROM fundamental_signal_runs WHERE run_id=?
    """, [build_run_id]).fetchone()
    if row is None or row[0] != "complete":
        raise ValueError("fundamental signal build is absent or not complete")
    (_status, spec_json, spec_sha, definitions_json, definitions_sha,
     query_version, code_sha, source_json, calendar_sha, panel_sha, diagnostic_json) = row[:11]
    start_date, end_date, as_of_date, run_at, blockers_json, production_eligible = row[11:]
    if query_version != QUERY_VERSION or source_json != _canonical(SOURCE_IDS):
        raise ValueError("unsupported fundamental panel query or source contract")
    if (production_eligible or start_date > end_date or end_date > as_of_date
            or run_at < dt.datetime.combine(end_date, dt.time(22, 0))):
        raise ValueError("fundamental panel timing or production contract changed")
    try:
        frozen_blockers = json.loads(blockers_json)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid fundamental panel blockers") from exc
    if not isinstance(frozen_blockers, list) or blockers_json != _canonical(frozen_blockers):
        raise ValueError("fundamental panel blockers changed")
    if (not _SHA.fullmatch(str(spec_sha)) or _sha(spec_json) != spec_sha
            or not _SHA.fullmatch(str(definitions_sha)) or _sha(definitions_json) != definitions_sha
            or not _SHA.fullmatch(str(code_sha)) or not _SHA.fullmatch(str(calendar_sha))
            or not _SHA.fullmatch(str(panel_sha))):
        raise ValueError("fundamental panel manifest hash mismatch")
    try:
        frozen = json.loads(spec_json)
        specs = canonical_signals(frozen["signals"])
        if not isinstance(frozen["max_age_days"], int) or not 1 <= frozen["max_age_days"] <= 730:
            raise ValueError("invalid age policy")
        if spec_json != _canonical({"signals": specs, "max_age_days": frozen["max_age_days"]}):
            raise ValueError("noncanonical signal specification")
        definitions = json.loads(definitions_json)
        if (not isinstance(definitions, list) or not definitions
                or definitions_json != _canonical(definitions)):
            raise ValueError("empty resolved definitions")
    except (KeyError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid fundamental panel manifest JSON") from exc
    definition_rows = con.execute("""
        SELECT signal_id,ordinal,spec_json,spec_sha256
        FROM fundamental_signal_definitions WHERE run_id=? ORDER BY ordinal
    """, [build_run_id]).fetchall()
    if len(definition_rows) != len(specs):
        raise ValueError("fundamental panel definition count changed")
    for ordinal, (stored_id, stored_ordinal, stored_json, stored_sha) in enumerate(definition_rows):
        expected_json = _canonical(specs[ordinal])
        if (stored_id != specs[ordinal]["signal_id"] or stored_ordinal != ordinal
                or stored_json != expected_json or stored_sha != _sha(expected_json)):
            raise ValueError("fundamental panel definition changed")
    calendar_rows = con.execute("""
        SELECT decision_date,min(entry_date),count(*),
               count(DISTINCT coalesce(entry_date,DATE '1900-01-01')),
               min(decision_at),count(DISTINCT decision_at)
        FROM fundamental_signal_coverage WHERE run_id=?
        GROUP BY decision_date ORDER BY decision_date
    """, [build_run_id]).fetchall()
    if not calendar_rows or any(
        row[2] != len(specs) or row[3] != 1 or row[5] != 1
        or row[4] != dt.datetime.combine(row[0], dt.time(22, 0))
        or row[0] < start_date or row[0] > end_date
        or (row[1] is not None and (row[1] <= row[0] or row[1] > as_of_date))
        for row in calendar_rows
    ):
        raise ValueError("fundamental panel coverage calendar changed")
    reconstructed_calendar = _sha(_canonical([
        (day.isoformat(), entry.isoformat() if entry else None)
        for day, entry, _, _, _, _ in calendar_rows
    ]))
    if reconstructed_calendar != calendar_sha:
        raise ValueError("fundamental panel calendar digest mismatch")
    observed_now = _sessions(
        con,
        FundamentalSignalResearchOptions(
            start_date=start_date, end_date=end_date, as_of_date=as_of_date,
            run_at=run_at.replace(tzinfo=dt.UTC), run_id=build_run_id,
        ),
        run_at,
    )
    if tuple(observed_now) != tuple((row[0], row[1]) for row in calendar_rows):
        raise ValueError("fundamental panel observed source calendar changed")
    duplicate_values = con.execute("""
        SELECT count(*)-count(DISTINCT (signal_id,decision_date,security_id))
        FROM fundamental_signal_values WHERE run_id=?
    """, [build_run_id]).fetchone()[0]
    duplicate_inputs = con.execute("""
        SELECT count(*)-count(DISTINCT (signal_id,decision_date,security_id,term_ordinal))
        FROM fundamental_signal_inputs WHERE run_id=?
    """, [build_run_id]).fetchone()[0]
    duplicate_proofs = con.execute("""
        SELECT count(*)-count(DISTINCT derived_value_id)
        FROM fundamental_signal_proofs WHERE run_id=?
    """, [build_run_id]).fetchone()[0]
    if duplicate_values or duplicate_inputs or duplicate_proofs:
        raise ValueError("fundamental panel grain is not unique")
    missing_calendar = con.execute("""
        SELECT count(*) FROM fundamental_signal_values v
        LEFT JOIN fundamental_signal_coverage c
          ON c.run_id=v.run_id AND c.signal_id=v.signal_id
         AND c.decision_date=v.decision_date
        WHERE v.run_id=? AND c.signal_id IS NULL
    """, [build_run_id]).fetchone()[0]
    invalid_eligibility = con.execute("""
        SELECT count(*) FROM fundamental_signal_values
        WHERE run_id=? AND ((eligible AND (score IS NULL OR NOT isfinite(score)
                             OR reason<>'valid' OR entry_date IS NULL))
             OR (NOT eligible AND score IS NOT NULL))
    """, [build_run_id]).fetchone()[0]
    mismatched_clocks = con.execute("""
        SELECT count(*) FROM fundamental_signal_values v
        JOIN fundamental_signal_coverage c
          ON c.run_id=v.run_id AND c.signal_id=v.signal_id
         AND c.decision_date=v.decision_date
        WHERE v.run_id=? AND (v.decision_at<>c.decision_at
             OR v.entry_date IS DISTINCT FROM c.entry_date)
    """, [build_run_id]).fetchone()[0]
    mismatched_proofs = con.execute("""
        SELECT count(*) FROM fundamental_signal_inputs i
        LEFT JOIN fundamental_signal_proofs p
          ON p.run_id=i.run_id AND p.derived_value_id=i.derived_value_id
        WHERE i.run_id=? AND i.derived_value_id IS NOT NULL
          AND (p.derived_value_id IS NULL
               OR p.proof_digest IS DISTINCT FROM i.lineage_digest
               OR p.status IS DISTINCT FROM i.lineage_status
               OR p.selected_cik IS DISTINCT FROM i.selected_input_cik)
    """, [build_run_id]).fetchone()[0]
    if missing_calendar or invalid_eligibility or mismatched_clocks or mismatched_proofs:
        raise ValueError("fundamental panel value or coverage contract changed")
    for spec in specs:
        values = con.execute("""
            SELECT count(*) FROM fundamental_signal_values
            WHERE run_id=? AND signal_id=?
        """, [build_run_id, spec["signal_id"]]).fetchone()[0]
        bad_terms = con.execute("""
            WITH input_counts AS (
              SELECT decision_date,security_id,count(*) AS n
              FROM fundamental_signal_inputs WHERE run_id=? AND signal_id=?
              GROUP BY decision_date,security_id
            )
            SELECT count(*) FROM fundamental_signal_values v
            LEFT JOIN input_counts i USING (decision_date,security_id)
            WHERE v.run_id=? AND v.signal_id=? AND coalesce(i.n,0)<>?
        """, [build_run_id, spec["signal_id"], build_run_id,
              spec["signal_id"], len(spec["terms"])]).fetchone()[0]
        inputs = con.execute("""
            SELECT count(*) FROM fundamental_signal_inputs
            WHERE run_id=? AND signal_id=?
        """, [build_run_id, spec["signal_id"]]).fetchone()[0]
        if bad_terms or inputs != values * len(spec["terms"]):
            raise ValueError("fundamental panel term count changed")
    value_count, eligible_count = con.execute("""
        SELECT count(*),count(*) FILTER (WHERE eligible)
        FROM fundamental_signal_values WHERE run_id=?
    """, [build_run_id]).fetchone()
    coverage_count, common_sum, unmatched_sum = con.execute("""
        SELECT count(*),coalesce(sum(common_members),0),
               coalesce(sum(unmatched_owner_states),0)
        FROM fundamental_signal_coverage WHERE run_id=?
    """, [build_run_id]).fetchone()
    try:
        diagnostic = json.loads(diagnostic_json)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid fundamental panel diagnostics") from exc
    expected_diagnostic = {
        "sessions": len(calendar_rows), "values": value_count,
        "eligible_values": eligible_count, "coverage_rows": coverage_count,
        "common_membership_rows": common_sum,
        "unmatched_owner_states": unmatched_sum,
    }
    if (diagnostic != expected_diagnostic
            or diagnostic_json != _canonical(expected_diagnostic)):
        raise ValueError("fundamental panel diagnostic counts changed")
    if frozen_blockers != list(_run_blockers(
        len(calendar_rows), common_sum, unmatched_sum, eligible_count,
    )):
        raise ValueError("fundamental panel blockers changed")
    computed = _panel_digest(con, build_run_id, spec_sha, definitions_sha,
                             code_sha, calendar_sha)
    if computed != panel_sha:
        raise ValueError("fundamental panel digest mismatch")
    return FundamentalSignalPanelValidation(
        build_run_id, panel_sha, calendar_sha,
        tuple(spec["signal_id"] for spec in specs),
        tuple(row[0] for row in calendar_rows),
        tuple((row[0], row[1]) for row in calendar_rows), specs, dict(SOURCE_IDS),
        int(value_count), int(eligible_count),
    )


def _json_safe(value: Any) -> Any:
    if isinstance(value, (dt.datetime, dt.date)):
        return value.isoformat()
    if isinstance(value, (tuple, list)):
        return [_json_safe(part) for part in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(part) for key, part in value.items()}
    return value


def _qualify_proof_batch(con: Any, ids: list[str],
                         expected_hashes: dict[tuple[str, str], str],
                         resolver: Any) -> dict[str, Any]:
    """Split aggregate-cap failures without hiding genuine per-root limits."""
    answer = resolver(
        con, ids, expected_cik=None, decision_cutoff=None,
        expected_definition_hashes=expected_hashes,
        max_depth=16, max_nodes=512, max_bytes=1_048_576,
    )
    aggregate_reasons = {
        "metric node bound", "leaf node bound", "selected-lineage batch byte bound",
    }
    if (len(ids) > 1 and len(answer) == len(ids)
            and all(answer[root].status == "limit_exceeded"
                    and answer[root].reason in aggregate_reasons for root in ids)):
        middle = len(ids) // 2
        left = _qualify_proof_batch(con, ids[:middle], expected_hashes, resolver)
        right = _qualify_proof_batch(con, ids[middle:], expected_hashes, resolver)
        return {**left, **right}
    return answer


# ---------------------------------------------------------------------------
# Shared PIT state-selection builder.
#
# FQ1 stages one observed decision session at a time; the monthly research
# panel (research.panel) stages one month-end formation and a batch of
# metrics at a time. Both run the same SQL over a calendar relation
# (decision_date, cutoff), a dated cohort relation and a metric relation, so
# the selection, owner association, lineage and rejection rules exist once.
# ---------------------------------------------------------------------------

STRICT_UNIVERSE_ID = SOURCE_IDS["universe_id"]
# Same value as universe_us_listed.RECONSTRUCTED_US_LISTED_UNIVERSE_ID (RX1).
RECONSTRUCTED_UNIVERSE_ID = "us_listed_reconstructed_v1"
IDENTITY_BASIS_STRICT = "dated_identifier_history"
IDENTITY_BASIS_RECONSTRUCTED = "current_ticker_unverified"
IDENTITY_BASES = (IDENTITY_BASIS_STRICT, IDENTITY_BASIS_RECONSTRUCTED)
LISTED_EXCHANGE_CODES = ("XNAS", "XNYS", "XASE", "ARCX", "BATS")
RECONSTRUCTED_ELIGIBLE_TYPES = ("common", "common_unverified")
# Values whose selected operand is a fiscal year may be older than a quarter.
ANNUAL_VALUE_ORIGINS = ("annual_fallback", "annual_dependency")
PROOF_TABLES = frozenset({"fundamental_signal_proofs", "research_panel_proofs"})
_PROOF_SOURCES = frozenset({"_fs_selected", "_fs_prior_candidates"})
_PROOF_COLUMNS_SQL = ",".join(f"unnest(CAST(? AS {kind}[]))" for kind in (
    "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "TIMESTAMP", "VARCHAR", "VARCHAR",
    "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "VARCHAR", "DATE", "DATE",
    "TIMESTAMP", "TIMESTAMP"))
_RELATION = re.compile(r"^_[a-z][a-z0-9_]{0,62}$")


@dataclass(frozen=True)
class MetricLeg:
    """One derived metric staged by :func:`stage_selected_states`.

    ``max_age_days`` bounds quarterly/instant anchors; ``annual_max_age_days``
    bounds values whose ``value_origin`` is an annual fallback or dependency.
    ``quarterly_origin_required`` rejects any non-quarterly arithmetic origin.
    """

    metric_code: str
    metric_window: str
    expected_hash: str
    max_age_days: int
    annual_max_age_days: int
    quarterly_origin_required: bool = False


def _relation(name: str) -> str:
    if not isinstance(name, str) or not _RELATION.fullmatch(name):
        raise ValueError(f"unsupported staging relation: {name!r}")
    return name


def _sql_text_list(values: Iterable[str]) -> str:
    return ",".join("'" + str(value).replace("'", "''") + "'" for value in values)


def stage_calendar(con: Any, rows: Iterable[tuple[dt.date, dt.datetime]],
                   table: str = "_fs_calendar") -> None:
    """Stage decision dates with their naive-UTC decision cutoffs."""
    table = _relation(table)
    payload = [(day, cutoff) for day, cutoff in rows]
    if any(type(day) is not dt.date or not isinstance(cutoff, dt.datetime)
           or cutoff.tzinfo is not None for day, cutoff in payload):
        raise ValueError("calendar rows need a date and a naive UTC cutoff")
    if len({day for day, _ in payload}) != len(payload):
        raise ValueError("calendar decision dates must be unique")
    con.execute(f"CREATE OR REPLACE TEMP TABLE {table} (decision_date DATE, cutoff TIMESTAMP)")
    if payload:
        con.executemany(f"INSERT INTO {table} VALUES (?, ?)", [list(row) for row in payload])


def stage_metric_legs(con: Any, legs: Iterable[MetricLeg], table: str = "_fs_metrics") -> None:
    """Stage the metric relation (code, window, expected hash and age policy)."""
    table = _relation(table)
    payload = list(legs)
    keys = [(leg.metric_code, leg.metric_window) for leg in payload]
    if not payload or len(set(keys)) != len(keys):
        raise ValueError("metric legs must be non-empty and unique")
    for leg in payload:
        if (not _SHA.fullmatch(str(leg.expected_hash))
                or any(isinstance(age, bool) or not isinstance(age, int) or not 1 <= age <= 3650
                       for age in (leg.max_age_days, leg.annual_max_age_days))):
            raise ValueError(f"invalid metric leg: {leg}")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {table} (
          metric_code VARCHAR, metric_window VARCHAR, expected_hash VARCHAR,
          max_age_days INTEGER, annual_max_age_days INTEGER,
          quarterly_origin_required BOOLEAN)
    """)
    con.executemany(f"INSERT INTO {table} VALUES (?,?,?,?,?,?)", [
        [leg.metric_code, leg.metric_window, leg.expected_hash, leg.max_age_days,
         leg.annual_max_age_days, bool(leg.quarterly_origin_required)] for leg in payload
    ])


def resolve_lineage_proofs(con: Any, run_id: str,
                           expected_hashes: dict[tuple[str, str], str], *,
                           source_table: str = "_fs_selected",
                           proof_table: str = "fundamental_signal_proofs") -> None:
    """Resolve each not-yet-proved selected root once per run, in fixed batches.

    The proof table is the run's cache: a root selected on many decision dates
    is qualified exactly once.
    """
    from .derived_lineage import qualify_selected_lineage

    if source_table not in _PROOF_SOURCES:
        raise ValueError("unsupported proof staging relation")
    if proof_table not in PROOF_TABLES:
        raise ValueError("unsupported proof table")

    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fs_unproved AS
        SELECT row_number() OVER (ORDER BY s.derived_value_id) AS proof_number,
               s.derived_value_id,s.security_id,s.metric_code,s.metric_window,
               s.available_at,s.selected_input_refs_hash
        FROM (SELECT DISTINCT derived_value_id,security_id,metric_code,metric_window,
                     available_at,selected_input_refs_hash
              FROM {source_table}) s LEFT JOIN {proof_table} p
          ON p.run_id=? AND p.derived_value_id=s.derived_value_id
        WHERE p.derived_value_id IS NULL
    """.replace("{source_table}", source_table).replace("{proof_table}", proof_table), [run_id])
    total = int(con.execute("SELECT count(*) FROM _fs_unproved").fetchone()[0])
    for start in range(1, total + 1, 256):
        batch = con.execute("""
            SELECT derived_value_id,security_id,metric_code,metric_window,
                   available_at,selected_input_refs_hash
            FROM _fs_unproved WHERE proof_number BETWEEN ? AND ?
            ORDER BY proof_number
        """, [start, start + 255]).fetchall()
        ids = [row[0] for row in batch]
        qualifications = _qualify_proof_batch(con, ids, expected_hashes,
                                               qualify_selected_lineage)
        payload = []
        for state_id, owner_id, metric_code, metric_window, event_at, refs_hash in batch:
            proof = qualifications[state_id]
            clocks = tuple(clock for clock in proof.input_clocks if clock is not None)
            ends = tuple(end for end in proof.fiscal_ends if end is not None)
            payload.append((run_id, state_id, owner_id, metric_code, metric_window,
                            event_at, refs_hash, proof.selected_cik,
                            proof.status, proof.reason,
                            _canonical(_json_safe(proof.leaf_ids)),
                            _canonical(_json_safe(clocks)),
                            _canonical(_json_safe(ends)), proof.digest,
                            min(ends) if ends else None, max(ends) if ends else None,
                            max(clocks) if clocks else None, event_at))
        if not payload:
            continue
        # One statement per batch: each column is bound as one typed list and
        # unnested in lockstep (18 parameters, not one per value).
        con.execute("""
            INSERT INTO {proof_table}
            (run_id,derived_value_id,derived_owner_security_id,metric_code,
             metric_window,root_available_at,selected_input_refs_hash,selected_cik,
             status,reason,leaf_ids_json,input_clocks_json,fiscal_ends_json,
             proof_digest,oldest_fiscal_end,newest_fiscal_end,latest_input_clock,
             proof_cutoff)
            SELECT {columns}
        """.replace("{proof_table}", proof_table).replace("{columns}", _PROOF_COLUMNS_SQL),
            [list(column) for column in zip(*payload, strict=True)])


def stage_cohort(
    con: Any, *, calendar_table: str = "_fs_calendar",
    universe_id: str = STRICT_UNIVERSE_ID,
    identity_basis: str = IDENTITY_BASIS_STRICT,
    universe_source: str = SOURCE_IDS["universe_source"],
    owner_links_table: str | None = None,
    eligible_security_types: tuple[str, ...] = RECONSTRUCTED_ELIGIBLE_TYPES,
    include_unlisted_tail: bool = True,
    membership_detail: bool = False,
) -> dict[dt.date, tuple[int, int, int, int]]:
    """Stage the dated cohort ``_fs_cohort_cal`` for every calendar decision.

    ``identity_basis='dated_identifier_history'`` (strict, FQ1 default): common
    stock on a listed venue whose CIK comes only from dated identifier history
    visible at the decision cutoff. ``'current_ticker_unverified'`` (RX1 labeled
    reconstruction): ``eligible_security_types`` on a listed venue, plus, with
    ``include_unlisted_tail``, retained names without listing evidence (the
    delisted tail), whose owner CIK comes from the reconstructed price-line ->
    owner bridge staged in ``owner_links_table`` (a link counts only when its
    interval covers the date and its modeled clock is at or before the cutoff).

    Returns (visible, eligible, overlapping, valid) member counts per date.
    """
    calendar_table = _relation(calendar_table)
    if identity_basis not in IDENTITY_BASES:
        raise ValueError(f"unsupported identity basis: {identity_basis!r}")
    reconstructed = identity_basis == IDENTITY_BASIS_RECONSTRUCTED
    if reconstructed and owner_links_table is None:
        raise ValueError("the reconstructed identity basis needs an owner-link relation")
    detail = membership_detail or reconstructed
    detail_columns = ("max(u.reason) AS membership_reason, max(u.symbol) AS symbol" if detail else
                      "CAST(NULL AS VARCHAR) AS membership_reason, CAST(NULL AS VARCHAR) AS symbol")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _fs_members AS
        SELECT c.decision_date, u.security_id, count(*) AS visible_count,
               max(u.security_type) AS security_type,
               max(u.exchange_code) AS exchange_code,
               max(u.cik) AS membership_cik, {detail_columns}
        FROM universe_us_listed_membership u JOIN {calendar_table} c
          ON u.valid_from<=c.decision_date
         AND (u.valid_to IS NULL OR u.valid_to>=c.decision_date)
         AND u.available_at<=c.cutoff AND u.as_of_date<=c.decision_date
        WHERE u.universe_id=? AND u.source=?
        GROUP BY c.decision_date, u.security_id
    """, [universe_id, universe_source])
    venues = _sql_text_list(LISTED_EXCHANGE_CODES)
    if not reconstructed:
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _fs_cik AS
            SELECT m.decision_date, h.security_id,
                   count(DISTINCT CASE WHEN regexp_full_match(h.id_value, '[0-9]{{1,10}}')
                     THEN lpad(h.id_value,10,'0') ELSE h.id_value END) AS cik_count,
                   max(CASE WHEN regexp_full_match(h.id_value, '[0-9]{{1,10}}')
                     THEN lpad(h.id_value,10,'0') END) AS cik,
                   count(*) FILTER (WHERE NOT regexp_full_match(h.id_value, '[0-9]{{1,10}}')) AS invalid_count
            FROM security_identifier_history h JOIN _fs_members m USING (security_id)
            JOIN {calendar_table} c ON c.decision_date=m.decision_date
            WHERE h.id_type='CIK' AND h.id_value IS NOT NULL
              AND h.valid_from<=c.decision_date
              AND (h.valid_to IS NULL OR h.valid_to>c.decision_date)
              AND h.available_at IS NOT NULL AND h.available_at<=c.cutoff
              AND h.as_of_date<=c.decision_date
            GROUP BY m.decision_date, h.security_id
        """)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _fs_cohort_cal AS
            SELECT m.decision_date, m.security_id, c.cik,
                   (m.security_type='common' AND m.exchange_code IN ({venues})) AS is_common,
                   CASE WHEN m.visible_count>1 THEN 'overlapping_membership'
                        WHEN m.security_type<>'common' OR m.exchange_code NOT IN
                             ({venues}) THEN 'not_common'
                        WHEN c.cik_count IS NULL THEN 'missing_dated_cik'
                        WHEN c.invalid_count>0 THEN 'invalid_dated_cik'
                        WHEN c.cik_count<>1 THEN 'ambiguous_dated_cik'
                        WHEN m.membership_cik IS NOT NULL AND
                             (NOT regexp_full_match(m.membership_cik,'[0-9]{{1,10}}') OR
                              lpad(m.membership_cik,10,'0') IS DISTINCT FROM c.cik)
                             THEN 'membership_cik_mismatch'
                        ELSE 'valid' END AS cohort_reason,
                   m.symbol, m.security_type, m.exchange_code, m.membership_reason,
                   m.membership_cik, '{IDENTITY_BASIS_STRICT}' AS identity_basis,
                   CASE WHEN c.cik_count=1 THEN 'dated_identifier_history' END AS owner_link_method,
                   CASE WHEN c.cik_count IS NULL THEN 'no_dated_cik_at_cutoff' END AS owner_link_reason
            FROM _fs_members m LEFT JOIN _fs_cik c
              ON c.decision_date=m.decision_date AND c.security_id=m.security_id
        """)
    else:
        links = _relation(owner_links_table)
        types = _sql_text_list(eligible_security_types) or "''"
        tail = "true" if include_unlisted_tail else "false"
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _fs_link_state AS
            WITH links AS (
              SELECT m.decision_date, m.security_id, l.cik AS raw_cik, l.link_method,
                     l.identity_basis, l.unlinked_reason,
                     coalesce(l.cik IS NOT NULL AND l.valid_from<=c.decision_date
                              AND (l.valid_to IS NULL OR l.valid_to>c.decision_date), false) AS covers,
                     coalesce(l.available_at<=c.cutoff, false) AS known
              FROM _fs_members m JOIN {calendar_table} c ON c.decision_date=m.decision_date
              JOIN {links} l ON l.price_security_id=m.security_id
            )
            SELECT decision_date, security_id,
                   count(DISTINCT CASE WHEN regexp_full_match(raw_cik,'[0-9]{{1,10}}')
                     THEN lpad(raw_cik,10,'0') ELSE raw_cik END) FILTER (WHERE covers AND known) AS link_count,
                   max(CASE WHEN regexp_full_match(raw_cik,'[0-9]{{1,10}}')
                     THEN lpad(raw_cik,10,'0') END) FILTER (WHERE covers AND known) AS cik,
                   count(*) FILTER (WHERE covers AND known
                                    AND NOT regexp_full_match(raw_cik,'[0-9]{{1,10}}')) AS invalid_count,
                   min(link_method) FILTER (WHERE covers AND known) AS link_method,
                   min(identity_basis) FILTER (WHERE covers AND known) AS identity_basis,
                   count(*) FILTER (WHERE covers AND NOT known) AS late_links,
                   min(unlinked_reason) AS unlinked_reason
            FROM links GROUP BY decision_date, security_id
        """)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _fs_cohort_cal AS
            WITH typed AS (
              SELECT *, coalesce((security_type IN ({types}) AND exchange_code IN ({venues}))
                        OR ({tail} AND membership_reason='reconstructed_no_listing_evidence'),
                        false) AS eligible
              FROM _fs_members
            )
            SELECT m.decision_date, m.security_id, k.cik, m.eligible AS is_common,
                   CASE WHEN m.visible_count>1 THEN 'overlapping_membership'
                        WHEN NOT m.eligible THEN 'not_common'
                        WHEN m.membership_reason='member_conflicting_cik'
                             THEN 'membership_conflicting_cik'
                        WHEN coalesce(k.link_count,0)=0 THEN 'missing_owner_link'
                        WHEN k.invalid_count>0 THEN 'invalid_owner_link_cik'
                        WHEN k.link_count<>1 THEN 'ambiguous_owner_link'
                        WHEN m.membership_cik IS NOT NULL AND
                             (NOT regexp_full_match(m.membership_cik,'[0-9]{{1,10}}') OR
                              lpad(m.membership_cik,10,'0') IS DISTINCT FROM k.cik)
                             THEN 'membership_cik_mismatch'
                        ELSE 'valid' END AS cohort_reason,
                   m.symbol, m.security_type, m.exchange_code, m.membership_reason,
                   m.membership_cik,
                   coalesce(k.identity_basis, '{IDENTITY_BASIS_RECONSTRUCTED}') AS identity_basis,
                   k.link_method AS owner_link_method,
                   CASE WHEN coalesce(k.link_count,0)>0 THEN NULL
                        WHEN coalesce(k.late_links,0)>0 THEN 'owner_link_after_cutoff'
                        ELSE coalesce(k.unlinked_reason,'no_owner_link_interval') END AS owner_link_reason
            FROM typed m LEFT JOIN _fs_link_state k
              ON k.decision_date=m.decision_date AND k.security_id=m.security_id
        """)
    return {row[0]: (int(row[1]), int(row[2]), int(row[3]), int(row[4])) for row in con.execute("""
        SELECT decision_date, count(*), count(*) FILTER (WHERE is_common),
               count(*) FILTER (WHERE cohort_reason='overlapping_membership'),
               count(*) FILTER (WHERE cohort_reason='valid')
        FROM _fs_cohort_cal GROUP BY decision_date
    """).fetchall()}


def _stage_cohort(con: Any, day: dt.date, cutoff: dt.datetime,
                  as_of_date: dt.date, *, universe_id: str = STRICT_UNIVERSE_ID,
                  identity_basis: str = IDENTITY_BASIS_STRICT,
                  owner_links_table: str | None = None) -> tuple[int, int, int, int]:
    """FQ1 single-session cohort (strict by default) staged as ``_fs_cohort``."""
    stage_calendar(con, [(day, cutoff)])
    counts = stage_cohort(con, universe_id=universe_id, identity_basis=identity_basis,
                          owner_links_table=owner_links_table)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fs_cohort AS
        SELECT security_id, cik, is_common, cohort_reason FROM _fs_cohort_cal
    """)
    return counts.get(day, (0, 0, 0, 0))


def _stage_leg(con: Any, run_id: str, code: str, window: str,
               expected_hash: str, all_hashes: dict[tuple[str, str], str],
               day: dt.date, cutoff: dt.datetime, max_age_days: int) -> None:
    """FQ1 single-session, single-metric leg over the staged ``_fs_cohort``."""
    stage_calendar(con, [(day, cutoff)])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fs_leg_cohort AS
        SELECT CAST(? AS DATE) AS decision_date, security_id, cik, cohort_reason FROM _fs_cohort
    """, [day])
    stage_metric_legs(con, [MetricLeg(code, window, expected_hash, max_age_days, max_age_days,
                                      code == "eps_diluted_q_growth_yoy")])
    stage_selected_states(con, run_id=run_id, all_hashes=all_hashes)


def stage_selected_states(
    con: Any, *, run_id: str, all_hashes: dict[tuple[str, str], str],
    calendar_table: str = "_fs_calendar", cohort_table: str = "_fs_leg_cohort",
    metrics_table: str = "_fs_metrics", proof_table: str = "fundamental_signal_proofs",
    metric_source: str = SOURCE_IDS["metric_source"],
    max_selected_states: int = 100_000, max_prior_states: int = 100_000,
) -> None:
    """Stage ``_fs_leg``: one selected state and reason per (date, member, metric).

    Inputs are relations: ``calendar_table`` (decision_date, cutoff),
    ``cohort_table`` (decision_date, security_id, cik, cohort_reason) and
    ``metrics_table`` (see :func:`stage_metric_legs`). Every derived state is
    selected as visible at its own decision cutoff; lineage proofs are cached in
    ``proof_table`` per run and resolved once per ``derived_value_id``.
    """
    calendar_table = _relation(calendar_table)
    cohort_table = _relation(cohort_table)
    metrics_table = _relation(metrics_table)
    if proof_table not in PROOF_TABLES:
        raise ValueError("unsupported proof table")

    def sql(text: str) -> str:
        return (text.replace("{calendar}", calendar_table).replace("{cohort}", cohort_table)
                .replace("{metrics}", metrics_table).replace("{proofs}", proof_table))

    # Select complete visible publisher states by owner before testing status.
    # An issuer-owned state may have a different security_id than the listed
    # security; exact selected leaf CIK proof supplies the joining identity.
    # The two rankings sort only narrow keys (the wide state row is joined
    # back by its primary key): identical selection, ~4x less sort volume.
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_selected_keys AS
        WITH bucket_states AS (
          SELECT c.decision_date,d.metric_code,d.metric_window,d.security_id,
                 d.period_end,d.available_at,d.derived_value_id,
                 row_number() OVER (
                   PARTITION BY c.decision_date,d.metric_code,d.metric_window,
                     d.security_id,coalesce(d.target_bucket,
                     CAST(floor((year(d.period_end)*12+month(d.period_end)-1+
                       CASE WHEN day(d.period_end)>=15 THEN 1 ELSE 0 END)/3.0) AS BIGINT))
                   ORDER BY d.available_at DESC,d.derived_value_id DESC) AS bucket_rank
          FROM derived_metric_values d
          JOIN {metrics} m ON m.metric_code=d.metric_code AND m.metric_window=d.metric_window
          JOIN {calendar} c ON d.period_end<=c.decision_date AND d.available_at<=c.cutoff
                           AND d.as_of_date<=c.decision_date
          WHERE d.source=?
        ), selected AS (
          SELECT *,row_number() OVER (
            PARTITION BY decision_date,metric_code,metric_window,security_id
            ORDER BY period_end DESC,available_at DESC,derived_value_id DESC
          ) AS period_rank FROM bucket_states WHERE bucket_rank=1
        )
        SELECT decision_date,metric_code,metric_window,security_id,derived_value_id
        FROM selected WHERE period_rank=1
    """), [metric_source])
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_selected AS
        SELECT k.decision_date,c.cutoff,d.derived_value_id,d.source,d.security_id,
               d.metric_code,d.metric_window,d.target_bucket,d.period_end,d.value,
               d.available_at,d.as_of_date,d.valid_to,d.inputs_hash,
               d.definition_hash,d.history_status,d.value_status,
               d.selected_input_refs_hash,d.fiscal_period_start,
               d.fiscal_period_end,d.value_origin
        FROM _fs_selected_keys k JOIN {calendar} c ON c.decision_date=k.decision_date
        JOIN derived_metric_values d ON d.derived_value_id=k.derived_value_id
         AND d.security_id=k.security_id AND d.metric_code=k.metric_code
         AND d.metric_window=k.metric_window
        WHERE d.source=?
    """), [metric_source])
    candidate_count = int(con.execute("SELECT count(*) FROM _fs_selected").fetchone()[0])
    if candidate_count > max_selected_states:
        raise RuntimeError(
            f"selected owner bound exceeded: {candidate_count} > {max_selected_states}")
    resolve_lineage_proofs(con, run_id, all_hashes, proof_table=proof_table)
    # An invalid current state may have no identifiable leaves. Count the full
    # prior history before staging, then resolve every prior event under a
    # fixed partition cap. A cap breach fails the run; it never silently drops
    # old issuer evidence and lets a different owner win. When every selected
    # state already has a proven CIK the prior history cannot matter, so the
    # history scan is skipped (the staged candidate set is empty either way).
    unowned = int(con.execute(sql("""
        SELECT count(*) FROM _fs_selected s JOIN {proofs} current_proof
          ON current_proof.run_id=? AND current_proof.derived_value_id=s.derived_value_id
        WHERE current_proof.selected_cik IS NULL
    """), [run_id]).fetchone()[0])
    prior_count = 0 if not unowned else int(con.execute(sql("""
        SELECT count(*)
        FROM _fs_selected s JOIN {proofs} current_proof
          ON current_proof.run_id=? AND current_proof.derived_value_id=s.derived_value_id
        JOIN derived_metric_values d ON d.security_id=s.security_id
          AND d.metric_code=s.metric_code AND d.metric_window=s.metric_window
          AND d.source=s.source AND d.available_at<s.available_at
          AND d.available_at<=s.cutoff AND d.as_of_date<=s.decision_date
        WHERE current_proof.selected_cik IS NULL
    """), [run_id]).fetchone()[0])
    if prior_count > max_prior_states:
        raise RuntimeError(
            f"prior owner proof bound exceeded: {prior_count} > {max_prior_states}")
    history = "" if unowned else "AND false"
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_prior_candidates AS
        SELECT DISTINCT d.derived_value_id,d.security_id,d.metric_code,d.metric_window,
               d.available_at,d.selected_input_refs_hash
        FROM _fs_selected s JOIN {proofs} current_proof
          ON current_proof.run_id=? AND current_proof.derived_value_id=s.derived_value_id
        JOIN derived_metric_values d ON d.security_id=s.security_id
          AND d.metric_code=s.metric_code AND d.metric_window=s.metric_window
          AND d.source=s.source AND d.available_at<s.available_at
          AND d.available_at<=s.cutoff AND d.as_of_date<=s.decision_date
        WHERE current_proof.selected_cik IS NULL {history}
    """).replace("{history}", history), [run_id])
    resolve_lineage_proofs(con, run_id, all_hashes, source_table="_fs_prior_candidates",
                           proof_table=proof_table)
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_prior_owner_cik AS
        SELECT s.decision_date,s.derived_value_id,
               arg_max(p.selected_cik,(p.root_available_at,p.derived_value_id)) AS prior_cik
        FROM _fs_selected s JOIN {proofs} p
          ON p.run_id=? AND p.derived_owner_security_id=s.security_id
         AND p.metric_code=s.metric_code AND p.metric_window=s.metric_window
         AND p.root_available_at<s.available_at AND p.root_available_at<=s.cutoff
         AND p.status='qualified' AND p.selected_cik IS NOT NULL
        GROUP BY s.decision_date,s.derived_value_id
    """), [run_id])
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_owner_state AS
        SELECT s.decision_date,s.metric_code,s.metric_window,
               s.security_id AS derived_owner_security_id,s.derived_value_id,
               s.definition_hash,s.inputs_hash,s.selected_input_refs_hash,
               s.period_end,s.fiscal_period_start,s.fiscal_period_end,
               s.available_at,s.value_origin,s.history_status,s.value_status,
               s.value AS raw_value,s.valid_to,
               p.selected_cik,coalesce(p.selected_cik,a.prior_cik) AS association_cik,
               p.status AS lineage_status,p.reason AS lineage_reason,
               p.proof_digest AS lineage_digest,p.leaf_ids_json AS lineage_leaf_ids_json,
               p.oldest_fiscal_end,p.newest_fiscal_end,p.latest_input_clock
        FROM _fs_selected s JOIN {proofs} p
          ON p.run_id=? AND p.derived_value_id=s.derived_value_id
        LEFT JOIN _fs_prior_owner_cik a
          ON a.decision_date=s.decision_date AND a.derived_value_id=s.derived_value_id
    """), [run_id])
    state = """struct_pack(
                 derived_owner_security_id := s.derived_owner_security_id,
                 derived_value_id := s.derived_value_id,
                 definition_hash := s.definition_hash,inputs_hash := s.inputs_hash,
                 selected_input_refs_hash := s.selected_input_refs_hash,
                 period_end := s.period_end,fiscal_period_start := s.fiscal_period_start,
                 fiscal_period_end := s.fiscal_period_end,available_at := s.available_at,
                 value_origin := s.value_origin,history_status := s.history_status,
                 value_status := s.value_status,raw_value := s.raw_value,
                 valid_to := s.valid_to,selected_cik := s.selected_cik,
                 lineage_status := s.lineage_status,lineage_reason := s.lineage_reason,
                 lineage_digest := s.lineage_digest,lineage_leaf_ids_json := s.lineage_leaf_ids_json,
                 oldest_fiscal_end := s.oldest_fiscal_end,newest_fiscal_end := s.newest_fiscal_end,
                 latest_input_clock := s.latest_input_clock)"""
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_owner_matches AS
        SELECT c.decision_date,c.security_id,s.metric_code,s.metric_window,
               count(*) AS owner_count,
               arg_max({state},(s.period_end,s.available_at,s.derived_value_id)) AS state
        FROM {cohort} c JOIN _fs_owner_state s
          ON s.decision_date=c.decision_date AND s.association_cik=c.cik
        WHERE c.cohort_reason='valid' AND s.association_cik IS NOT NULL
        GROUP BY c.decision_date,c.security_id,s.metric_code,s.metric_window
    """).replace("{state}", state))
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_direct_state AS
        SELECT c.decision_date,c.security_id,s.metric_code,s.metric_window,
               arg_max({state},(s.period_end,s.available_at,s.derived_value_id)) AS state
        FROM {cohort} c JOIN _fs_owner_state s
          ON s.decision_date=c.decision_date AND s.derived_owner_security_id=c.security_id
        GROUP BY c.decision_date,c.security_id,s.metric_code,s.metric_window
    """).replace("{state}", state))
    # One row per (decision, cohort member, metric). The first failing rule is
    # the row's reason; staleness is judged per value origin (annual operands
    # may legitimately be older than quarterly ones).
    con.execute(sql("""
        CREATE OR REPLACE TEMP TABLE _fs_leg AS
        WITH joined AS (
          SELECT c.decision_date,cal.cutoff,c.security_id,c.cik,c.cohort_reason,
                 m.metric_code,m.metric_window,m.expected_hash,
                 m.quarterly_origin_required,o.owner_count,
                 coalesce(o.state,d.state) AS s,
                 CASE WHEN struct_extract(coalesce(o.state,d.state),'value_origin')
                           IN ({annual_origins})
                      THEN m.annual_max_age_days ELSE m.max_age_days END AS max_age_days
          FROM {cohort} c JOIN {calendar} cal ON cal.decision_date=c.decision_date
          CROSS JOIN {metrics} m
          LEFT JOIN _fs_owner_matches o
            ON o.decision_date=c.decision_date AND o.security_id=c.security_id
           AND o.metric_code=m.metric_code AND o.metric_window=m.metric_window
          LEFT JOIN _fs_direct_state d
            ON d.decision_date=c.decision_date AND d.security_id=c.security_id
           AND d.metric_code=m.metric_code AND d.metric_window=m.metric_window
        )
        SELECT decision_date,cutoff,security_id,cik,cohort_reason,metric_code,metric_window,
               owner_count,s.derived_value_id,
               s.derived_owner_security_id,s.definition_hash,s.inputs_hash,
               s.selected_input_refs_hash,s.lineage_digest,s.selected_cik AS selected_input_cik,
               s.lineage_status,s.lineage_reason,s.lineage_leaf_ids_json,
               s.oldest_fiscal_end AS selected_leaf_oldest_end,
               s.newest_fiscal_end AS selected_leaf_newest_end,
               s.period_end,s.fiscal_period_start,s.fiscal_period_end,s.available_at,
               s.value_origin,s.history_status,s.value_status,s.raw_value,
               s.latest_input_clock,s.valid_to AS state_valid_to,
               date_diff('day',s.fiscal_period_end,decision_date) AS age_days,max_age_days,
               CASE WHEN cohort_reason<>'valid' THEN cohort_reason
                    WHEN owner_count>1 THEN 'ambiguous_derived_owner'
                    WHEN s.derived_value_id IS NULL THEN 'missing_metric_state'
                    WHEN s.lineage_status IS DISTINCT FROM 'qualified' THEN
                         coalesce(s.lineage_reason,'selected_lineage_unqualified')
                    WHEN s.selected_cik IS DISTINCT FROM cik THEN 'selected_input_cik_mismatch'
                    WHEN s.history_status IS DISTINCT FROM 'event_reconstructed' THEN 'uncertified_history'
                    WHEN s.definition_hash IS DISTINCT FROM expected_hash THEN 'definition_hash_mismatch'
                    WHEN s.inputs_hash IS NULL OR NOT regexp_full_match(s.inputs_hash,'[0-9a-f]{64}')
                         THEN 'invalid_inputs_hash'
                    WHEN s.selected_input_refs_hash IS NULL OR
                         NOT regexp_full_match(s.selected_input_refs_hash,'[0-9a-f]{64}')
                         THEN 'invalid_selected_refs_hash'
                    WHEN s.value_status IS DISTINCT FROM 'valid' OR s.raw_value IS NULL
                         OR NOT isfinite(s.raw_value) THEN 'invalid_current_state'
                    WHEN s.value_origin IS NULL OR s.value_origin IN
                         ('legacy_unspecified','unavailable','incomparable') THEN 'invalid_value_origin'
                    WHEN quarterly_origin_required AND s.value_origin<>'quarterly'
                         THEN 'nonquarterly_eps_origin'
                    WHEN s.fiscal_period_end IS NULL OR s.fiscal_period_end>decision_date
                         OR s.newest_fiscal_end IS NULL OR s.newest_fiscal_end>decision_date
                         OR (s.oldest_fiscal_end IS NOT NULL AND s.oldest_fiscal_end>decision_date)
                         OR (s.fiscal_period_start IS NOT NULL AND s.fiscal_period_start>decision_date)
                         THEN 'future_or_missing_operand_period'
                    WHEN date_diff('day',s.fiscal_period_end,decision_date)>max_age_days
                         OR date_diff('day',s.newest_fiscal_end,decision_date)>max_age_days
                         THEN 'stale_current_anchor'
                    WHEN s.latest_input_clock IS NULL OR s.latest_input_clock>cutoff
                         OR s.latest_input_clock>s.available_at THEN 'invalid_input_clock'
                    WHEN s.valid_to IS NOT NULL AND s.valid_to<=cutoff
                         THEN 'expired_state_without_successor'
                    ELSE 'valid' END AS reason
        FROM joined
    """).replace("{annual_origins}", _sql_text_list(ANNUAL_VALUE_ORIGINS)))


def _publish_signal(con: Any, run_id: str, spec: dict[str, Any],
                    day: dt.date, cutoff: dt.datetime, entry: dt.date | None,
                    counts: tuple[int, int, int, int], hashes: dict[tuple[str, str], str],
                    max_age_days: int) -> None:
    sid = spec["signal_id"]
    unmatched_owner_states = 0
    con.execute("CREATE OR REPLACE TEMP TABLE _fs_terms (security_id VARCHAR, term_ordinal INTEGER, "
                "raw_value DOUBLE, reason VARCHAR, available_at TIMESTAMP, fiscal_period_end DATE, "
                "weight DOUBLE, transform VARCHAR)")
    for ordinal, term in enumerate(spec["terms"]):
        code, window = term["metric_code"], term["metric_window"]
        _stage_leg(con, run_id, code, window, hashes[(code, window)],
                   hashes, day, cutoff, max_age_days)
        unmatched_owner_states += int(con.execute("""
            SELECT count(*) FROM _fs_owner_state WHERE association_cik IS NULL
        """).fetchone()[0])
        con.execute("""
            INSERT INTO fundamental_signal_inputs
            (run_id,signal_id,decision_date,security_id,term_ordinal,metric_code,metric_window,
             weight,derived_value_id,derived_owner_security_id,definition_hash,inputs_hash,
             selected_input_refs_hash,lineage_digest,selected_input_cik,lineage_status,
             lineage_leaf_ids_json,selected_leaf_oldest_end,selected_leaf_newest_end,
             period_end,fiscal_period_start,fiscal_period_end,available_at,value_origin,
             history_status,value_status,raw_value,reason)
            SELECT ?,?,?,security_id,?,?,?, ?,derived_value_id,derived_owner_security_id,
                   definition_hash,inputs_hash,selected_input_refs_hash,lineage_digest,
                   selected_input_cik,lineage_status,lineage_leaf_ids_json,
                   selected_leaf_oldest_end,selected_leaf_newest_end,period_end,
                   fiscal_period_start,fiscal_period_end,available_at,value_origin,
                   history_status,value_status,raw_value,reason FROM _fs_leg
        """, [run_id, sid, day, ordinal, code, window, term["weight"]])
        con.execute("""
            INSERT INTO _fs_terms SELECT security_id,?,raw_value,reason,available_at,
                                         fiscal_period_end,?,?
            FROM _fs_leg
        """, [ordinal, term["weight"], term["transform"]])
    n_terms = len(spec["terms"])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fs_complete AS
        SELECT security_id,
               count(*) FILTER (WHERE reason='valid') AS valid_terms,
               min(reason) FILTER (WHERE reason<>'valid') AS failure_reason,
               max(available_at) AS input_available_at,
               max(fiscal_period_end) AS input_end
        FROM _fs_terms GROUP BY security_id
    """)
    # Rank only complete names. Direction is applied before ranking, and ties
    # receive the same percentile. A constant cohort gets neutral 0.5 ranks.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fs_scores AS
        WITH ordered AS (
          SELECT t.security_id,t.term_ordinal,t.raw_value,t.weight,t.transform,
                 rank() OVER (PARTITION BY t.term_ordinal
                              ORDER BY sign(t.weight)*t.raw_value) AS ordinal_rank,
                 count(*) OVER (PARTITION BY t.term_ordinal) AS cohort_count,
                 min(sign(t.weight)*t.raw_value) OVER (PARTITION BY t.term_ordinal) AS min_value,
                 max(sign(t.weight)*t.raw_value) OVER (PARTITION BY t.term_ordinal) AS max_value
          FROM _fs_terms t JOIN _fs_complete c USING (security_id)
          WHERE c.valid_terms=?
        )
        SELECT security_id, sum(CASE WHEN transform='identity' THEN weight*raw_value
             WHEN max_value=min_value THEN abs(weight)*0.5
             ELSE abs(weight)*(ordinal_rank-1)::DOUBLE/(cohort_count-1) END) AS score
        FROM ordered GROUP BY security_id
    """, [n_terms])
    con.execute("""
        INSERT INTO fundamental_signal_values
          (run_id,signal_id,decision_date,security_id,decision_at,entry_date,score,
           eligible,reason,input_end,input_available_at,cohort_size)
        SELECT ?,?,?,c.security_id,?,?,CASE WHEN ? IS NOT NULL THEN s.score END,
               s.score IS NOT NULL AND ? IS NOT NULL,
               CASE WHEN c.failure_reason IS NOT NULL THEN c.failure_reason
                    WHEN ? IS NULL THEN 'missing_next_session'
                    WHEN s.score IS NULL THEN 'invalid_score' ELSE 'valid' END,
               c.input_end,c.input_available_at,?
        FROM _fs_complete c LEFT JOIN _fs_scores s USING (security_id)
    """, [run_id, sid, day, cutoff, entry, entry, entry, entry, counts[0]])
    diagnostic = con.execute("""
        SELECT count(*),count(*) FILTER (WHERE eligible),
               count(DISTINCT score) FILTER (WHERE eligible)
        FROM fundamental_signal_values
        WHERE run_id=? AND signal_id=? AND decision_date=?
    """, [run_id, sid, day]).fetchone()
    reasons = _canonical(dict(con.execute("""
        SELECT reason,count(*) FROM fundamental_signal_values
        WHERE run_id=? AND signal_id=? AND decision_date=? GROUP BY reason ORDER BY reason
    """, [run_id, sid, day]).fetchall()))
    status = ("empty_membership" if counts[0] == 0 else
              "empty_common_cohort" if counts[1] == 0 else
              "missing_next_session" if entry is None else
              "eligible" if int(diagnostic[1]) else "blocked_or_ineligible")
    con.execute("""
        INSERT INTO fundamental_signal_coverage
        (run_id,signal_id,decision_date,decision_at,entry_date,status,visible_members,
         common_members,overlap_members,qualified_cik,unmatched_owner_states,
         eligible_scores,excluded_scores,
         constant_scores,reasons_json)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    """, [run_id, sid, day, cutoff, entry, status, *counts,
          unmatched_owner_states,
          int(diagnostic[1]), int(diagnostic[0])-int(diagnostic[1]),
          int(diagnostic[1]) > 0 and int(diagnostic[2]) <= 1, reasons])


def build_fundamental_signal_panel(
    store: DuckDBStore, options: FundamentalSignalResearchOptions,
) -> FundamentalSignalResearchResult:
    """Build a research-only panel in bounded, independently committed sessions."""
    run_at, specs = _validate_options(options)
    con = store.con
    con.execute("SET memory_limit = '256MB'")
    con.execute("SET threads = 1")
    con.execute("SET preserve_insertion_order = false")
    for table in ("fundamental_signal_runs", "fundamental_signal_values",
                  "fundamental_signal_inputs", "fundamental_signal_coverage",
                  "fundamental_signal_proofs"):
        if not con.execute("SELECT count(*) FROM information_schema.tables WHERE table_name=?",
                           [table]).fetchone()[0]:
            raise RuntimeError("warehouse must already have migration 0324")
    if not con.execute("""
        SELECT count(*) FROM information_schema.columns
        WHERE table_name='derived_metric_values' AND column_name='selected_input_refs_hash'
    """).fetchone()[0]:
        raise RuntimeError("warehouse must already have DL1 selected-input lineage migration 0323")
    if con.execute("SELECT count(*) FROM fundamental_signal_runs WHERE run_id=?",
                   [options.run_id]).fetchone()[0]:
        raise ValueError(f"run_id already exists: {options.run_id}")
    definitions_json, definitions_sha, hashes = _definitions(con, specs)
    spec_json = _canonical({"signals": specs, "max_age_days": options.max_age_days})
    sessions = _sessions(con, options, run_at)
    calendar_sha = _sha(_canonical([(a.isoformat(), b.isoformat() if b else None)
                                     for a, b in sessions]))
    blockers = _BLOCKERS
    code_sha = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    with store.transaction():
        con.execute("""
            INSERT INTO fundamental_signal_runs
            (run_id,status,spec_json,spec_sha256,definitions_json,definitions_sha256,
             query_version,code_sha256,source_ids_json,calendar_sha256,start_date,end_date,as_of_date,
             run_at,decision_policy,blockers_json)
            VALUES (?,'building',?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """, [options.run_id, spec_json, _sha(spec_json), definitions_json, definitions_sha,
              QUERY_VERSION, code_sha, _canonical(SOURCE_IDS), calendar_sha, options.start_date,
              options.end_date, options.as_of_date, run_at,
              "T 22:00 UTC; next observed session close", _canonical(blockers)])
        con.executemany("""
            INSERT INTO fundamental_signal_definitions
            (run_id,signal_id,ordinal,spec_json,spec_sha256) VALUES (?,?,?,?,?)
        """, [(options.run_id, spec["signal_id"], ordinal, _canonical(spec),
               _sha(_canonical(spec))) for ordinal, spec in enumerate(specs)])
    try:
        for day, entry in sessions:
            cutoff = dt.datetime.combine(day, dt.time(22, 0))
            if cutoff > run_at:
                continue
            with store.transaction():
                counts = _stage_cohort(con, day, cutoff, options.as_of_date)
                for spec in specs:
                    _publish_signal(con, options.run_id, spec, day, cutoff, entry,
                                    counts, hashes, options.max_age_days)
        panel_sha = _panel_digest(con, options.run_id, _sha(spec_json),
                                  definitions_sha, code_sha, calendar_sha)
        value_count, eligible_count = con.execute("""
            SELECT count(*),count(*) FILTER (WHERE eligible)
            FROM fundamental_signal_values WHERE run_id=?
        """, [options.run_id]).fetchone()
        coverage_count = con.execute("SELECT count(*) FROM fundamental_signal_coverage WHERE run_id=?",
                                     [options.run_id]).fetchone()[0]
        if coverage_count != len(sessions) * len(specs):
            raise RuntimeError("coverage does not contain every requested observed decision session")
        common_sum, unmatched_sum = con.execute("""
            SELECT coalesce(sum(common_members),0),coalesce(sum(unmatched_owner_states),0)
            FROM fundamental_signal_coverage WHERE run_id=?
        """, [options.run_id]).fetchone()
        diagnostic = {"sessions": len(sessions), "values": value_count,
                      "eligible_values": eligible_count, "coverage_rows": coverage_count,
                      "common_membership_rows": common_sum,
                      "unmatched_owner_states": unmatched_sum}
        final_blockers = _run_blockers(len(sessions), common_sum,
                                       unmatched_sum, eligible_count)
        status = "complete" if sessions and eligible_count else "blocked_empty"
        with store.transaction():
            con.execute("""
                UPDATE fundamental_signal_runs
                SET status=?,diagnostic_json=?,blockers_json=?,panel_sha256=?
                WHERE run_id=? AND status='building'
            """, [status, _canonical(diagnostic), _canonical(final_blockers),
                  panel_sha, options.run_id])
        return FundamentalSignalResearchResult(options.run_id, status, len(sessions),
                                               value_count, eligible_count, panel_sha, final_blockers)
    except Exception as exc:
        with store.transaction():
            con.execute("""
                UPDATE fundamental_signal_runs SET status='failed',diagnostic_json=?
                WHERE run_id=? AND status='building'
            """, [_canonical({"error": type(exc).__name__, "message": str(exc)}), options.run_id])
        raise
