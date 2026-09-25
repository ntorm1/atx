"""Historical identity evidence contract (L2 §5.1): labels, validators and audits.

Two tables, created by migration 0327, carry the certification track:

``security_identity_evidence``
    One row per source-stated (or explicitly inferred) identity fact about one
    instrument. The fact kinds are ``issuer_link``, ``security_type``,
    ``primary_listing``, ``symbol_mapping``, ``delisting_effective`` and
    ``terminal_return``. Each row keeps a namespaced native key
    (``native_key_namespace`` + ``native_key``; ticker text is never globally
    unique across time) and the raw value payload (``value_json``, raw values
    and timezone text preserved; ``source_time_text`` keeps the raw source
    timestamp). Economic validity is ``[valid_from, valid_to)`` with an
    **exclusive** end. Four separate clocks are kept: ``source_published_at``
    (the source's publication), ``observed_at`` (local retrieval),
    ``available_at`` (PIT availability) and the validity interval. Provenance
    is ``source_locator`` (source/member/row or accession locator),
    ``artifact_sha256`` and ``source_revision_id``.

``historical_security_decisions``
    One row per candidate security per decision session (run-scoped): the
    selected ``identity_evidence_id`` / ``type_evidence_id`` /
    ``venue_evidence_id``, normalized ``cik``, ``security_type``,
    ``share_class`` and ``primary_mic``, the cutoff ``decision_at``, an
    ``eligibility`` and a specific ``reason`` for every disposition. It is the
    *strict* verified basis only; a labelled reconstructed research view is a
    separate projection and must never be substituted into it.

Evidence labels (``evidence_status``):

* ``verified_dated`` -- the retained authoritative artifact states the dated
  fact at security grain. It does not by itself mean verified availability.
* ``snapshot`` -- valid at the documented observation only: a single day
  ``[valid_from, valid_from + 1)`` no later than the observation date and not
  backdated before the day the snapshot was published (or observed, when
  publication is unknown; one day of timezone slack), never available before
  that publication/observation. Forward carry is a separate ``inferred`` row.
* ``reconstructed`` -- a later vendor file describing earlier intervals; its
  dates are kept, its vintage qualified by the clocks.
* ``inferred`` -- price cessation, current-symbol joins, heuristics; never
  silently promoted.
* ``unknown`` / ``conflicting`` -- no accepted fact; the row and a
  ``rejection_reason`` are preserved.

Availability (``availability_status``) is labelled separately: ``verified``
requires validated publication evidence (``source_published_at``) and an
``available_at`` no earlier than it; ``modeled`` and ``unknown`` never count
toward certification.

Value payload conventions checked against decisions: ``issuer_link`` carries
the CIK in the ``cik`` column (payload may add ``owner_security_id`` and
``share_class_symbol``, matching ``market_owner_bridge.OwnerLinkEvidence``);
``security_type`` carries ``$.security_type`` and ``$.share_class``;
``primary_listing`` carries ``$.primary_mic``.

Hard invariants are also CHECK constraints in the 0327 DDL (status
vocabularies, interval order, verified proof/clock completeness, publication
never after availability, SHA-256 and ten-digit CIK formats, rejection-reason
pairing). The row rules below restate them plus policy rules (fact-kind
vocabulary, snapshot point-in-time rules, observed-before-published, JSON
payload); the audits add cross-row checks (duplicates, conflicting overlapping
intervals, the L2 SQL C proof checks and decision/proof value agreement).
Existing universe outputs use *inclusive* end dates: convert explicitly at the
boundary with :func:`exclusive_valid_to` / :func:`inclusive_valid_to`.
"""

from __future__ import annotations

import datetime as dt
from collections.abc import Iterable, Mapping, Sequence

import duckdb

EVIDENCE_TABLE = "security_identity_evidence"
DECISIONS_TABLE = "historical_security_decisions"

FACT_KINDS = (
    "issuer_link",
    "security_type",
    "primary_listing",
    "symbol_mapping",
    "delisting_effective",
    "terminal_return",
)
EVIDENCE_STATUSES = ("verified_dated", "snapshot", "reconstructed", "inferred", "unknown", "conflicting")
ACCEPTED_EVIDENCE_STATUSES = ("verified_dated", "snapshot", "reconstructed", "inferred")
REJECTED_EVIDENCE_STATUSES = ("unknown", "conflicting")
AVAILABILITY_STATUSES = ("verified", "modeled", "unknown")
DECISION_ELIGIBILITIES = ("eligible", "excluded", "unresolved")
# Decision proof column -> the evidence fact kind it must reference (L2 SQL C).
DECISION_PROOF_COLUMNS = (
    ("identity_evidence_id", "issuer_link"),
    ("type_evidence_id", "security_type"),
    ("venue_evidence_id", "primary_listing"),
)
_OPEN_END = "DATE '9999-12-31'"

# Column order/types of the physical tables (the 0327 DDL adds constraints/defaults).
EVIDENCE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("evidence_id", "VARCHAR"),
    ("fact_kind", "VARCHAR"),
    ("source", "VARCHAR"),
    ("native_key_namespace", "VARCHAR"),
    ("native_key", "VARCHAR"),
    ("security_id", "VARCHAR"),
    ("cik", "VARCHAR"),
    ("symbol", "VARCHAR"),
    ("value_json", "VARCHAR"),
    ("valid_from", "DATE"),
    ("valid_to", "DATE"),
    ("source_locator", "VARCHAR"),
    ("artifact_sha256", "VARCHAR"),
    ("source_revision_id", "VARCHAR"),
    ("source_time_text", "VARCHAR"),
    ("source_published_at", "TIMESTAMP"),
    ("observed_at", "TIMESTAMP"),
    ("available_at", "TIMESTAMP"),
    ("evidence_status", "VARCHAR"),
    ("availability_status", "VARCHAR"),
    ("method", "VARCHAR"),
    ("rejection_reason", "VARCHAR"),
    ("is_latest_revision", "BOOLEAN"),
    ("run_id", "VARCHAR"),
    ("source_loaded_at", "TIMESTAMP"),
)
DECISION_COLUMNS: tuple[tuple[str, str], ...] = (
    ("run_id", "VARCHAR"),
    ("universe_id", "VARCHAR"),
    ("security_id", "VARCHAR"),
    ("decision_date", "DATE"),
    ("decision_at", "TIMESTAMP"),
    ("decision_rule_version", "VARCHAR"),
    ("eligibility", "VARCHAR"),
    ("reason", "VARCHAR"),
    ("identity_evidence_id", "VARCHAR"),
    ("type_evidence_id", "VARCHAR"),
    ("venue_evidence_id", "VARCHAR"),
    ("cik", "VARCHAR"),
    ("symbol", "VARCHAR"),
    ("security_type", "VARCHAR"),
    ("share_class", "VARCHAR"),
    ("primary_mic", "VARCHAR"),
    ("available_at", "TIMESTAMP"),
    ("details_json", "VARCHAR"),
    ("is_latest_revision", "BOOLEAN"),
    ("source_loaded_at", "TIMESTAMP"),
)


def _in_list(values: Sequence[str]) -> str:
    return ", ".join(f"'{value}'" for value in values)


def _blank(column: str) -> str:
    return f"nullif(trim({column}), '') IS NULL"


# Each predicate is TRUE for a violating row (evaluated under coalesce(..., false)).
EVIDENCE_ROW_RULES: tuple[tuple[str, str], ...] = (
    ("evidence_id_missing", _blank("evidence_id")),
    ("fact_kind_unknown", f"fact_kind IS NULL OR fact_kind NOT IN ({_in_list(FACT_KINDS)})"),
    ("evidence_status_invalid", f"evidence_status IS NULL OR evidence_status NOT IN ({_in_list(EVIDENCE_STATUSES)})"),
    (
        "availability_status_invalid",
        f"availability_status IS NULL OR availability_status NOT IN ({_in_list(AVAILABILITY_STATUSES)})",
    ),
    ("source_missing", _blank("source")),
    ("native_key_missing", f"{_blank('native_key_namespace')} OR {_blank('native_key')}"),
    ("method_missing", _blank("method")),
    ("value_json_invalid", "value_json IS NULL OR NOT json_valid(value_json)"),
    ("invalid_interval", "valid_to IS NOT NULL AND (valid_from IS NULL OR valid_to <= valid_from)"),
    # L2 SQL B (1): verified evidence needs its artifact, locator and a dated start.
    (
        "verified_missing_proof",
        "evidence_status = 'verified_dated' AND ("
        f"{_blank('artifact_sha256')} OR {_blank('source_locator')} OR valid_from IS NULL)",
    ),
    # L2 SQL B (2): verified availability needs publication evidence at or before availability.
    (
        "verified_availability_missing_clock",
        "availability_status = 'verified' AND (source_published_at IS NULL OR available_at IS NULL "
        "OR available_at < source_published_at)",
    ),
    ("available_before_published", "available_at < source_published_at"),
    (
        "artifact_sha256_malformed",
        "artifact_sha256 IS NOT NULL AND NOT regexp_full_match(artifact_sha256, '[0-9a-f]{64}')",
    ),
    ("cik_malformed", "cik IS NOT NULL AND NOT regexp_full_match(cik, '[0-9]{10}')"),
    (
        "rejected_without_reason",
        f"evidence_status IN ({_in_list(REJECTED_EVIDENCE_STATUSES)}) AND {_blank('rejection_reason')}",
    ),
    (
        "accepted_with_rejection_reason",
        f"evidence_status IN ({_in_list(ACCEPTED_EVIDENCE_STATUSES)}) AND rejection_reason IS NOT NULL",
    ),
    # A snapshot asserts its documented day only and is never known before it was published/observed.
    (
        "snapshot_not_point_in_time",
        "evidence_status = 'snapshot' AND (observed_at IS NULL OR valid_from IS NULL "
        "OR valid_to IS DISTINCT FROM valid_from + 1 OR valid_from > CAST(observed_at AS DATE))",
    ),
    # No backward extension: the documented day is the publication (else observation) day; one
    # day of slack covers a US-evening file whose UTC publication clock is the next day.
    (
        "snapshot_backdated",
        "evidence_status = 'snapshot' AND valid_from < CAST(coalesce(source_published_at, observed_at) AS DATE) - 1",
    ),
    (
        "snapshot_available_before_observation",
        "evidence_status = 'snapshot' AND (available_at IS NULL "
        "OR available_at < coalesce(source_published_at, observed_at))",
    ),
    # Conflicting clocks are labelled 'conflicting', never accepted.
    (
        "observed_before_published",
        f"evidence_status IN ({_in_list(ACCEPTED_EVIDENCE_STATUSES)}) AND observed_at < source_published_at",
    ),
)

DECISION_ROW_RULES: tuple[tuple[str, str], ...] = (
    ("eligibility_invalid", f"eligibility IS NULL OR eligibility NOT IN ({_in_list(DECISION_ELIGIBILITIES)})"),
    ("reason_missing", _blank("reason")),
    (
        "eligible_missing_proof_ids",
        "eligibility = 'eligible' AND (identity_evidence_id IS NULL OR type_evidence_id IS NULL "
        "OR venue_evidence_id IS NULL)",
    ),
    (
        "eligible_available_after_cutoff",
        "eligibility = 'eligible' AND (available_at IS NULL OR decision_at IS NULL OR available_at > decision_at)",
    ),
    ("cik_malformed", "cik IS NOT NULL AND NOT regexp_full_match(cik, '[0-9]{10}')"),
)

# Exact L2 §7 acceptance SQL B (expected: both zero).
ACCEPTANCE_SQL_B = (
    """
SELECT count(*) AS invalid_verified_evidence
FROM security_identity_evidence
WHERE evidence_status='verified_dated'
  AND (evidence_id IS NULL OR nullif(artifact_sha256,'') IS NULL
       OR nullif(source_locator,'') IS NULL OR valid_from IS NULL
       OR (valid_to IS NOT NULL AND valid_to <= valid_from))
""",
    """
SELECT count(*) AS invalid_verified_clocks
FROM security_identity_evidence
WHERE availability_status='verified'
  AND (source_published_at IS NULL OR available_at IS NULL
       OR available_at < source_published_at)
""",
)

# Exact L2 §7 acceptance SQL C (1) and (3) (expected zero) and (2) (disposition totals).
ACCEPTANCE_SQL_C_REJECTED_PROOFS = """
WITH refs AS (
  SELECT security_id,decision_date,decision_at,identity_evidence_id AS evidence_id,
         'issuer_link' AS expected_kind
  FROM historical_security_decisions WHERE eligibility='eligible'
  UNION ALL
  SELECT security_id,decision_date,decision_at,type_evidence_id,'security_type'
  FROM historical_security_decisions WHERE eligibility='eligible'
  UNION ALL
  SELECT security_id,decision_date,decision_at,venue_evidence_id,'primary_listing'
  FROM historical_security_decisions WHERE eligibility='eligible'
)
SELECT count(*) AS rejected_eligible_proofs
FROM refs r LEFT JOIN security_identity_evidence e USING(evidence_id)
WHERE e.evidence_id IS NULL OR e.fact_kind IS DISTINCT FROM r.expected_kind
   OR e.evidence_status IS DISTINCT FROM 'verified_dated'
   OR e.availability_status IS DISTINCT FROM 'verified'
   OR e.available_at IS NULL OR e.available_at > r.decision_at
   OR e.valid_from IS NULL OR e.valid_from > r.decision_date
   OR (e.valid_to IS NOT NULL AND e.valid_to <= r.decision_date)
"""
ACCEPTANCE_SQL_C_DISPOSITIONS = """
SELECT year(decision_date) AS year, eligibility, reason, count(*) AS security_days,
       count(DISTINCT security_id) AS securities
FROM historical_security_decisions
GROUP BY year(decision_date),eligibility,reason ORDER BY 1,2,3
"""
ACCEPTANCE_SQL_C_MISSING_DISPOSITIONS = """
SELECT count(*) AS missing_dispositions
FROM historical_security_decisions
WHERE eligibility IS NULL OR nullif(reason,'') IS NULL
"""


def normalize_cik(value: object) -> str:
    """Return a ten-digit CIK; reject anything that is not 1-10 ASCII digits.

    Invalid identifiers fail here instead of being coerced into plausible ones
    (no stripping of letters, no truncation of long numbers).
    """
    if isinstance(value, bool) or value is None:
        raise ValueError(f"invalid CIK: {value!r}")
    text = str(value).strip() if not isinstance(value, int) else str(value)
    if not text or len(text) > 10 or not text.isascii() or not text.isdigit() or int(text) == 0:
        raise ValueError(f"invalid CIK: {value!r}")
    return text.zfill(10)


def exclusive_valid_to(inclusive_end: dt.date | None) -> dt.date | None:
    """Convert an inclusive end date (universe outputs) to the evidence exclusive end."""
    return None if inclusive_end is None else inclusive_end + dt.timedelta(days=1)


def inclusive_valid_to(exclusive_end: dt.date | None) -> dt.date | None:
    """Convert an evidence exclusive end to an inclusive end date (universe outputs)."""
    return None if exclusive_end is None else exclusive_end - dt.timedelta(days=1)


def interval_covers(valid_from: dt.date | None, valid_to: dt.date | None, day: dt.date) -> bool:
    """True when ``day`` lies in ``[valid_from, valid_to)`` (``None`` end = open)."""
    return valid_from is not None and valid_from <= day and (valid_to is None or day < valid_to)


def _rule_union(table: str, rules: Sequence[tuple[str, str]], key: str) -> str:
    return "\nUNION ALL\n".join(
        f"SELECT {key} AS row_key, '{name}' AS rule FROM {table} WHERE coalesce(({predicate}), false)"
        for name, predicate in rules
    )


def _stage(
    con: duckdb.DuckDBPyConnection,
    table: str,
    rows: Iterable[Mapping[str, object]],
    columns: Sequence[tuple[str, str]],
) -> None:
    """Load plain rows into in-memory table ``table`` (``row_index`` + typed columns)."""
    definitions = ", ".join(f"{name} {data_type}" for name, data_type in columns)
    con.execute(f"CREATE TABLE {table} (row_index BIGINT, {definitions})")
    names = [name for name, _ in columns]
    records = []
    for index, row in enumerate(rows):
        unknown = sorted(set(row) - set(names))
        if unknown:
            raise ValueError(f"row {index} has unknown columns: {unknown}")
        records.append([index, *(row.get(name) for name in names)])
    if records:
        placeholders = ", ".join("?" for _ in range(len(names) + 1))
        con.executemany(f"INSERT INTO {table} VALUES ({placeholders})", records)


def _memory_connection() -> duckdb.DuckDBPyConnection:
    return duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": 1})


def evidence_row_violations(rows: Iterable[Mapping[str, object]]) -> list[tuple[int, str]]:
    """Row-level evidence violations as sorted ``(row_index, rule)`` pairs (empty = valid)."""
    con = _memory_connection()
    try:
        _stage(con, "stage", rows, EVIDENCE_COLUMNS)
        found = con.execute(_rule_union("stage", EVIDENCE_ROW_RULES, "row_index")).fetchall()
    finally:
        con.close()
    return sorted((int(index), str(rule)) for index, rule in found)


def decision_row_violations(
    decisions: Iterable[Mapping[str, object]],
    evidence: Iterable[Mapping[str, object]] = (),
) -> list[tuple[int, str]]:
    """Row-level and proof violations for decisions against the given evidence rows."""
    con = _memory_connection()
    try:
        _stage(con, "stage", decisions, DECISION_COLUMNS)
        _stage(con, "evidence", evidence, EVIDENCE_COLUMNS)
        found = con.execute(
            _rule_union("stage", DECISION_ROW_RULES, "row_index")
            + "\nUNION ALL\n"
            + _decision_proof_rules_sql("stage", "evidence", "d.row_index")
        ).fetchall()
    finally:
        con.close()
    return sorted((int(index), str(rule)) for index, rule in found)


def _decision_proof_rules_sql(decisions: str, evidence: str, key: str) -> str:
    """Per-decision proof violations: L2 SQL C (1) plus value agreement and availability."""
    refs = "\nUNION ALL\n".join(
        f"SELECT d.*, d.{column} AS proof_id, '{kind}' AS expected_kind FROM {decisions} d "
        "WHERE d.eligibility = 'eligible'"
        for column, kind in DECISION_PROOF_COLUMNS
    )
    return f"""
SELECT DISTINCT {key} AS row_key, 'rejected_eligible_proof' AS rule
FROM ({refs}) d LEFT JOIN {evidence} e ON e.evidence_id = d.proof_id
WHERE e.evidence_id IS NULL OR e.fact_kind IS DISTINCT FROM d.expected_kind
   OR e.evidence_status IS DISTINCT FROM 'verified_dated'
   OR e.availability_status IS DISTINCT FROM 'verified'
   OR e.available_at IS NULL OR e.available_at > d.decision_at
   OR e.valid_from IS NULL OR e.valid_from > d.decision_date
   OR (e.valid_to IS NOT NULL AND e.valid_to <= d.decision_date)
UNION ALL
SELECT DISTINCT {key} AS row_key, 'proof_value_mismatch' AS rule
FROM ({refs}) d JOIN {evidence} e ON e.evidence_id = d.proof_id
WHERE e.security_id IS DISTINCT FROM d.security_id
   OR (d.expected_kind = 'issuer_link' AND e.cik IS DISTINCT FROM d.cik)
   OR (d.expected_kind = 'security_type' AND (
        json_extract_string(e.value_json, '$.security_type') IS DISTINCT FROM d.security_type
        OR json_extract_string(e.value_json, '$.share_class') IS DISTINCT FROM d.share_class))
   OR (d.expected_kind = 'primary_listing'
        AND json_extract_string(e.value_json, '$.primary_mic') IS DISTINCT FROM d.primary_mic)
UNION ALL
SELECT {key} AS row_key, 'decision_available_before_proofs' AS rule
FROM ({refs}) d JOIN {evidence} e ON e.evidence_id = d.proof_id
GROUP BY ALL
HAVING bool_or(d.available_at IS NULL OR d.available_at < e.available_at)"""


def _counts(con: duckdb.DuckDBPyConnection, sql: str, rules: Sequence[str]) -> dict[str, int]:
    counts = dict.fromkeys(rules, 0)
    for rule, count in con.execute(f"SELECT rule, count(*) FROM ({sql}) GROUP BY rule").fetchall():
        counts[str(rule)] = int(count)
    return counts


def audit_identity_evidence(con: duckdb.DuckDBPyConnection, *, table: str = EVIDENCE_TABLE) -> dict[str, int]:
    """Count violating rows per rule over an evidence table (every value 0 = clean).

    Row rules are :data:`EVIDENCE_ROW_RULES`; cross-row rules: duplicate
    ``evidence_id``; accepted latest facts of one security and kind whose
    intervals overlap with different values; and a symbol mapped to two
    securities over overlapping intervals (ticker reuse must not overlap). An
    empty table passes -- coverage is a separate gate.
    """
    accepted = f"evidence_status IN ({_in_list(ACCEPTED_EVIDENCE_STATUSES)}) AND coalesce(is_latest_revision, true)"
    overlap = (
        f"a.valid_from < coalesce(b.valid_to, {_OPEN_END}) AND b.valid_from < coalesce(a.valid_to, {_OPEN_END})"
    )
    cross = f"""
SELECT evidence_id AS row_key, 'duplicate_evidence_id' AS rule
FROM {table} GROUP BY evidence_id HAVING count(*) > 1
UNION ALL
SELECT DISTINCT a.evidence_id, 'conflicting_security_interval'
FROM (SELECT * FROM {table} WHERE {accepted} AND security_id IS NOT NULL AND valid_from IS NOT NULL) a
JOIN (SELECT * FROM {table} WHERE {accepted} AND security_id IS NOT NULL AND valid_from IS NOT NULL) b
  ON a.security_id = b.security_id AND a.fact_kind = b.fact_kind AND a.evidence_id <> b.evidence_id
 AND {overlap}
 AND (a.value_json IS DISTINCT FROM b.value_json OR a.cik IS DISTINCT FROM b.cik)
UNION ALL
SELECT DISTINCT a.evidence_id, 'symbol_reused_while_active'
FROM (SELECT * FROM {table} WHERE {accepted} AND fact_kind = 'symbol_mapping' AND valid_from IS NOT NULL) a
JOIN (SELECT * FROM {table} WHERE {accepted} AND fact_kind = 'symbol_mapping' AND valid_from IS NOT NULL) b
  ON a.native_key_namespace = b.native_key_namespace AND a.native_key = b.native_key
 AND a.evidence_id <> b.evidence_id AND a.security_id IS DISTINCT FROM b.security_id
 AND {overlap}"""
    rules = [name for name, _ in EVIDENCE_ROW_RULES] + [
        "duplicate_evidence_id",
        "conflicting_security_interval",
        "symbol_reused_while_active",
    ]
    return _counts(con, _rule_union(table, EVIDENCE_ROW_RULES, "evidence_id") + "\nUNION ALL\n" + cross, rules)


def audit_security_decisions(
    con: duckdb.DuckDBPyConnection,
    *,
    table: str = DECISIONS_TABLE,
    evidence_table: str = EVIDENCE_TABLE,
) -> dict[str, int]:
    """Count violating decision rows per rule (every value 0 = clean).

    Adds to the row rules: L2 SQL C (1) per eligible decision
    (``rejected_eligible_proof``), decision/proof value agreement
    (security, CIK, type/class, primary MIC), a decision ``available_at``
    earlier than a selected proof's clock, and duplicate decision keys.
    """
    key = "concat_ws('|', d.run_id, d.universe_id, d.security_id, CAST(d.decision_date AS VARCHAR))"
    row_key = "concat_ws('|', run_id, universe_id, security_id, CAST(decision_date AS VARCHAR))"
    sql = (
        _rule_union(table, DECISION_ROW_RULES, row_key)
        + "\nUNION ALL\n"
        + _decision_proof_rules_sql(table, evidence_table, key)
        + f"""
UNION ALL
SELECT {row_key} AS row_key, 'duplicate_decision' AS rule
FROM {table} GROUP BY run_id, universe_id, security_id, decision_date HAVING count(*) > 1"""
    )
    rules = [name for name, _ in DECISION_ROW_RULES] + [
        "rejected_eligible_proof",
        "proof_value_mismatch",
        "decision_available_before_proofs",
        "duplicate_decision",
    ]
    return _counts(con, sql, rules)


def decision_dispositions(con: duckdb.DuckDBPyConnection) -> list[tuple[object, ...]]:
    """L2 SQL C (2): security-days and securities per year, eligibility and reason."""
    return con.execute(ACCEPTANCE_SQL_C_DISPOSITIONS).fetchall()


__all__ = [
    "ACCEPTANCE_SQL_B",
    "ACCEPTANCE_SQL_C_DISPOSITIONS",
    "ACCEPTANCE_SQL_C_MISSING_DISPOSITIONS",
    "ACCEPTANCE_SQL_C_REJECTED_PROOFS",
    "ACCEPTED_EVIDENCE_STATUSES",
    "AVAILABILITY_STATUSES",
    "DECISIONS_TABLE",
    "DECISION_COLUMNS",
    "DECISION_ELIGIBILITIES",
    "DECISION_PROOF_COLUMNS",
    "DECISION_ROW_RULES",
    "EVIDENCE_COLUMNS",
    "EVIDENCE_ROW_RULES",
    "EVIDENCE_STATUSES",
    "EVIDENCE_TABLE",
    "FACT_KINDS",
    "REJECTED_EVIDENCE_STATUSES",
    "audit_identity_evidence",
    "audit_security_decisions",
    "decision_dispositions",
    "decision_row_violations",
    "evidence_row_violations",
    "exclusive_valid_to",
    "inclusive_valid_to",
    "interval_covers",
    "normalize_cik",
]
