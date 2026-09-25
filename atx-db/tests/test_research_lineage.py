"""The set-based lineage prover must reproduce the Python resolver row for row."""

from __future__ import annotations

import datetime as dt
import hashlib
import json

import duckdb
import pytest

from atx_db.derived_lineage import qualify_selected_lineage
from atx_db.derived_registry import DERIVED_SOURCE_NAME
from atx_db.research import lineage

HASHES = {("m_q", "q"): "d" * 64, ("m_g", "q"): "e" * 64, ("m_c", "q"): "f" * 64}
T0 = dt.datetime(2023, 11, 9, 18)


def _bucket(day: dt.date) -> int:
    return (day.year * 12 + day.month - 1 + int(day.day >= 15)) // 3


class Graph:
    def __init__(self) -> None:
        self.con = duckdb.connect(":memory:")
        self.con.execute("SET TimeZone='UTC'")
        self.con.execute("""
            CREATE TABLE derived_metric_values (
              derived_value_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
              metric_code VARCHAR, metric_window VARCHAR, target_bucket BIGINT, period_end DATE,
              value DOUBLE, available_at TIMESTAMP, valid_to TIMESTAMP, inputs_hash VARCHAR,
              definition_hash VARCHAR, fiscal_period_start DATE, fiscal_period_end DATE,
              history_status VARCHAR, value_status VARCHAR, selected_input_refs_json VARCHAR,
              selected_input_refs_hash VARCHAR);
            CREATE TABLE fundamental_standardized (
              standardized_id VARCHAR PRIMARY KEY, canonical_code VARCHAR, cik VARCHAR,
              basis VARCHAR, source VARCHAR, period_start DATE, period_end DATE,
              available_at TIMESTAMP, value DOUBLE);
        """)
        self.nodes: dict[str, dict] = {}

    def leaf(self, leaf_id: str, *, cik: str | None = "1", end: dt.date = dt.date(2023, 9, 30),
             at: dt.datetime = T0 - dt.timedelta(hours=2), value: float | None = 1.0) -> str:
        self.con.execute("INSERT INTO fundamental_standardized VALUES (?,'x',?,'quarterly','test',NULL,?,?,?)",
                         [leaf_id, cik, end, at, value])
        return leaf_id

    def item(self, leaf_id: str, *, offset: int = 0, end: dt.date = dt.date(2023, 9, 30),
             at: dt.datetime = T0 - dt.timedelta(hours=2), cik: object = "1", **override) -> dict:
        ref = {"kind": "item", "code": "x", "bucket": _bucket(end), "offset": offset,
               "status": "selected", "state_id": leaf_id, "available_at": str(at), "cik": cik,
               "basis": "quarterly", "source": "test", "period_start": None,
               "period_end": end.isoformat(), "inputs_hash": None, "definition_hash": None}
        ref.update(override)
        return ref

    def metric(self, child: str, **override) -> dict:
        row = self.nodes[child]
        ref = {"kind": "metric", "code": row["code"], "bucket": row["bucket"], "offset": 0,
               "status": "selected", "state_id": child, "available_at": str(row["at"]), "cik": None,
               "basis": None, "source": DERIVED_SOURCE_NAME, "period_start": None,
               "period_end": row["end"].isoformat(), "inputs_hash": "b" * 64,
               "definition_hash": HASHES[(row["code"], "q")]}
        ref.update(override)
        return ref

    def node(self, node_id: str, refs: list, *, code: str = "m_q", at: dt.datetime = T0,
             end: dt.date = dt.date(2023, 9, 30), value: float | None = 1.0, status: str = "valid",
             history: str = "event_reconstructed", definition_hash: str | None = None,
             valid_to: dt.datetime | None = None, payload: str | None = None,
             stored_hash: str | None = None, version: object = 1) -> str:
        if payload is None:
            payload = json.dumps({"version": version, "refs": refs}, sort_keys=True, separators=(",", ":"))
        digest = stored_hash or hashlib.sha256(payload.encode()).hexdigest()
        bucket = _bucket(end)
        self.con.execute("""
            INSERT INTO derived_metric_values VALUES
            (?,?,'owner',?,'q',?,?,?,?,?,?,?,NULL,?,?,?,?,?)
        """, [node_id, DERIVED_SOURCE_NAME, code, bucket, end, value, at, valid_to, "b" * 64,
              definition_hash or HASHES[(code, "q")], end, history, status, payload, digest])
        self.nodes[node_id] = {"code": code, "bucket": bucket, "at": at, "end": end}
        return node_id


def _python(con, root):
    proof = qualify_selected_lineage(con, [root], expected_cik=None, decision_cutoff=None,
                                     expected_definition_hashes=HASHES, max_depth=16,
                                     max_nodes=512, max_bytes=1_048_576)[root]
    clocks = [c for c in proof.input_clocks if c is not None]
    ends = [e for e in proof.fiscal_ends if e is not None]
    return (proof.status, proof.reason, proof.selected_cik, proof.digest,
            min(ends) if ends else None, max(ends) if ends else None,
            max(clocks) if clocks else None, len(proof.leaf_ids))


def _build() -> tuple[Graph, dict[str, str]]:
    g = Graph()
    expect: dict[str, str] = {}
    for n in range(3):
        g.leaf(f"L{n}")
    g.leaf("L3", end=dt.date(2022, 9, 30))
    g.leaf("Lnull", value=None)
    g.leaf("Llate", at=T0 + dt.timedelta(hours=3))
    g.leaf("Lnocik", cik=None)
    g.leaf("Lcik2", cik="2")
    g.leaf("Lmicro", at=dt.datetime(2023, 11, 9, 11, 30, 0, 250000))
    good = [g.item("L0"), g.item("L1"), g.item("L2")]
    cases = {
        "r_good": (good, {}, lineage.METHOD_EXACT),
        "r_nullleaf": ([g.item("L0"), g.item("Lnull")], {}, lineage.METHOD_EXACT),
        "r_missing_id": ([g.item("L0"), g.item("L1", status="missing", state_id=None)], {}, lineage.METHOD_EXACT),
        "r_missing_status": ([g.item("L0"), g.item("L1", status="missing")], {}, lineage.METHOD_EXACT),
        "r_leaf_absent": ([g.item("L0"), g.item("Labsent")], {}, lineage.METHOD_EXACT),
        "r_cik_mismatch": ([g.item("L0", cik="2")], {}, lineage.METHOD_EXACT),
        "r_leaf_late": ([g.item("Llate", at=T0 + dt.timedelta(hours=3))], {}, lineage.METHOD_EXACT),
        "r_leaf_nocik": ([g.item("Lnocik", cik=None)], {}, lineage.METHOD_EXACT),
        "r_mixed": ([g.item("L0"), g.item("Lcik2", cik="2")], {}, lineage.METHOD_EXACT),
        "r_hash": (good, {"stored_hash": "0" * 64}, lineage.METHOD_EXACT),
        "r_legacy": (good, {"history": "legacy_latest_only"}, lineage.METHOD_EXACT),
        "r_defhash": (good, {"definition_hash": "9" * 64}, lineage.METHOD_EXACT),
        "r_expired": (good, {"valid_to": T0}, lineage.METHOD_EXACT),
        "r_frame": ([g.item("L0", bucket=1)], {}, lineage.METHOD_EXACT),
        "r_order": ([g.item("L0"), {"kind": "bogus"}, g.item("L1", state_id=None)], {}, lineage.METHOD_EXACT),
        "r_zero": ([], {}, lineage.METHOD_EXACT),
        "r_negoff": ([g.item("L0", offset=-1)], {}, lineage.METHOD_EXACT),
        "r_version": (good, {"version": 2}, lineage.METHOD_EXACT),
        "r_value_null": (good, {"value": None, "status": "missing_input_or_domain"}, lineage.METHOD_EXACT),
        "r_cik_number": ([g.item("L0", cik=1)], {}, lineage.METHOD_EXACT),
        "r_period": ([g.item("L0", period_end="2023-09-29")], {}, lineage.METHOD_EXACT),
        "r_micro": ([g.item("Lmicro", at=dt.datetime(2023, 11, 9, 11, 30, 0, 250000))], {}, lineage.METHOD_EXACT),
        "r_tclock": ([g.item("L0", available_at=(T0 - dt.timedelta(hours=2)).isoformat())], {}, lineage.METHOD_EXACT),
        "r_tzclock": ([g.item("L0", available_at="2023-11-09T16:00:00+00:00")], {}, lineage.METHOD_FALLBACK),
        "r_version_float": (good, {"version": 1.0}, lineage.METHOD_FALLBACK),
        "r_bucket_bool": ([g.item("L0", bucket=True)], {}, lineage.METHOD_FALLBACK),
        "r_bad_json": ([], {"payload": '{"version":1,"refs":[}'}, lineage.METHOD_FALLBACK),
        "r_dup_keys": ([], {"payload": '{"version":1,"refs":[{"kind":"item","kind":"item"}]}'},
                       lineage.METHOD_FALLBACK),
    }
    for root, (refs, options, method) in cases.items():
        g.node(root, refs, **options)
        expect[root] = method
    # Depth > 0: growth over two quarterly children; chains; shared children.
    g.node("c1", [g.item("L0"), g.item("L1")], at=T0 - dt.timedelta(hours=1))
    g.node("c2", [g.item("L2"), g.item("L3", offset=4, end=dt.date(2022, 9, 30))],
           at=T0 - dt.timedelta(hours=1))
    g.node("c_null", [g.item("L0")], at=T0 - dt.timedelta(hours=1), value=None,
           status="missing_input_or_domain")
    g.node("c_late", [g.item("L0")], at=T0 + dt.timedelta(hours=1))
    g.node("c_cik2", [g.item("Lcik2", cik="2")], at=T0 - dt.timedelta(hours=1))
    g.node("c_chain", [g.metric("c1")], code="m_c", at=T0 - dt.timedelta(minutes=30))
    for root, refs, method in [
        ("d_good", [g.metric("c1"), g.metric("c2")], lineage.METHOD_CERTIFIED),
        ("d_shared", [g.metric("c1"), g.metric("c1"), g.item("L3", offset=4, end=dt.date(2022, 9, 30))],
         lineage.METHOD_CERTIFIED),
        ("d_chain", [g.metric("c_chain")], lineage.METHOD_CERTIFIED),
        ("d_child_null", [g.metric("c1"), g.metric("c_null")], lineage.METHOD_CERTIFIED),
        ("d_child_late", [g.metric("c_late")], lineage.METHOD_FALLBACK),
        ("d_child_absent", [g.metric("c1"), dict(g.metric("c1"), state_id="nope")], lineage.METHOD_FALLBACK),
        ("d_mixed", [g.metric("c1"), g.metric("c_cik2")], lineage.METHOD_FALLBACK),
    ]:
        g.node(root, refs, code="m_g")
        expect[root] = method
    expect["r_not_there"] = lineage.METHOD_FALLBACK
    return g, expect


def test_set_prover_equals_the_python_resolver_for_every_root():
    g, expect = _build()
    con = g.con
    con.execute("CREATE TEMP TABLE _lp_roots (root_id VARCHAR)")
    con.executemany("INSERT INTO _lp_roots VALUES (?)", [[root] for root in expect])
    counts = lineage.prove_roots(con, HASHES)
    rows = {row[0]: row for row in lineage.result_rows(con)}
    assert set(rows) == set(expect)
    for root, method in expect.items():
        row = rows[root]
        got = (row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[8])
        assert got == _python(con, root), root
        assert row[9] == method, (root, row[9], got)
    assert counts[lineage.METHOD_EXACT] >= 20 and counts[lineage.METHOD_CERTIFIED] == 4
    statuses = {root: rows[root][1] for root in expect}
    assert statuses["r_good"] == "qualified" and statuses["d_good"] == "qualified"
    assert {statuses[r] for r in ("r_nullleaf", "r_missing_status", "r_value_null", "d_child_null")} == {"invalid"}
    assert rows["r_order"][2] == "invalid ref kind: r_order"
    assert rows["r_defhash"][2] == "definition hash mismatch: ('m_q', 'q')"


@pytest.mark.parametrize("chunk", [1, 3])
def test_set_prover_is_independent_of_how_roots_are_chunked(chunk):
    g, expect = _build()
    con = g.con
    roots = sorted(expect)
    together = {}
    con.execute("CREATE OR REPLACE TEMP TABLE _lp_roots (root_id VARCHAR)")
    con.executemany("INSERT INTO _lp_roots VALUES (?)", [[root] for root in roots])
    lineage.prove_roots(con, HASHES)
    together = {row[0]: row for row in lineage.result_rows(con)}
    for start in range(0, len(roots), chunk):
        con.execute("CREATE OR REPLACE TEMP TABLE _lp_roots (root_id VARCHAR)")
        con.executemany("INSERT INTO _lp_roots VALUES (?)", [[root] for root in roots[start:start + chunk]])
        lineage.prove_roots(con, HASHES)
        for row in lineage.result_rows(con):
            assert row == together[row[0]]
