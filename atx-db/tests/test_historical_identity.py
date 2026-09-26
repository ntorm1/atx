"""A9 / migration 0327: identity evidence contract, row labels, est_actual period key.

The bundle is exercised through its migration body so the same tests hold before
and after 0327 is registered. Tests that need a genuine 0326 warehouse use
``warehouse_0326``: the cached test template while 0327 is unregistered (it *is*
a 0326 bootstrap), otherwise a bootstrap with the registry truncated at 0326
(slow lane only).
"""

from __future__ import annotations

import datetime as dt
import json
import shutil
import types

import duckdb
import pytest

import atx_db.migrations as migrations_pkg
from atx_db import historical_identity as hi
from atx_db._forward_return_publication import CALCULATION_VERSION
from atx_db.connection import DuckDBStore
from atx_db.estimates import EstimateMeasureSeedDataset, EstimateMeasureSeedOptions
from atx_db.migration_admin import verify_schema
from atx_db.migrations import bodies_0327 as b0327
from atx_db.quality import pit_column_presence_check
from atx_db.schema_contract import build_contract_manifest

SHA = "ab" * 32
NEW_TABLES = {"security_identity_evidence", "historical_security_decisions"}


def _registered_0327() -> bool:
    return any(migration.version == 327 for migration in migrations_pkg.MIGRATIONS)


def _connect(path) -> duckdb.DuckDBPyConnection:
    con = duckdb.connect(str(path), config={"memory_limit": "512MB", "threads": 1})
    con.execute("SET TimeZone='UTC'")
    return con


@pytest.fixture(scope="module")
def warehouse_0326(request, tmp_path_factory):
    path = tmp_path_factory.mktemp("w0326") / "w0326.duckdb"
    if not _registered_0327():
        shutil.copyfile(request.getfixturevalue("_schema_template"), path)
        return path
    if not request.config.getoption("--run-slow"):
        pytest.skip("a 0326 warehouse needs a truncated bootstrap once 0327 is registered; pass --run-slow")
    full = migrations_pkg.MIGRATIONS
    migrations_pkg.MIGRATIONS = [migration for migration in full if migration.version <= 326]
    store = DuckDBStore(path)
    store.connection = duckdb.connect(str(path), config={"memory_limit": "256MB", "threads": 1})
    try:
        store._configure_session(store.connection)
        store.initialize()
    finally:
        migrations_pkg.MIGRATIONS = full
        store.connection.close()
    return path


@pytest.fixture
def con_0326(warehouse_0326, tmp_path):
    path = tmp_path / "w.duckdb"
    shutil.copyfile(warehouse_0326, path)
    con = _connect(path)
    assert con.execute(
        "SELECT max(CAST(version AS INTEGER)) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchone() == (326,)
    try:
        yield con
    finally:
        con.close()


def _apply_0327(con: duckdb.DuckDBPyConnection) -> None:
    """Exactly what the runner does: the body inside one transaction."""
    con.execute("BEGIN TRANSACTION")
    try:
        b0327._pre_run5_identity_bundle(con)
    except Exception:
        con.execute("ROLLBACK")
        raise
    con.execute("COMMIT")


def _in_transaction(con, body) -> None:
    con.execute("BEGIN TRANSACTION")
    try:
        body(con)
    except Exception:
        con.execute("ROLLBACK")
        raise
    con.execute("COMMIT")


def _schema_snapshot(con):
    tables = {}
    names = con.execute(
        """
        SELECT table_name FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = 'main' AND NOT internal AND NOT temporary
        """
    ).fetchall()
    for (name,) in names:
        columns = con.execute(
            """
            SELECT column_name, data_type, is_nullable, column_default FROM duckdb_columns()
            WHERE database_name = current_database() AND table_name = ? ORDER BY column_index
            """,
            [name],
        ).fetchall()
        constraints = sorted(
            (kind, tuple(cols or ()), expression or "")
            for kind, cols, expression in con.execute(
                """
                SELECT constraint_type, constraint_column_names, expression FROM duckdb_constraints()
                WHERE database_name = current_database() AND table_name = ?
                """,
                [name],
            ).fetchall()
        )
        indexes = con.execute(
            "SELECT index_name, sql FROM duckdb_indexes() WHERE table_name = ? ORDER BY 1", [name]
        ).fetchall()
        tables[name] = (columns, constraints, indexes)
    views = dict(
        con.execute(
            "SELECT view_name, sql FROM duckdb_views() WHERE NOT internal AND schema_name = 'main' AND NOT temporary"
        ).fetchall()
    )
    return tables, views


def _primary_key(con, table):
    for kind, cols, _ in _schema_snapshot_table_constraints(con, table):
        if kind == "PRIMARY KEY":
            return tuple(cols)
    return ()


def _schema_snapshot_table_constraints(con, table):
    return con.execute(
        "SELECT constraint_type, constraint_column_names, expression FROM duckdb_constraints() WHERE table_name = ?",
        [table],
    ).fetchall()


# --------------------------------------------------------------------------- migration shape


def test_0327_changes_exactly_the_bundle_schema_and_is_idempotent(con_0326):
    before_tables, before_views = _schema_snapshot(con_0326)
    _apply_0327(con_0326)
    after_tables, after_views = _schema_snapshot(con_0326)

    assert set(after_tables) - set(before_tables) == NEW_TABLES
    assert set(before_tables) - set(after_tables) == set()
    changed = {name for name in before_tables if before_tables[name] != after_tables[name]}
    assert changed == {"market_daily_metrics", "delisting_events", "est_actual"}

    for table, added in (
        ("market_daily_metrics", b0327.MARKET_DAILY_LABEL_COLUMNS),
        ("delisting_events", b0327.DELISTING_REVISION_COLUMNS),
    ):
        cols_before, constraints_before, indexes_before = before_tables[table]
        cols_after, constraints_after, indexes_after = after_tables[table]
        assert cols_after[: len(cols_before)] == cols_before
        assert cols_after[len(cols_before):] == [(name, kind, True, None) for name, kind in added]
        assert constraints_after == constraints_before
        assert indexes_after == indexes_before

    cols_before, constraints_before, indexes_before = before_tables["est_actual"]
    cols_after, constraints_after, indexes_after = after_tables["est_actual"]
    assert cols_after[: len(cols_before)] == cols_before
    assert cols_after[len(cols_before):] == [
        ("period_start", "DATE", False, None),
        ("duration_days", "INTEGER", False, None),
    ]
    assert indexes_after == indexes_before
    assert _primary_key(con_0326, "est_actual") == b0327.EST_ACTUAL_KEY
    added_constraints = set(constraints_after) - set(constraints_before)
    assert {kind for kind, _, _ in added_constraints} == {"PRIMARY KEY", "NOT NULL", "CHECK"}

    assert {name for name in before_views if before_views[name] != after_views.get(name)} == {
        "v_delisting_return_coverage"
    }
    assert set(after_views) == set(before_views)

    # A second application changes nothing (runner retries, or a body re-run).
    _apply_0327(con_0326)
    assert _schema_snapshot(con_0326) == (after_tables, after_views)

    # The persisted schema contract pin matches the widened schema; PIT presence holds.
    assert verify_schema(con_0326) == ()
    presence = pit_column_presence_check(types.SimpleNamespace(con=con_0326))
    assert presence.status == "passed", presence.details
    manifest = build_contract_manifest(con_0326)
    for table in NEW_TABLES:
        assert all(spec.unit and spec.sign and spec.scale for spec in manifest[table]), table


def test_0327_commit_is_replayable_from_the_wal(con_0326, tmp_path):
    """DuckDB 1.5.5 cannot replay ALTER on DEFAULT now() tables; 0327 must not need it."""
    path = tmp_path / "w.duckdb"
    con_0326.execute("CHECKPOINT")
    con_0326.execute("PRAGMA disable_checkpoint_on_shutdown")
    con_0326.execute("SET checkpoint_threshold = '10GB'")  # keep the whole commit in the WAL
    _apply_0327(con_0326)
    con_0326.close()
    assert path.with_suffix(".duckdb.wal").exists()

    reopened = _connect(path)  # WAL replay happens here
    try:
        assert _primary_key(reopened, "est_actual") == b0327.EST_ACTUAL_KEY
        names = {row[0] for row in reopened.execute("SELECT table_name FROM duckdb_tables()").fetchall()}
        assert names >= NEW_TABLES
        assert reopened.execute(
            "SELECT count(*) FROM duckdb_columns() WHERE table_name = 'market_daily_metrics' "
            "AND column_name IN ('owner_security_id','identity_basis','availability_basis','link_method')"
        ).fetchone() == (4,)
    finally:
        reopened.close()


# --------------------------------------------------------------------------- evidence rows


def _evidence(**overrides):
    row = {
        "evidence_id": "ev-issuer-A",
        "fact_kind": "issuer_link",
        "source": "sec_submissions",
        "native_key_namespace": "sec_cik",
        "native_key": "0000000001",
        "security_id": "SEC-CIK-0000000001",
        "cik": "0000000001",
        "symbol": "ABC",
        "value_json": '{"cik": "0000000001"}',
        "valid_from": dt.date(2010, 1, 4),
        "valid_to": None,
        "source_locator": "submissions/CIK0000000001.json#filings.recent[3]",
        "artifact_sha256": SHA,
        "source_revision_id": "rev-1",
        "source_time_text": "2010-01-04T16:31:02-05:00",
        "source_published_at": dt.datetime(2010, 1, 4, 21, 31, 2),
        "observed_at": dt.datetime(2026, 9, 20, 10, 0),
        "available_at": dt.datetime(2010, 1, 4, 21, 31, 2),
        "evidence_status": "verified_dated",
        "availability_status": "verified",
        "method": "sec_form_8a12b_registration",
        "rejection_reason": None,
        "run_id": "a9-test",
    }
    row.update(overrides)
    return row


def _snapshot_row(**overrides):
    row = _evidence(
        evidence_id="ev-snapshot",
        fact_kind="symbol_mapping",
        source="nasdaq_symbol_directory",
        native_key_namespace="nasdaq_symbol",
        native_key="ABC",
        value_json='{"symbol": "ABC"}',
        valid_from=dt.date(2026, 9, 18),
        valid_to=dt.date(2026, 9, 19),
        source_locator="nasdaqlisted.txt#row=17",
        source_time_text="File Creation Time: 0918202621:31",
        source_published_at=dt.datetime(2026, 9, 19, 1, 31),
        observed_at=dt.datetime(2026, 9, 20, 10, 0),
        available_at=dt.datetime(2026, 9, 20, 10, 0),
        evidence_status="snapshot",
        method="nasdaq_directory_snapshot",
    )
    row.update(overrides)
    return row


VALID_EVIDENCE = [
    _evidence(),
    _snapshot_row(),
    _evidence(
        evidence_id="ev-inferred", fact_kind="delisting_effective", evidence_status="inferred",
        availability_status="modeled", artifact_sha256=None, source_locator=None, source_published_at=None,
        value_json='{"delist_date": "2019-03-25"}', method="price_cessation_gap_31",
        available_at=dt.datetime(2019, 5, 6, 22, 0),
    ),
    _evidence(
        evidence_id="ev-unknown", evidence_status="unknown", availability_status="unknown",
        rejection_reason="zero_instrument_id", artifact_sha256=None, valid_from=None,
        source_published_at=None, available_at=None,
    ),
]


def test_valid_evidence_rows_pass_validator_checks_and_acceptance_sql_b(tmp_store):
    assert hi.evidence_row_violations(VALID_EVIDENCE) == []
    b0327.create_historical_identity_tables(tmp_store.con)
    _insert(tmp_store.con, "security_identity_evidence", VALID_EVIDENCE)
    for sql in hi.ACCEPTANCE_SQL_B:
        assert tmp_store.con.execute(sql).fetchone() == (0,)
    assert set(hi.audit_identity_evidence(tmp_store.con).values()) == {0}


@pytest.mark.parametrize(
    ("overrides", "rules"),
    [
        ({"artifact_sha256": None}, {"verified_missing_proof"}),
        ({"source_locator": " "}, {"verified_missing_proof"}),
        ({"valid_from": None}, {"verified_missing_proof"}),
        ({"valid_to": dt.date(2010, 1, 4)}, {"invalid_interval"}),
        ({"valid_to": dt.date(2009, 12, 31)}, {"invalid_interval"}),
        ({"source_published_at": None}, {"verified_availability_missing_clock"}),
        ({"available_at": None}, {"verified_availability_missing_clock"}),
        (
            {"available_at": dt.datetime(2010, 1, 4, 21, 31, 1)},
            {"verified_availability_missing_clock", "available_before_published"},
        ),
        ({"artifact_sha256": "AB" * 32}, {"artifact_sha256_malformed"}),
        ({"cik": "320193"}, {"cik_malformed"}),
        ({"evidence_status": "verified"}, {"evidence_status_invalid"}),
        ({"availability_status": "assumed"}, {"availability_status_invalid"}),
        ({"fact_kind": "cusip_mapping"}, {"fact_kind_unknown"}),
        ({"value_json": "{cik: 1}"}, {"value_json_invalid"}),
        ({"rejection_reason": "looks odd"}, {"accepted_with_rejection_reason"}),
        ({"evidence_status": "conflicting"}, {"rejected_without_reason"}),
        ({"observed_at": dt.datetime(2010, 1, 4, 20, 0)}, {"observed_before_published"}),
        ({"native_key_namespace": ""}, {"native_key_missing"}),
    ],
)
def test_validators_reject_each_invalid_evidence_row(overrides, rules):
    assert hi.evidence_row_violations([_evidence(**overrides)]) == [(0, rule) for rule in sorted(rules)]


@pytest.mark.parametrize(
    ("overrides", "rules"),
    [
        # Backdated: a 2026 observation cannot assert a 2012 state.
        ({"valid_from": dt.date(2012, 1, 3), "valid_to": dt.date(2012, 1, 4)}, {"snapshot_backdated"}),
        # Forward carry is an assertion of history, not a snapshot.
        ({"valid_to": None}, {"snapshot_not_point_in_time"}),
        ({"valid_to": dt.date(2026, 9, 25)}, {"snapshot_not_point_in_time"}),
        # Future-dated relative to the observation.
        (
            {"valid_from": dt.date(2026, 9, 21), "valid_to": dt.date(2026, 9, 22)},
            {"snapshot_not_point_in_time"},
        ),
        # Known before it was published.
        (
            {"available_at": dt.datetime(2026, 9, 18, 22, 0)},
            {"snapshot_available_before_observation", "available_before_published",
             "verified_availability_missing_clock"},
        ),
    ],
)
def test_snapshot_evidence_is_point_in_time_and_never_backdated(overrides, rules):
    assert hi.evidence_row_violations([_snapshot_row(**overrides)]) == [(0, rule) for rule in sorted(rules)]


@pytest.mark.parametrize(
    "overrides",
    [
        {"artifact_sha256": None},
        {"source_locator": ""},
        {"valid_from": None},
        {"valid_to": dt.date(2010, 1, 4)},
        {"source_published_at": None},
        {"available_at": dt.datetime(2010, 1, 4, 21, 0)},
        {"cik": "320193"},
        {"evidence_status": "verified"},
        {"evidence_status": "conflicting"},
        {"rejection_reason": "stale"},
    ],
)
def test_warehouse_constraints_reject_invalid_verified_evidence(tmp_store, overrides):
    b0327.create_historical_identity_tables(tmp_store.con)
    with pytest.raises(duckdb.ConstraintException):
        _insert(tmp_store.con, "security_identity_evidence", [_evidence(**overrides)])


def test_acceptance_sql_b_has_teeth_on_an_unconstrained_copy():
    con = duckdb.connect()
    columns = ", ".join(f"{name} {kind}" for name, kind in hi.EVIDENCE_COLUMNS)
    con.execute(f"CREATE TABLE security_identity_evidence ({columns})")
    _insert(con, "security_identity_evidence", [
        _evidence(evidence_id="bad-sha", artifact_sha256=None),
        _evidence(evidence_id="bad-interval", valid_to=dt.date(2009, 1, 1)),
        _evidence(evidence_id="bad-clock", source_published_at=None),
    ])
    assert [con.execute(sql).fetchone() for sql in hi.ACCEPTANCE_SQL_B] == [(2,), (1,)]
    audit = hi.audit_identity_evidence(con)
    assert audit["verified_missing_proof"] == 1
    assert audit["invalid_interval"] == 1
    assert audit["verified_availability_missing_clock"] == 1


def test_physical_tables_match_the_python_contract(tmp_store):
    b0327.create_historical_identity_tables(tmp_store.con)
    for table, expected in (
        ("security_identity_evidence", hi.EVIDENCE_COLUMNS),
        ("historical_security_decisions", hi.DECISION_COLUMNS),
    ):
        actual = tmp_store.con.execute(
            "SELECT column_name, data_type FROM duckdb_columns() WHERE table_name = ? ORDER BY column_index", [table]
        ).fetchall()
        assert tuple(actual) == expected
    # Every label in the Python vocabulary is accepted by the warehouse CHECKs.
    rows = []
    for index, status in enumerate(hi.EVIDENCE_STATUSES):
        rejected = status in hi.REJECTED_EVIDENCE_STATUSES
        rows.append(_evidence(
            evidence_id=f"status-{index}", evidence_status=status,
            rejection_reason="reason" if rejected else None,
            **({"valid_to": dt.date(2010, 1, 5)} if status == "snapshot" else {}),
        ))
    for index, status in enumerate(hi.AVAILABILITY_STATUSES):
        rows.append(_evidence(evidence_id=f"availability-{index}", availability_status=status))
    _insert(tmp_store.con, "security_identity_evidence", rows)


# --------------------------------------------------------------------------- decisions


def _issuer_evidence(prefix, security_id, cik, share_class, *, valid_from=dt.date(2010, 1, 4), valid_to=None):
    published = dt.datetime(2010, 1, 4, 21, 31, 2)
    common = {"security_id": security_id, "cik": cik, "valid_from": valid_from, "valid_to": valid_to}
    return [
        _evidence(evidence_id=f"{prefix}-issuer", value_json=f'{{"cik": "{cik}"}}', **common),
        _evidence(
            evidence_id=f"{prefix}-type", fact_kind="security_type", native_key_namespace="sec_ticker_class",
            native_key=f"{security_id}", value_json=f'{{"security_type": "common", "share_class": "{share_class}"}}',
            **common,
        ),
        _evidence(
            evidence_id=f"{prefix}-venue", fact_kind="primary_listing", native_key_namespace="sec_8a12b",
            native_key=f"{security_id}", value_json='{"primary_mic": "XNYS"}',
            source_published_at=published, available_at=published, **common,
        ),
    ]


def _decision(prefix, security_id, cik, share_class, **overrides):
    row = {
        "run_id": "run-1",
        "universe_id": "us_listed_strict_v1",
        "security_id": security_id,
        "decision_date": dt.date(2014, 6, 2),
        "decision_at": dt.datetime(2014, 6, 2, 22, 0),
        "decision_rule_version": "strict_v1",
        "eligibility": "eligible",
        "reason": "eligible_verified_common",
        "identity_evidence_id": f"{prefix}-issuer",
        "type_evidence_id": f"{prefix}-type",
        "venue_evidence_id": f"{prefix}-venue",
        "cik": cik,
        "symbol": "ABC",
        "security_type": "common",
        "share_class": share_class,
        "primary_mic": "XNYS",
        "available_at": dt.datetime(2010, 1, 4, 21, 31, 2),
        "details_json": None,
    }
    row.update(overrides)
    return row


def test_one_issuer_two_share_classes_are_both_eligible(tmp_store):
    evidence = [
        *_issuer_evidence("A", "SEC-CIK-0000000001", "0000000001", "A"),
        *_issuer_evidence("B", "TBLTICKERHISTORY-7", "0000000001", "B"),
    ]
    decisions = [
        _decision("A", "SEC-CIK-0000000001", "0000000001", "A"),
        _decision("B", "TBLTICKERHISTORY-7", "0000000001", "B"),
        _decision("X", "PX-9", None, None, eligibility="excluded", reason="no_dated_type_evidence",
                  identity_evidence_id=None, type_evidence_id=None, venue_evidence_id=None,
                  security_type=None, primary_mic=None, available_at=None),
    ]
    assert hi.evidence_row_violations(evidence) == []
    assert hi.decision_row_violations(decisions, evidence) == []
    con = tmp_store.con
    b0327.create_historical_identity_tables(con)
    _insert(con, "security_identity_evidence", evidence)
    _insert(con, "historical_security_decisions", decisions)
    assert con.execute(hi.ACCEPTANCE_SQL_C_REJECTED_PROOFS).fetchone() == (0,)
    assert con.execute(hi.ACCEPTANCE_SQL_C_MISSING_DISPOSITIONS).fetchone() == (0,)
    assert set(hi.audit_identity_evidence(con).values()) == {0}
    assert set(hi.audit_security_decisions(con).values()) == {0}
    assert hi.decision_dispositions(con) == [
        (2014, "eligible", "eligible_verified_common", 2, 2),
        (2014, "excluded", "no_dated_type_evidence", 1, 1),
    ]


def test_cik_change_uses_the_exclusive_boundary(tmp_store):
    before = _issuer_evidence("old", "PX-1", "0000000002", "A", valid_to=dt.date(2015, 6, 1))
    after = _issuer_evidence("new", "PX-1", "0000000003", "A", valid_from=dt.date(2015, 6, 1))
    evidence = before + after
    last_old_day = dt.date(2015, 5, 29)
    boundary = dt.date(2015, 6, 1)
    decisions = [
        _decision("old", "PX-1", "0000000002", "A", decision_date=last_old_day,
                  decision_at=dt.datetime(2015, 5, 29, 22)),
        # On the boundary the old CIK is no longer valid (valid_to is exclusive).
        _decision("old", "PX-1", "0000000002", "A", run_id="run-stale", decision_date=boundary,
                  decision_at=dt.datetime(2015, 6, 1, 22)),
        _decision("new", "PX-1", "0000000003", "A", decision_date=boundary,
                  decision_at=dt.datetime(2015, 6, 1, 22)),
        # Proof says CIK ...3 while the decision claims ...2.
        _decision("new", "PX-1", "0000000002", "A", run_id="run-mismatch", decision_date=boundary,
                  decision_at=dt.datetime(2015, 6, 1, 22)),
    ]
    assert hi.decision_row_violations(decisions, evidence) == [
        (1, "rejected_eligible_proof"),
        (3, "proof_value_mismatch"),
    ]
    assert not hi.interval_covers(dt.date(2010, 1, 4), dt.date(2015, 6, 1), boundary)
    assert hi.interval_covers(dt.date(2010, 1, 4), dt.date(2015, 6, 1), last_old_day)
    con = tmp_store.con
    b0327.create_historical_identity_tables(con)
    _insert(con, "security_identity_evidence", evidence)
    _insert(con, "historical_security_decisions", decisions)
    # L2 SQL C counts proof references: all three expired proofs of the stale decision.
    assert con.execute(hi.ACCEPTANCE_SQL_C_REJECTED_PROOFS).fetchone() == (3,)
    audit = hi.audit_security_decisions(con)
    assert (audit["rejected_eligible_proof"], audit["proof_value_mismatch"]) == (1, 1)
    # No interval conflict: the two CIK links are adjacent, not overlapping.
    assert hi.audit_identity_evidence(con)["conflicting_security_interval"] == 0


def test_proof_published_after_the_cutoff_is_rejected():
    evidence = _issuer_evidence("A", "SEC-CIK-0000000001", "0000000001", "A")
    late = dict(evidence[2], source_published_at=dt.datetime(2014, 6, 3, 13, 0),
                available_at=dt.datetime(2014, 6, 3, 13, 0))
    decisions = [_decision("A", "SEC-CIK-0000000001", "0000000001", "A",
                           available_at=dt.datetime(2014, 6, 2, 21, 0))]
    assert hi.decision_row_violations(decisions, [*evidence[:2], late]) == [
        (0, "decision_available_before_proofs"),
        (0, "rejected_eligible_proof"),
    ]


def test_ticker_reuse_is_allowed_only_without_overlap():
    first = _snapshot_row(evidence_id="abc-1", security_id="PX-1", evidence_status="reconstructed",
                          valid_from=dt.date(2010, 1, 4), valid_to=dt.date(2015, 1, 2))
    second = dict(first, evidence_id="abc-2", security_id="PX-2", valid_from=dt.date(2015, 1, 2), valid_to=None)
    con = duckdb.connect()
    columns = ", ".join(f"{name} {kind}" for name, kind in hi.EVIDENCE_COLUMNS)
    con.execute(f"CREATE TABLE security_identity_evidence ({columns})")
    _insert(con, "security_identity_evidence", [first, second])
    assert hi.audit_identity_evidence(con)["symbol_reused_while_active"] == 0
    con.execute("UPDATE security_identity_evidence SET valid_from = DATE '2014-06-02' WHERE evidence_id = 'abc-2'")
    assert hi.audit_identity_evidence(con)["symbol_reused_while_active"] == 2


def test_eligible_decision_without_proofs_is_refused_by_the_warehouse(tmp_store):
    b0327.create_historical_identity_tables(tmp_store.con)
    with pytest.raises(duckdb.ConstraintException):
        _insert(tmp_store.con, "historical_security_decisions",
                [_decision("A", "PX-1", None, None, type_evidence_id=None)])
    with pytest.raises(duckdb.ConstraintException):
        _insert(tmp_store.con, "historical_security_decisions",
                [_decision("A", "PX-1", None, None, available_at=dt.datetime(2014, 6, 3))])
    with pytest.raises(duckdb.ConstraintException):
        _insert(tmp_store.con, "historical_security_decisions",
                [_decision("A", "PX-1", None, None, eligibility="excluded", reason=" ")])


@pytest.mark.parametrize(
    ("value", "expected"),
    [("320193", "0000320193"), (320193, "0000320193"), (" 0000320193 ", "0000320193"), ("1", "0000000001")],
)
def test_normalize_cik_accepts_digit_ciks(value, expected):
    assert hi.normalize_cik(value) == expected


@pytest.mark.parametrize("value", ["CIK320193", "12345678901", "", None, "0", "-5", "32O193", True, 3.5])
def test_normalize_cik_never_coerces_invalid_identifiers(value):
    with pytest.raises(ValueError):
        hi.normalize_cik(value)


def test_inclusive_and_exclusive_ends_convert_at_the_boundary():
    assert hi.exclusive_valid_to(dt.date(2015, 5, 31)) == dt.date(2015, 6, 1)
    assert hi.inclusive_valid_to(dt.date(2015, 6, 1)) == dt.date(2015, 5, 31)
    assert hi.exclusive_valid_to(None) is None and hi.inclusive_valid_to(None) is None


# --------------------------------------------------------------------------- market_daily / delisting labels


def test_market_daily_rows_carry_owner_labels_and_legacy_rows_stay_unlabeled(tmp_store):
    con = tmp_store.con
    _in_transaction(con, b0327._market_daily_labels)
    base = ("market_daily_id, source, security_id, trade_date, available_at, inputs_hash, as_of_date")
    con.execute(f"""
        INSERT INTO market_daily_metrics ({base})
        VALUES ('legacy', 'atx-db daily market panel v1', 'PX-1', DATE '2024-01-02',
                TIMESTAMP '2024-01-02 22:00:00', 'h', DATE '2024-01-02')
    """)
    con.execute(f"""
        INSERT INTO market_daily_metrics ({base}, owner_security_id, identity_basis, availability_basis, link_method)
        VALUES ('bridged', 'atx-db daily market panel v1', 'TBLTICKERHISTORY-7', DATE '2024-01-02',
                TIMESTAMP '2024-01-02 22:00:00', 'h', DATE '2024-01-02',
                'SEC-CIK-0000000001', 'current_ticker_unverified', 'modeled', 'current_sec_ticker')
    """)
    assert con.execute("""
        SELECT market_daily_id, owner_security_id, identity_basis, availability_basis, link_method
        FROM market_daily_metrics ORDER BY 1
    """).fetchall() == [
        ("bridged", "SEC-CIK-0000000001", "current_ticker_unverified", "modeled", "current_sec_ticker"),
        ("legacy", None, None, None, None),
    ]
    described = dict(con.execute("""
        SELECT field_name, description FROM field_catalog
        WHERE table_name = 'market_daily_metrics'
          AND field_name IN ('owner_security_id','identity_basis','availability_basis','link_method')
    """).fetchall())
    assert set(described) == {"owner_security_id", "identity_basis", "availability_basis", "link_method"}
    assert "certification" in described["identity_basis"]


_FOLD_COLUMNS = (
    "delisting_event_id, source, listing_status_source, source_listing_status_id, security_id, symbol, "
    "delist_date, as_of_date, available_at, delist_code, delist_reason, delisting_return_type, "
    "return_policy, return_confidence, evidence_source, evidence_source_table, method, evidence_confidence, "
    "details_json"
)


def _delisting_row(con, event_id, details_json):
    con.execute(
        f"""
        INSERT INTO delisting_events ({_FOLD_COLUMNS})
        VALUES (?, 'public_evidence', 'archive_last_trade', 'x', 'PX-1', 'ABC', DATE '2024-03-25',
                DATE '2024-04-10', TIMESTAMP '2024-04-10 17:00:00', 'gap', 'merger_acquisition',
                'UNOBSERVED', 'none', 'none', 'public_evidence', 'delisting_evidence', 'm', 'low', ?)
        """,
        [event_id, details_json],
    )


def test_delisting_reason_revision_columns_copy_only_stored_fold_values(tmp_store):
    con = tmp_store.con
    _in_transaction(con, b0327._delisting_reason_revisions)
    # Rows written with the fold's own column list (no new columns) still insert.
    _delisting_row(con, "revised", (
        '{"existence_available_at": "2024-03-26 22:00:00", "reason_available_at": "2024-04-10 17:00:00", '
        '"reason_at_existence": "unknown", "reason_revised_after_existence": true}'
    ))
    _delisting_row(con, "other-writer", '{"effective_date_basis": "listing_status"}')
    _delisting_row(con, "not-json", "legacy free text")
    _in_transaction(con, b0327._delisting_reason_revisions)
    assert con.execute("""
        SELECT delisting_event_id, delisting_event_key, existence_available_at, reason_available_at,
               reason_at_existence, reason_revised_after_existence
        FROM delisting_events ORDER BY 1
    """).fetchall() == [
        ("not-json", "not-json", None, None, None, None),
        ("other-writer", "other-writer", None, None, None, None),
        ("revised", "revised", dt.datetime(2024, 3, 26, 22), dt.datetime(2024, 4, 10, 17), "unknown", True),
    ]


# --------------------------------------------------------------------------- est_actual period key

_LEGACY_EST_ACTUAL = """
    CREATE TABLE est_actual (
        security_id VARCHAR NOT NULL, measure_code VARCHAR NOT NULL, fiscal_year INTEGER NOT NULL,
        fiscal_period VARCHAR NOT NULL, period_end DATE NOT NULL, "value" DOUBLE, unit VARCHAR, form VARCHAR,
        accession_number VARCHAR NOT NULL, announce_date DATE, as_of_date DATE NOT NULL, available_at TIMESTAMP,
        source_loaded_at TIMESTAMP NOT NULL DEFAULT now(), run_id VARCHAR, "source" VARCHAR NOT NULL,
        basis VARCHAR, is_latest_revision BOOLEAN,
        PRIMARY KEY (security_id, measure_code, fiscal_year, fiscal_period, accession_number)
    )
"""


def _legacy_est_actual(store):
    """Restore the pre-0327 est_actual shape (whatever the template's version)."""
    store.con.execute("DROP TABLE est_actual")
    store.con.execute(_LEGACY_EST_ACTUAL)
    store.con.execute("CREATE INDEX idx_est_actual_key ON est_actual(security_id, measure_code, period_end)")
    EstimateMeasureSeedDataset().load(store, EstimateMeasureSeedOptions())


def _fact(con, *, concept, start, end, value, accession, fy=2025, fp="Q2", unit="USD/shares", security="PX-1"):
    con.execute(
        """
        INSERT INTO sec_company_facts (source, security_id, cik, taxonomy, concept, unit, period_start,
            period_end, filed_date, fiscal_year, fiscal_period, form, accession_number, "value",
            available_at, source_url)
        VALUES ('SEC companyfacts', ?, '0000000001', 'us-gaap', ?, ?, ?, ?, DATE '2025-08-01', ?, ?, '10-Q', ?, ?,
                TIMESTAMP '2025-08-01 21:00:00', 'https://data.sec.gov/x')
        """,
        [security, concept, unit, start, end, fy, fp, accession, value],
    )


def _legacy_actual(con, *, measure, end, value, accession, fy=2025, fp="Q2", unit="USD/shares"):
    con.execute(
        """
        INSERT INTO est_actual (security_id, measure_code, fiscal_year, fiscal_period, period_end, "value", unit,
            basis, form, accession_number, announce_date, as_of_date, available_at, "source")
        VALUES ('PX-1', ?, ?, ?, ?, ?, ?, 'GAAP', '10-Q', ?, DATE '2025-08-01', ?,
                TIMESTAMP '2025-08-03 22:00:00', 'sec_company_facts')
        """,
        [measure, fy, fp, end, value, unit, accession, end],
    )


def _q2_filing_facts(con):
    q2 = dt.date(2025, 6, 30)
    _fact(con, concept="EarningsPerShareDiluted", start=dt.date(2025, 4, 1), end=q2, value=0.50, accession="A")
    _fact(con, concept="EarningsPerShareDiluted", start=dt.date(2025, 1, 1), end=q2, value=0.90, accession="A")
    _fact(con, concept="EarningsPerShareDiluted", start=dt.date(2024, 4, 1), end=dt.date(2024, 6, 30), value=0.40,
          accession="A")
    _fact(con, concept="Revenues", start=dt.date(2025, 4, 1), end=q2, value=1.0e9, accession="A", unit="USD")


def test_est_actual_rebuild_keeps_every_legacy_row_with_its_proven_geometry(tmp_store):
    con = tmp_store.con
    _legacy_est_actual(tmp_store)
    _q2_filing_facts(con)
    # The collapsed legacy key kept one arbitrary value per filing: here the 6M YTD EPS.
    _legacy_actual(con, measure="EPS_DILUTED", end=dt.date(2025, 6, 30), value=0.90, accession="A")
    _legacy_actual(con, measure="REVENUE", end=dt.date(2025, 6, 30), value=1.0e9, accession="A", unit="USD")

    _in_transaction(con, b0327._est_actual_period_key)

    assert _primary_key(con, "est_actual") == b0327.EST_ACTUAL_KEY
    assert con.execute("""
        SELECT measure_code, period_start, period_end, duration_days, "value" FROM est_actual ORDER BY 1
    """).fetchall() == [
        ("EPS_DILUTED", dt.date(2025, 1, 1), dt.date(2025, 6, 30), 181, 0.90),
        ("REVENUE", dt.date(2025, 4, 1), dt.date(2025, 6, 30), 91, 1.0e9),
    ]
    assert con.execute(
        "SELECT index_name FROM duckdb_indexes() WHERE table_name = 'est_actual'"
    ).fetchall() == [("idx_est_actual_key",)]
    assert con.execute(
        "SELECT natural_key_json FROM table_catalog WHERE table_name = 'est_actual'"
    ).fetchone() == ('["security_id","measure_code","fiscal_year","fiscal_period","accession_number",'
                     '"period_end","period_start"]',)

    # Quarter, YTD and comparative of one filing now coexist under the same filing labels.
    insert = """
        INSERT INTO est_actual (security_id, measure_code, fiscal_year, fiscal_period, period_start, period_end,
            duration_days, "value", accession_number, as_of_date, "source")
        VALUES ('PX-1', 'EPS_DILUTED', 2025, 'Q2', ?, ?, ?, ?, 'A', ?, 'sec_company_facts')
    """
    con.execute(insert, [dt.date(2025, 4, 1), dt.date(2025, 6, 30), 91, 0.50, dt.date(2025, 6, 30)])
    con.execute(insert, [dt.date(2024, 4, 1), dt.date(2024, 6, 30), 91, 0.40, dt.date(2024, 6, 30)])
    assert con.execute(
        "SELECT count(*) FROM est_actual WHERE measure_code = 'EPS_DILUTED' AND fiscal_period = 'Q2'"
    ).fetchone() == (3,)
    with pytest.raises(duckdb.ConstraintException):
        con.execute(insert, [dt.date(2025, 4, 1), dt.date(2025, 6, 30), 91, 0.55, dt.date(2025, 6, 30)])
    with pytest.raises(duckdb.ConstraintException):
        con.execute(insert, [dt.date(2025, 3, 31), dt.date(2025, 6, 30), 91, 0.55, dt.date(2025, 6, 30)])


@pytest.mark.parametrize("case", ["no_fact", "ambiguous"])
def test_est_actual_rebuild_fails_loudly_on_unprovable_legacy_rows(tmp_store, case):
    con = tmp_store.con
    _legacy_est_actual(tmp_store)
    if case == "ambiguous":
        # Q1 value equal in 3M and 6M windows ending on the same date cannot be told apart.
        _fact(con, concept="EarningsPerShareDiluted", start=dt.date(2025, 4, 1), end=dt.date(2025, 6, 30),
              value=0.25, accession="A")
        _fact(con, concept="EarningsPerShareDiluted", start=dt.date(2025, 1, 1), end=dt.date(2025, 6, 30),
              value=0.25, accession="A")
    _legacy_actual(con, measure="EPS_DILUTED", end=dt.date(2025, 6, 30), value=0.25, accession="A")

    expected = "ambiguous" if case == "ambiguous" else "without an exact Company Facts fact"
    with pytest.raises(RuntimeError, match=expected):
        _in_transaction(con, b0327._est_actual_period_key)
    # Rolled back: the legacy table and its row are intact, nothing was dropped or guessed.
    assert _primary_key(con, "est_actual") == b0327.EST_ACTUAL_LEGACY_KEY
    assert con.execute("SELECT count(*) FROM est_actual").fetchone() == (1,)


# --------------------------------------------------------------------------- coverage monitor (item 4)


def _coverage_fixture(con):
    sessions = [d for d in (dt.date(2024, 3, 1) + dt.timedelta(days=i) for i in range(140)) if d.weekday() < 5]
    con.executemany(
        "INSERT INTO trading_calendar (calendar_id, trade_date, is_open, source) "
        "VALUES ('XNYS', ?, true, 'equity_daily_bars calendar')",
        [[d] for d in sessions],
    )

    def bars(security, last):
        con.executemany(
            "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, adjusted_close, "
            "available_at) VALUES ('t', ?, ?, ?, 10.0, 10.0, ?)",
            [[security, security, d, dt.datetime.combine(d, dt.time(22))] for d in sessions if d <= last],
        )

    def terminal(security, delist_date, source="observed"):
        con.execute(
            "INSERT INTO delisting_terminal_returns (terminal_return_id, source, security_id, delist_date, "
            "as_of_date, available_at, terminal_return, terminal_return_source) "
            "VALUES (?, 't', ?, ?, ?, ?, -0.5, ?)",
            [f"t-{security}", security, delist_date, delist_date,
             dt.datetime.combine(delist_date, dt.time(22)), source],
        )

    def stitched(security, delist_date, version):
        con.execute(
            "INSERT INTO forward_returns_survivorship_safe (forward_return_id, source, security_id, as_of_date, "
            "horizon_days, forward_return, is_delisted_in_horizon, is_stitched, delist_date, available_at, "
            "calculation_version) VALUES (?, 't', ?, DATE '2024-03-01', 21, -0.5, true, true, ?, ?, ?)",
            [f"f-{security}", security, delist_date, dt.datetime.combine(delist_date, dt.time(22)), version],
        )

    # H: halted after 2024-03-22, terminal dated 2024-04-10 (12 absent sessions) -> published 2024-03-25.
    bars("H", dt.date(2024, 3, 22))
    terminal("H", dt.date(2024, 4, 10))
    stitched("H", dt.date(2024, 3, 25), CALCULATION_VERSION)
    # M: terminal on the next session, never stitched -> a genuine drop.
    bars("M", dt.date(2024, 5, 10))
    terminal("M", dt.date(2024, 5, 13))
    # L: legacy v1 publication at the terminal's own date after a halt gap.
    bars("L", dt.date(2024, 6, 14))
    terminal("L", dt.date(2024, 6, 28), source="policy")
    stitched("L", dt.date(2024, 6, 28), "forward_return_publication_v1")


def test_coverage_monitor_counts_a_reanchored_stitch_and_still_reports_a_real_drop(tmp_store):
    con = tmp_store.con
    _in_transaction(con, b0327._delisting_return_coverage_effective_view)
    _coverage_fixture(con)
    # The exact-date join of 0188 would miss H: nothing is stitched at its terminal date.
    assert con.execute(
        "SELECT count(*) FROM forward_returns_survivorship_safe WHERE security_id = 'H' "
        "AND delist_date = DATE '2024-04-10'"
    ).fetchone() == (0,)
    rows = con.execute("""
        SELECT CAST(delist_cohort_month AS DATE), delist_security_days, observed_terminal_count,
               policy_terminal_count, missing_terminal_count, stitched_count, dropped_count
        FROM v_delisting_return_coverage ORDER BY 1
    """).fetchall()
    assert rows == [
        (dt.date(2024, 4, 1), 1, 1, 0, 0, 1, 0),  # H: stitched at its effective date
        (dt.date(2024, 5, 1), 1, 1, 0, 0, 0, 1),  # M: genuinely dropped
        (dt.date(2024, 6, 1), 1, 0, 1, 0, 1, 0),  # L: v1 exact-date stitch still counts
    ]


# --------------------------------------------------------------------------- ISSUER_CONTENT coverage SLOs

# Each ISSUER_CONTENT schema serves the same source table as its FUNDAMENTALS twin
# (ttm is built from statement points; shares uses the filer population of reported).
_SLO_TWINS = {
    "statements": "reported",
    "standardized": "standardized",
    "ttm": "reported",
    "ratios": "ratios",
    "shares": "reported",
    "derived-metrics": "derived-metrics",
}


def test_issuer_content_coverage_slos_are_seeded_and_never_weaker_than_their_twins(tmp_store):
    con = tmp_store.con
    con.execute("DELETE FROM api_schema_coverage_slo WHERE dataset_id = 'ATX.US.ISSUER_CONTENT'")
    _in_transaction(con, b0327._issuer_content_coverage_slos)
    _in_transaction(con, b0327._issuer_content_coverage_slos)  # idempotent reseed
    columns = ("expected_history_start, minimum_history_years, minimum_security_count, minimum_item_count, "
               "maximum_freshness_lag_days, item_count_basis")
    issuer = {row[0]: row[1:] for row in con.execute(
        f"SELECT schema_code, {columns} FROM api_schema_coverage_slo "
        "WHERE dataset_id = 'ATX.US.ISSUER_CONTENT' AND is_active"
    ).fetchall()}
    twins = {row[0]: row[1:] for row in con.execute(
        f"SELECT schema_code, {columns} FROM api_schema_coverage_slo "
        "WHERE dataset_id = 'ATX.US.FUNDAMENTALS' AND is_active"
    ).fetchall()}
    assert set(issuer) == set(_SLO_TWINS)
    for schema, twin_code in _SLO_TWINS.items():
        start, years, securities, items, freshness, basis = issuer[schema]
        t_start, t_years, t_securities, t_items, t_freshness, t_basis = twins[twin_code]
        assert start <= t_start and years >= t_years and securities >= t_securities, schema
        assert freshness <= t_freshness, schema
        if schema == "shares":
            assert items is None  # share-count types are a fixed vocabulary, not a metric catalog
        else:
            assert items is not None and items >= t_items and basis == t_basis, schema


# --------------------------------------------------------------------------- TickerHistory share units (item 7)


def _bar(con, *, source, run_id, security, shares, close=250.0, day=dt.date(2025, 1, 2)):
    con.execute(
        """
        INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, "close", adjusted_close,
            run_id, shares_outstanding, market_cap_usd)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [source, security, security, day, close, close, run_id, shares,
         None if shares is None else shares * close],
    )


def _bars(con):
    return con.execute("""
        SELECT source, run_id, security_id, shares_outstanding, market_cap_usd
        FROM equity_daily_bars ORDER BY source, run_id NULLS FIRST, security_id
    """).fetchall()


def _run(con, run_id, params_json):
    con.execute(
        "INSERT INTO dataset_runs (run_id, dataset_id, status, started_at, source, params_json) "
        "VALUES (?, 'tbltickerhistory_daily', 'succeeded', TIMESTAMP '2026-09-24 00:00:00', ?, ?)",
        [run_id, b0327.TICKER_HISTORY_SOURCE, params_json],
    )


# Input paths exactly as the old loaders recorded them (json.dumps(asdict(options), default=str)).
_PARQUET = json.dumps({"source_path": "C:\\Users\\ops\\Downloads\\TickerHistory3.parquet", "tsv_path": None})
_TSV = json.dumps({"source_path": None, "tsv_path": "C:\\atx\\staging\\tbltickerhistory3_10y.txt"})
_ZIP = json.dumps({"zip_path": "C:\\Users\\ops\\Downloads\\tbltickerhistory3_10y.zip", "symbols": ["SEB", "NEW"]})


def test_ticker_history_unit_gate_decides_every_run_and_writes_nothing(tmp_store):
    """0327 item 7 is read-only (tier-1 v2 1.2): the inventory decides, bars_unit_correction scales."""
    con = tmp_store.con
    th = b0327.TICKER_HISTORY_SOURCE
    _run(con, "parquet-run", _PARQUET)
    _run(con, "tsv-run", _TSV)
    # Parquet delivery (vendor thousands): AAPL 15,204,137 = 15.2B shares.
    _bar(con, source=th, run_id="parquet-run", security="AAPL", shares=15_204_137)
    _bar(con, source=th, run_id="parquet-run", security="MID", shares=17_644, close=20.0)
    _bar(con, source=th, run_id="parquet-run", security="SMALL", shares=29_464, close=5.0)
    _bar(con, source=th, run_id="parquet-run", security="NOSHARES", shares=None)
    # TSV delivery (shares) under the same source name.
    _bar(con, source=th, run_id="tsv-run", security="AAPL", shares=15_204_137_000)
    _bar(con, source=th, run_id="tsv-run", security="NVDA", shares=24_598_341_970, close=120.0)
    # Another vendor with small counts: other sources are never inventoried.
    _bar(con, source="bulk_bars_2015plus", run_id="parquet-run", security="AAPL", shares=17_644)
    before = _bars(con)
    ledger_before = con.execute("SELECT count(*) FROM equity_bar_unit_corrections").fetchone()

    assert [
        (run["run_id"], run["rows_in_run"], run["distinct_securities"], run["source_format"], run["format_unit"],
         run["median_verdict"], run["decision_basis"], run["action"])
        for run in b0327.ticker_history_unit_inventory(con)
    ] == [
        ("parquet-run", 3, 3, "parquet", "thousands", "either", "format_and_median", "scale_x1000"),
        ("tsv-run", 2, 2, "tsv", "units", "units", "format_and_median", "none"),
    ]
    _in_transaction(con, b0327._ticker_history_unit_gate)
    assert _bars(con) == before
    # The 0329 ledger belongs to the bars_unit_correction stage; the gate never writes to it.
    assert con.execute("SELECT count(*) FROM equity_bar_unit_corrections").fetchone() == ledger_before


@pytest.mark.parametrize(
    ("params_json", "shares", "expected"),
    [
        # A8 loader (records shares_unit and a parquet path): stored shares, never rescaled, even
        # though the parquet rule alone would say thousands.
        pytest.param(json.dumps({"shares_unit": "thousands", "source_path": "C:\\d\\TickerHistory3.parquet"}),
                     [450_000, 820_000, 610_000], ("none", "loader_params"), id="unit-aware-loader-run-kept"),
        # rv-a9 probe (a): SEB 968K and a new ETF's 400K shares from the units ZIP; the old median
        # rule scaled them to 968M shares and a $2.9T cap.
        pytest.param(_ZIP, [968_000, 400_000], ("none", "format_and_median"), id="units-run-sub-1M-names-kept"),
        # rv-a9 probe (b): AAPL and MSFT in parquet thousands; the old median rule left them 1000x small.
        pytest.param(_PARQUET, [15_204_137, 7_433_982], ("scale_x1000", "format_and_median"),
                     id="thousands-run-mega-caps-scaled"),
        # A full-universe run (1,200 names stand in for it) with no format evidence: median decides.
        pytest.param(None, 20_000, ("scale_x1000", "median_full_universe"), id="full-universe-no-evidence-median"),
        # A units cross-section recorded as parquet: the path says thousands, the median says shares.
        pytest.param(_PARQUET, 25_000_000, ("abort", "disagreement"), id="format-median-disagreement-raises"),
        pytest.param("{not json", [15_204_137, 17_644, 29_464], ("abort", "no format evidence"),
                     id="small-run-without-format-evidence-raises"),
        pytest.param(_PARQUET, 1_000_000, ("abort", "ambiguous band"), id="ambiguous-median-band-raises"),
        # rv-a9 N1: a thousands run holding a row already in shares (5e9) would become 5e12 shares.
        pytest.param(_PARQUET, [15_204_137, 7_433_982, 5_000_000_000], ("abort", "mixed units"),
                     id="thousands-run-with-rows-already-in-shares-raises"),
    ],
)
def test_ticker_history_unit_needs_format_evidence_and_median_to_agree(
    tmp_store, monkeypatch, params_json, shares, expected
):
    """I1: never decide a production run's unit on a guess; every doubt aborts 0327, which writes nothing."""
    monkeypatch.setattr(b0327, "NO_EVIDENCE_MIN_ROWS", 1_000)
    con = tmp_store.con
    th = b0327.TICKER_HISTORY_SOURCE
    if params_json is not None:
        _run(con, "run", params_json)
    if isinstance(shares, int):  # a cross-section: 1,200 securities at this share count
        con.execute(
            """
            INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, "close", adjusted_close,
                run_id, shares_outstanding, market_cap_usd)
            SELECT ?, 'S' || i, 'S' || i, DATE '2025-01-02', 250.0, 250.0, 'run', ?, ? * 250.0 FROM range(1200) t(i)
            """,
            [th, shares, shares],
        )
    else:
        for index, count in enumerate(shares):
            _bar(con, source=th, run_id="run", security=f"S{index}", shares=count)
    before = _bars(con)
    action, detail = expected

    # The read-only B0 inventory reports exactly what the migration gate then decides.
    [run] = b0327.ticker_history_unit_inventory(con)
    assert (run["run_id"], run["action"]) == ("run", action)

    if action == "abort":
        assert detail in run["reason"]
        with pytest.raises(RuntimeError, match=detail):
            _in_transaction(con, b0327._ticker_history_unit_gate)
    else:
        assert run["decision_basis"] == detail
        _in_transaction(con, b0327._ticker_history_unit_gate)
    assert _bars(con) == before  # the correction is the bars_unit_correction stage, never 0327


# --------------------------------------------------------------------------- helpers


def _insert(con, table, rows):
    for row in rows:
        defaulted = {"source_loaded_at", "is_latest_revision"}
        values = {key: value for key, value in row.items() if value is not None or key not in defaulted}
        columns = ", ".join(f'"{name}"' for name in values)
        placeholders = ", ".join("?" for _ in values)
        con.execute(f"INSERT INTO {table} ({columns}) VALUES ({placeholders})", list(values.values()))
