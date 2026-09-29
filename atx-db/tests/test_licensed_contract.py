"""Licensed-adapter contract (S8.1): clock macros, identifier mapping, receipts, validation, stage writing, registry."""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
import pytest

from atx_db import licensed
from atx_db.licensed import contract as K
from atx_db.licensed.identity import IdentityResolver
from atx_db.licensed.mock import MockUniverse, cusip_check_digit, make_isin

BUILD = dt.datetime(2026, 7, 2)
D = dt.date


def test_clock_macros() -> None:
    con = K.connect()
    row = con.execute("""SELECT lic_date('2024-03-14'), lic_date('20240314'), lic_date('03/14/2024'), lic_date(''),
                                lic_time('16:05:00'), lic_time('160500'), lic_time('93000'),
                                lic_utc(DATE '2024-01-03', TIME '16:00:00', 'America/New_York'),
                                lic_utc(DATE '2024-07-03', TIME '16:00:00', 'America/New_York'),
                                lic_utc(DATE '2024-07-03', TIME '22:00:00', 'America/Chicago'),
                                lic_next_weekday(DATE '2024-03-15'), lic_next_weekday(DATE '2024-03-14'),
                                lic_ticker_key('BRK.B'), lic_ticker_key('BRK/B'), lic_ticker_key(' '),
                                lic_ts('2024-01-25T21:00:00Z')""").fetchone()
    assert row[0] == row[1] == row[2] == D(2024, 3, 14) and row[3] is None
    assert row[4] == row[5] == dt.time(16, 5) and row[6] == dt.time(9, 30)
    assert row[7] == dt.datetime(2024, 1, 3, 21, 0)  # EST
    assert row[8] == dt.datetime(2024, 7, 3, 20, 0)  # EDT
    assert row[9] == dt.datetime(2024, 7, 4, 3, 0)  # 22:00 CDT
    assert row[10] == D(2024, 3, 18) and row[11] == D(2024, 3, 15)  # Friday -> Monday
    assert row[12] == row[13] == "BRKB" and row[14] is None
    assert row[15] == dt.datetime(2024, 1, 25, 21, 0)


def test_identifier_check_digits() -> None:
    assert cusip_check_digit("03783310") == "0"  # Apple 037833100
    assert cusip_check_digit("59491810") == "4"  # Microsoft 594918104
    assert make_isin("037833100") == "US0378331005"


def _resolve(res: IdentityResolver, rows: list[tuple]) -> list[tuple]:
    con = K.connect()
    con.register("src_in", pa.table({"_rid": list(range(1, len(rows) + 1)), "cusip": [r[0] for r in rows],
                                     "ticker": [r[1] for r in rows], "native_security_id": [r[2] for r in rows],
                                     "id_date": [r[3] for r in rows]},
                                    schema=pa.schema([("_rid", pa.int64()), ("cusip", pa.string()),
                                                      ("ticker", pa.string()), ("native_security_id", pa.int64()),
                                                      ("id_date", pa.date32())])))
    con.execute("CREATE TEMP TABLE src AS SELECT * FROM src_in")
    res.register(con)
    res.resolve(con, "src", "dst")
    return con.execute("SELECT security_id, link_tier, cik, cik_link_tier FROM dst ORDER BY _rid").fetchall()


def test_identity_tiers_and_cik() -> None:
    res = IdentityResolver.from_rows(
        cusips=[("037833100", 1, D(2019, 1, 2), D(2022, 12, 30)), ("11111110X", 2, D(2019, 1, 2), D(2026, 6, 30)),
                ("22222220Y", 3, D(2019, 1, 2), D(2026, 6, 30)), ("22222220Y", 4, D(2019, 1, 2), D(2026, 6, 30))],
        tickers=[("AAPL", 1, D(2019, 1, 2), D(2026, 6, 30)), ("BRK.B", 5, D(2019, 1, 2), D(2026, 6, 30)),
                 ("DUP", 6, D(2020, 1, 2), D(2020, 12, 31)), ("DUP", 7, D(2020, 6, 1), D(2021, 12, 31))],
        links=[(1, 320193, D(2019, 1, 2), D(2020, 12, 31), "strict"), (1, 320193, D(2021, 1, 11), D(2026, 6, 30), "name"),
               (5, 1067983, D(2019, 1, 2), D(2026, 6, 30), "backfill")])
    out = _resolve(res, [
        (None, None, 99, D(2024, 1, 2)),              # vendor native id
        ("03783310", "XX", None, D(2021, 5, 3)),      # 8-char CUSIP inside the interval
        ("037833100", None, None, D(2023, 6, 1)),     # 153 days after the interval -> undated
        ("037833100", None, None, D(2025, 6, 1)),     # beyond 400 days -> falls to ticker (none) -> unmapped
        ("99999Z107", "BRK/B", None, D(2024, 1, 2)),  # unknown CUSIP, ticker canonical form
        (None, "DUP", None, D(2020, 7, 1)),           # two lines carry the ticker that day
        ("22222220Y", None, None, D(2024, 1, 2)),     # two lines carry the CUSIP
        (None, "AAPL", None, D(2021, 1, 5)),          # ticker; CIK link gap of 5 days bridged
        (None, "AAPL", None, D(2018, 6, 1)),          # outside every history
    ])
    assert out[0][:2] == (99, "vendor_native")
    assert out[1][:4] == (1, "cusip_dated", 320193, "name")
    assert out[2][:2] == (1, "cusip_undated")
    assert out[3][:2] == (None, "unmapped")
    assert out[4][:4] == (5, "ticker_dated", 1067983, "backfill")
    assert out[5][:2] == (None, "ambiguous") and out[6][:2] == (None, "ambiguous")
    assert out[7][:4] == (1, "ticker_dated", 320193, "strict")
    assert out[8][:2] == (None, "unmapped")


def test_receipts_default_to_backfill_and_check_sha(tmp_path: Path) -> None:
    f = K.csv_write(tmp_path / "x.csv", ["a"], [[1]])
    t = K.read_receipts(tmp_path, [f])
    assert t.column("history_mode").to_pylist() == [None] and t.column("delivered_at").to_pylist() == [None]
    K.write_receipt(tmp_path, f, fetched_at=dt.datetime(2026, 1, 2, 3, 4, 5), history_mode="daily")
    t = K.read_receipts(tmp_path, [f])
    assert t.column("delivered_at").to_pylist() == [dt.datetime(2026, 1, 2, 3, 4, 5)]
    f.write_text("a\n2\n", encoding="utf-8")
    with pytest.raises(ValueError, match="receipt sha256"):
        K.read_receipts(tmp_path, [f])
    with pytest.raises(ValueError):
        K.write_receipt(tmp_path, f, fetched_at=BUILD, history_mode="asof")


@pytest.fixture(scope="module")
def est(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    a = licensed.get("estimates")
    raw = tmp_path_factory.mktemp("est")
    a.mock(raw, u)
    return a, u, raw, a.load(raw, identity=u.resolver(), build_time=BUILD, strict=True)


def test_file_without_receipt_is_backfill_at_build_time(est, tmp_path: Path) -> None:
    a, u, raw, _ = est
    (tmp_path / "statsum_x.csv").write_bytes((raw / "statsum_epsus.csv").read_bytes())
    st = a.load(tmp_path, identity=u.resolver(), build_time=BUILD)
    c = st.tables["consensus"]
    assert set(c.column("history_mode").to_pylist()) == {"backfill"}
    assert set(c.column("available_at").to_pylist()) == {BUILD}
    assert set(c.column("clock_basis").to_pylist()) == {"delivery"} and all(c.column("vintage_risk").to_pylist())


def _mutate(t: pa.Table, col: str, idx: int, value) -> pa.Table:
    vals = t.column(col).to_pylist()
    vals[idx] = value
    return t.set_column(t.schema.get_field_index(col), t.schema.field(col), pa.array(vals, t.schema.field(col).type))


def test_validation_detects_each_violation(est) -> None:
    a, _, _, st = est
    base = st.tables["consensus"]
    assert not a.validate(st.tables, BUILD).failures
    dup = pa.concat_tables([base, base.slice(0, 1)])
    assert a.validate({**st.tables, "consensus": dup}, BUILD).failed("consensus", "keys_unique")
    fut = _mutate(base, "available_at", 0, dt.datetime(2027, 1, 1))
    assert a.validate({**st.tables, "consensus": fut}, BUILD).failed("consensus", "no_future_clocks")
    snap = _mutate(base, "vendor_snapshot_at", 0, dt.datetime(2026, 7, 1, 23))
    assert a.validate({**st.tables, "consensus": snap}, BUILD).failed("consensus", "snapshot_before_available")
    # an older vintage of a series made visible after a newer one breaks monotone clocks
    s = base.filter(pc.equal(base.column("vendor_security_id"), base.column("vendor_security_id")[0]))
    s = s.filter(pc.and_(pc.equal(s.column("measure_code"), "EPS"), pc.equal(s.column("period_type"), "FY")))
    first = s.slice(0, 1)
    late = _mutate(first, "available_at", 0, dt.datetime(2026, 1, 1))
    rest = base.filter(pc.invert(pc.equal(base.column("vendor_snapshot_at"), first.column("vendor_snapshot_at")[0])
                                 .fill_null(False)))
    mono = pa.concat_tables([rest, late])
    assert a.validate({**st.tables, "consensus": mono}, BUILD).failed("consensus", "clocks_monotone_per_vintage")
    tier = _mutate(base, "link_tier", 0, "unmapped")
    assert a.validate({**st.tables, "consensus": tier}, BUILD).failed("consensus", "link_tier_consistent")
    wrong = base.drop_columns(["cik"])
    assert a.validate({**st.tables, "consensus": wrong}, BUILD).failed("consensus", "schema")


def test_strict_load_raises_on_future_vendor_dates(est, tmp_path: Path) -> None:
    a, u, raw, _ = est
    lines = (raw / "statsum_epsus.csv").read_text(encoding="utf-8").splitlines()
    ln = u.lines[3]
    lines.append(f"I{ln.security_id:04d},{ln.cusips[0][0][:8]},REUS,2026-09-17,EPS,ANN,1,2026-12-31,4,0,0,1,1,0.1,1.2,0.8,USD")
    p = K.csv_write(tmp_path / "statsum_future.csv", lines[0].split(","), [r.split(",") for r in lines[1:]])
    K.write_receipt(tmp_path, p, fetched_at=dt.datetime(2026, 7, 1), history_mode="pit_archive")
    with pytest.raises(K.ContractViolation, match="no_future_clocks"):
        a.load(tmp_path, identity=u.resolver(), build_time=BUILD)


def test_write_stage_and_manifest(est, tmp_path: Path) -> None:
    a, u, raw, _ = est
    out = tmp_path / "licensed_estimates"
    st2 = a.load(raw, identity=u.resolver(), build_time=BUILD, out_dir=out)
    man = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    assert man["schema"] == "atx.licensed.estimates/v1" and man["status"] == "complete"
    assert set(man["files"]) == {f"{n}.parquet" for n in a.tables}
    for name, meta in man["files"].items():
        assert meta["sha256"] == K.sha256_file(out / name)
        f = pq.ParquetFile(out / name)
        assert f.schema_arrow == a.tables[name[:-8]].schema
        assert all(f.metadata.row_group(i).num_rows <= K.ROW_GROUP for i in range(f.num_row_groups))
    assert man["raw_inputs"]["statsum_restated.csv"]["history_mode"] == "backfill"
    assert "contract.py" in man["code"] and man["validation"]["failures"] == 0
    assert not list(out.glob("*.partial")) and st2.manifest == man


def test_registry_contract() -> None:
    for name in licensed.ADAPTERS:
        a = licensed.get(name)
        assert a.name == name and a.tables and a.pit_rule and a.purchase and a.product
        for spec in a.tables.values():
            names = spec.schema.names
            for f in K.ID_FIELDS + K.CLOCK_FIELDS + K.LINEAGE_FIELDS:
                assert f.name in names
            assert set(spec.key) <= set(names) and set(spec.series) <= set(names) and spec.pit_rule
            assert len(names) == len(set(names))


def test_substitute_resolve(tmp_path: Path) -> None:
    sub = licensed.get("lending").substitute()
    assert sub.stage == "borrow_proxy"
    assert sub.resolve(tmp_path)["exists"] is False
    d = tmp_path / "borrow_proxy" / "year=2024"
    d.mkdir(parents=True)
    pq.write_table(pa.table({"session_date": [D(2024, 1, 2)], "security_id": [1], "cutoff_utc": [BUILD],
                             "si_to_io": [0.1], "si_shares": [1.0]}), d / "borrow_proxy.parquet")
    (tmp_path / "borrow_proxy" / "manifest.json").write_text('{"status": "complete"}', encoding="utf-8")
    r = sub.resolve(tmp_path)
    assert r["exists"] and r["manifest_status"] == "complete" and r["files"] == 1
    assert "si_to_io" in r["columns_present"] and r["columns_missing"] == ["inst_shares", "on_threshold_list"]
