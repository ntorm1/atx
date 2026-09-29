"""Estimates adapter (S8.1): I/B/E/S-layout mocks -> consensus, detail, actuals, recommendations, price targets."""

from __future__ import annotations

import datetime as dt
import itertools
from pathlib import Path

import pytest

from atx_db import licensed
from atx_db.licensed import contract as K
from atx_db.licensed import estimates as E
from atx_db.licensed.mock import MockUniverse

BUILD = dt.datetime(2026, 7, 2)


@pytest.fixture(scope="module")
def run(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    raw = tmp_path_factory.mktemp("est")
    E.ADAPTER.mock(raw, u)
    st = E.ADAPTER.load(raw, identity=u.resolver(), build_time=BUILD)
    return u, raw, st, {n: t.to_pylist() for n, t in st.tables.items()}


def test_schema_and_validation(run) -> None:
    _, _, st, rows = run
    for name, spec in E.ADAPTER.tables.items():
        assert st.tables[name].schema == spec.schema and rows[name]
    assert not st.report.failures
    assert st.rejects["detail"] == {"missing_activation": 1}


def test_consensus_clock_is_statpers_plus_publication_lag(run) -> None:
    _, _, _, rows = run
    r = next(r for r in rows["consensus"] if r["consensus_date"] == dt.date(2024, 3, 14))  # Thursday, EDT
    assert r["vendor_snapshot_at"] == dt.datetime(2024, 3, 15, 3, 59, 59)
    assert r["available_at"] == dt.datetime(2024, 3, 15, 16, 0) and r["clock_basis"] == "publication_lag"
    r = next(r for r in rows["consensus"] if r["consensus_date"] == dt.date(2024, 1, 18))  # EST
    assert r["available_at"] == dt.datetime(2024, 1, 19, 17, 0)
    # first decision session: cutoff 22:00 UTC of d-1 must be later than available_at -> Monday 2024-03-18
    assert r["available_at"] < dt.datetime(2024, 1, 19, 22, 0)
    assert {r["period_type"] for r in rows["consensus"]} == {"FY", "FQ"}
    assert {r["measure_code"] for r in rows["consensus"]} == {"EPS", "REVENUE"}


def test_backfill_consensus_is_clocked_at_delivery(run) -> None:
    _, _, _, rows = run
    r = next(r for r in rows["consensus"] if r["source_file"] == "statsum_restated.csv")
    assert r["history_mode"] == "backfill" and r["vintage_risk"] and r["clock_basis"] == "delivery"
    assert r["available_at"] == dt.datetime(2026, 6, 30, 12, 0) and r["consensus_date"] == dt.date(2023, 5, 18)


def test_detail_uses_activation_never_anndats(run) -> None:
    _, _, _, rows = run
    for r in rows["detail"]:
        assert r["available_at"] == r["vendor_snapshot_at"]
        assert r["available_at"].date() >= r["announce_date"]
    normal = [r for r in rows["detail"] if r["clock_basis"] == "vendor_pit"]
    assert all(r["available_at"].date() > r["announce_date"] for r in normal)  # activation the day after
    floor = [r for r in rows["detail"] if r["clock_basis"] == "floor"]
    assert len(floor) == 1 and floor[0]["available_at"] == dt.datetime(2024, 5, 3, 3, 59, 59) and floor[0]["vintage_risk"]
    assert floor[0]["measure_code"] == "EPS_BASIC" and floor[0]["period_type"] == "FQ"
    assert {r["measure_code"] for r in normal} == {"EPS_DILUTED"}


def test_restated_actual_is_a_second_vintage(run) -> None:
    u, _, _, rows = run
    sid = u.lines[5].security_id
    v = sorted((r for r in rows["actuals"] if r["security_id"] == sid and r["period_end"] == dt.date(2024, 6, 30)),
               key=lambda r: r["vendor_snapshot_at"])
    assert len(v) == 2 and v[0]["value"] != v[1]["value"] and v[0]["available_at"] < v[1]["available_at"]
    # announced 16:05 ET, activated 18:40 ET: the later clock wins
    assert v[0]["available_at"] == dt.datetime(2024, 7, 30, 22, 40) and v[0]["announce_at"] == dt.datetime(2024, 7, 30, 20, 5)


def test_recommendations_actions_and_targets(run) -> None:
    _, _, _, rows = run
    recs = rows["recommendations"]
    assert all(1 <= r["rec_code"] <= 5 and r["rec_label"] == E.RECOMMENDATION_LABELS[r["rec_code"]] for r in recs)
    by = {}
    for r in sorted(recs, key=lambda r: r["vendor_snapshot_at"]):
        by.setdefault((r["vendor_security_id"], r["broker_id"]), []).append(r)
    for seq in by.values():
        assert seq[0]["action"] == "INITIATE" and seq[0]["prior_rec_code"] is None
        for a, b in itertools.pairwise(seq):
            assert b["prior_rec_code"] == a["rec_code"]
            assert b["action"] == ("UPGRADE" if b["rec_code"] < a["rec_code"] else
                                   "DOWNGRADE" if b["rec_code"] > a["rec_code"] else "REITERATE")
    pts = sorted((r for r in rows["price_targets"] if r["prior_target"] is not None), key=lambda r: r["vendor_snapshot_at"])
    assert pts and all(r["target"] > 0 and r["horizon_months"] == 12 for r in pts)


def test_identifier_mapping_tiers(run) -> None:
    u, _, st, rows = run
    cons = rows["consensus"]
    reus = sorted((r for r in cons if r["vendor_security_id"] == "IREUS"), key=lambda r: r["id_date"])
    assert [(r["link_tier"], r["security_id"]) for r in reus] == [
        ("ticker_dated", u.lines[3].security_id), ("ticker_dated", u.lines[4].security_id)]
    ln1 = [r for r in cons if r["vendor_security_id"] == f"I{u.lines[1].security_id:04d}" and r["cusip"] == u.lines[1].cusips[0][0][:8]]
    assert {(r["id_date"].year, r["link_tier"]) for r in ln1} == {(2021, "cusip_dated"), (2023, "cusip_undated")}
    unk = [r for r in cons if r["vendor_security_id"] == "IUNK"]
    assert unk[0]["link_tier"] == "unmapped" and unk[0]["security_id"] is None and unk[0]["cik"] is None
    # delisted line keeps its history; second share class maps to its own line with the issuer CIK
    assert any(r["security_id"] == u.lines[2].security_id for r in cons)
    six = [r for r in cons if r["security_id"] == u.lines[6].security_id]
    assert six and {r["cik"] for r in six} == {u.lines[5].cik}
    assert st.report.stats["consensus"]["mapped_share"] > 0.99


def test_aliases_accept_factset_style_headers(run, tmp_path: Path) -> None:
    u, raw, st, _ = run
    src = (raw / "statsum_epsus.csv").read_text(encoding="utf-8").splitlines()
    head = src[0].replace("STATPERS", "snapshot_date").replace("MEANEST", "mean_est").replace("OFTIC", "symbol") \
                 .replace("NUMEST", "num_est").replace("FPEDATS", "fiscal_period_end")
    p = tmp_path / "statsum_fs.csv"
    p.write_text("\n".join([head, *src[1:]]) + "\n", encoding="utf-8")
    K.write_receipt(tmp_path, p, fetched_at=dt.datetime(2026, 7, 1, 6), history_mode="pit_archive")
    alt = E.ADAPTER.load(tmp_path, identity=u.resolver(), build_time=BUILD).tables["consensus"]
    orig = st.tables["consensus"].filter(
        __import__("pyarrow.compute", fromlist=["x"]).equal(st.tables["consensus"].column("source_file"), "statsum_epsus.csv"))
    cols = ["security_id", "consensus_date", "mean", "num_estimates", "period_end", "available_at", "link_tier"]
    assert alt.select(cols).to_pylist() == orig.select(cols).to_pylist()


def test_substitute_points_at_guidance() -> None:
    sub = licensed.get("estimates").substitute()
    assert sub.stage == "events/guidance.parquet" and sub.owner.startswith("EVT") and "cik" in sub.keys
    assert sub.columns["mean"] == "mid"


def test_ported_v2_maps_match() -> None:
    v2 = pytest.importorskip("atx_db.estimates._columns")
    assert E.IBES_MEASURE_MAP == v2.IBES_MEASURE_MAP
    assert E.RECOMMENDATION_TEXT_MAP == v2.RECOMMENDATION_TEXT_MAP
    assert E.RECOMMENDATION_LABELS == v2.RECOMMENDATION_LABELS
    common = pytest.importorskip("atx_db.estimates._common")
    for code, ptype in E.FPI_PERIOD_TYPE.items():
        assert common._period_type_from_fpi(code) == ptype
