"""Tiny offline independent release admission fixtures (128MB/one thread)."""
import datetime as dt
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import release_inputs as R
from atx_db.alpha_panel import universe as U
from atx_db.alpha_panel import universe_release as V

D = dt.date(2021, 1, 4)


@pytest.fixture
def con(tmp_path):
    db = C.connect(memory="128MB", threads=1, temp_dir=tmp_path / "spill")
    yield db
    db.close()


def payload(path, keys=((D, 1),), clock=None):
    values = {}
    for name in V.OUTPUT_COLUMNS:
        if name == "session_date":
            values[name] = pa.array([key[0] for key in keys], type=pa.date32())
        elif name in ("security_id", "cik"):
            values[name] = pa.array([key[1] for key in keys], type=pa.int64())
        elif name in ("shrcd_like", "exchcd_like"):
            values[name] = pa.array([11 if name == "shrcd_like" else 1] * len(keys), type=pa.int32())
        elif name in ("exchcd_basis", "filer_regime"):
            values[name] = pa.array(["domestic"] * len(keys), type=pa.string())
        elif name == "me":
            values[name] = pa.array([2.] * len(keys), type=pa.float64())
        elif name == "available_at":
            values[name] = pa.array([clock if clock is not None else dt.datetime.combine(key[0] or D, dt.time(22)) for key in keys], type=pa.timestamp("us"))
        else:
            values[name] = pa.array([True] * len(keys), type=pa.bool_())
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table(values), path)


def axis(tmp_path):
    panel = tmp_path / "panel/year=2021/p.parquet"
    panel.parent.mkdir(parents=True)
    pq.write_table(pa.table({"session_date": pa.array([D], type=pa.date32()), "security_id": pa.array([1], type=pa.int64())}), panel)
    pq.write_table(pa.table({"session_date": pa.array([D], type=pa.date32())}), tmp_path / "calendar.parquet")
    return panel


@pytest.mark.parametrize("keys", [((D, 1), (D, 1)), ((D, None),), ((None, 1),), ((D, 2),),
                                        ((dt.date(2020, 1, 4), 1),), ((dt.date(2021, 1, 5), 1),), ()])
def test_exact_key_rejection(tmp_path, con, keys):
    panel = axis(tmp_path)
    path = tmp_path / "bad.parquet"
    payload(path, keys)
    with pytest.raises(RuntimeError):
        V.validate_keys(con, path, [panel], tmp_path / "calendar.parquet", 2021)


def test_schema_and_clock_refusal(tmp_path, con):
    panel = axis(tmp_path)
    path = tmp_path / "bad.parquet"
    pq.write_table(pa.table({"year": [2021]}), path)
    with pytest.raises(RuntimeError, match="schema"):
        V.validate_keys(con, path, [panel], tmp_path / "calendar.parquet", 2021)
    payload(path, clock=dt.datetime(2021, 1, 4, 21))
    with pytest.raises(RuntimeError, match="clock"):
        V.validate_keys(con, path, [panel], tmp_path / "calendar.parquet", 2021)
    payload(path)
    table = pq.read_table(path)
    table = table.set_column(table.schema.get_field_index("security_id"), "security_id", pa.array(["1"]))
    pq.write_table(table, path)
    with pytest.raises(RuntimeError, match="key types"):
        V.validate_keys(con, path, [panel], tmp_path / "calendar.parquet", 2021)
    payload(path)
    table = pq.read_table(path)
    table = table.set_column(table.schema.get_field_index("available_at"), "available_at", pa.array([None], type=pa.timestamp("us")))
    pq.write_table(table, path)
    with pytest.raises(RuntimeError, match="clock"):
        V.validate_keys(con, path, [panel], tmp_path / "calendar.parquet", 2021)


def binding(root, relative):
    return {"files": {relative: R.identity(root / relative)}, "source": "offline primary fixture",
            "clock": "fixture session close UTC", "admission": "independent byte binding; not economic certification"}


def test_snapshot_omission_extras_mutation(tmp_path):
    folder = tmp_path / "security_master"
    folder.mkdir()
    path = folder / "finra_names.parquet"
    path.write_bytes(b"a")
    other = folder / "other.parquet"
    other.write_bytes(b"b")
    C.write_json_atomic(folder / "manifest.json", {"files": {"other.parquet": R.identity(other)}})
    contract = {"finra": {"pattern": "security_master/finra_names.parquet", "manifest": "security_master/manifest.json"}}
    bound = {"finra": binding(tmp_path, "security_master/finra_names.parquet")}
    frozen = R.snapshot(tmp_path, contract, bound, [])
    assert "security_master/finra_names.parquet" in frozen["files"]
    with pytest.raises(RuntimeError, match="complete reader"):
        R.snapshot(tmp_path, contract, {}, [])
    path.write_bytes(b"c")
    with pytest.raises(RuntimeError, match="stale"):
        R.recheck(tmp_path, frozen)
    path.unlink()
    with pytest.raises(RuntimeError, match="members"):
        R.snapshot(tmp_path, contract, bound, [])
    path.write_bytes(b"a")
    C.write_json_atomic(folder / "manifest.json", {"files": {}})
    with pytest.raises(RuntimeError, match="empty producer"):
        R.snapshot(tmp_path, contract, bound, [])


def test_wildcard_extra_and_path_escape(tmp_path):
    (tmp_path / "a.parquet").write_bytes(b"a")
    contract = {"panel": {"pattern": "*.parquet", "manifest": None}}
    bounds = {"panel": binding(tmp_path, "a.parquet")}
    frozen = R.snapshot(tmp_path, contract, bounds, [])
    (tmp_path / "b.parquet").write_bytes(b"b")
    with pytest.raises(RuntimeError, match="members"):
        R.recheck(tmp_path, frozen)
    with pytest.raises(ValueError):
        R.safe_path(tmp_path, "../outside")


def candidate_fixture(tmp_path):
    panel = axis(tmp_path)
    frozen = R.snapshot(tmp_path, {"panel_2021": {"pattern": "panel/year=2021/*.parquet", "manifest": None},
                                  "calendar": {"pattern": "calendar.parquet", "manifest": None}},
                        {"panel_2021": binding(tmp_path, panel.relative_to(tmp_path).as_posix()),
                         "calendar": binding(tmp_path, "calendar.parquet")}, [])
    return V.Candidate(tmp_path / "generation", frozen, tmp_path)


def test_authenticated_resume_corruption_and_interruption(tmp_path, con):
    candidate = candidate_fixture(tmp_path)
    payload(candidate.payload(2021))
    with pytest.raises(RuntimeError, match="interrupted"):
        candidate.admit(con, 2021)
    daily = {D.isoformat(): {col: 1 for col in U.DAILY_COLS}}
    sealed = candidate.seal(con, 2021, daily, None)
    resumed = V.Candidate(candidate.directory, candidate.frozen, tmp_path)
    assert resumed.admit(con, 2021) == sealed
    payload(candidate.payload(2021), ((D, 2),))
    with pytest.raises(RuntimeError, match="corrupt"):
        resumed.admit(con, 2021)


def test_corrupt_daily_and_stale_sources(tmp_path, con):
    candidate = candidate_fixture(tmp_path)
    payload(candidate.payload(2021))
    daily = {D.isoformat(): {col: 1 for col in U.DAILY_COLS}}
    candidate.seal(con, 2021, daily, None)
    receipt = candidate.directory / "year=2021/receipt.json"
    row = C.read_json(receipt)
    row["daily"][D.isoformat()][U.DAILY_COLS[0]] = 0
    C.write_json_atomic(receipt, row)
    with pytest.raises(RuntimeError, match="daily"):
        candidate.admit(con, 2021)
    (tmp_path / "calendar.parquet").write_bytes(b"stale")
    with pytest.raises(RuntimeError, match="stale"):
        candidate.seal(con, 2021, daily, None)


def test_adapter_isolation_and_full_inventory(tmp_path):
    original = U._p("universe", "year=2021", "universe.parquet")
    a = V.native_adapter(tmp_path / "a", tmp_path / "a_candidate")
    b = V.native_adapter(tmp_path / "b", tmp_path / "b_candidate")
    assert "a_candidate" in a.out_glob() and "b_candidate" in b.out_glob()
    assert "/a/" in a.M.calendar_sql() and "/b/" in b.M.calendar_sql()
    assert U._p("universe", "year=2021", "universe.parquet") == original
    contract = R.reader_contract()
    assert all(f"panel_{y}" in contract for y in V.REQUIRED)
    assert all(name in contract for name in ("finra_names", "profile", "reconstitutions", "calendar"))


def test_publish_last_interruption_and_rollback(tmp_path, con, monkeypatch):
    stage = tmp_path / V.STAGE
    first = stage / "generations/first"
    second = stage / "generations/second"
    manifest = {"stage": V.STAGE, "status": "complete", "readiness": "NOT_PRODUCTION_READY",
                "required_years": list(V.REQUIRED), "files": {f"year={year}/universe.parquet": {} for year in V.REQUIRED}}
    C.write_json_atomic(first / "manifest.json", {**manifest, "fixture": 1})
    C.write_json_atomic(second / "manifest.json", {**manifest, "fixture": 2})
    V.publish_pointer(stage, first, first / "manifest.json")
    original = C.write_json_atomic
    def interrupted(path, value):
        if path.name == "current.json":
            raise RuntimeError("interrupt")
        original(path, value)
    monkeypatch.setattr(C, "write_json_atomic", interrupted)
    with pytest.raises(RuntimeError, match="interrupt"):
        V.publish_pointer(stage, second, second / "manifest.json")
    assert V.resolve_current(tmp_path) == first.resolve()
    monkeypatch.setattr(C, "write_json_atomic", original)
    V.publish_pointer(stage, second, second / "manifest.json")
    monkeypatch.setattr(V, "validate_generation", lambda root, generation, con: {})
    V.rollback(tmp_path, con)
    assert V.resolve_current(tmp_path) == first.resolve()
    (first / "manifest.json").write_text("{}")
    with pytest.raises(RuntimeError, match="pointer"):
        V.resolve_current(tmp_path)


def test_full_inventory_bounded_resume_equivalence_and_honest_fail(tmp_path, monkeypatch):
    """All nine original years are required even when a caller selects one."""
    import types
    original_connect = C.connect
    monkeypatch.setattr(C, "connect", lambda **kwargs: original_connect(**{**kwargs, "memory": "128MB", "threads": 1}))
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    contract, products = {}, {}
    days = []
    for year in V.REQUIRED:
        day = dt.date(year, 1, 4)
        days.append(day)
        relative = f"panel/year={year}/p.parquet"
        path = tmp_path / relative
        payload(path, ((day, 1),))
        contract[f"panel_{year}"] = {"pattern": relative, "manifest": None}
        products[f"panel_{year}"] = binding(tmp_path, relative)
    pq.write_table(pa.table({"session_date": pa.array(days, type=pa.date32())}), tmp_path / "calendar.parquet")
    contract["calendar"] = {"pattern": "calendar.parquet", "manifest": None}
    products["calendar"] = binding(tmp_path, "calendar.parquet")
    bindings = tmp_path / "bindings.json"
    C.write_json_atomic(bindings, {"version": "reader_binding_v1", "products": products})
    hand = tmp_path / "hand.json"
    hand.write_text("{}")
    french = tmp_path / "french.parquet"
    french.write_bytes(b"fixture")
    monkeypatch.setattr(U, "HAND_CHECK", hand)
    monkeypatch.setattr(U, "FRENCH_BREAKPOINTS", french)
    monkeypatch.setattr(R, "reader_contract", lambda years=V.REQUIRED: contract)
    def adapter(root, generation):
        state = {}
        def build_year(con, year):
            state["year"] = year
        return types.SimpleNamespace(load_issuer_tables=lambda con: None, build_year=build_year,
            output_sql=lambda: "SELECT * FROM " + V.parquet_sql([tmp_path / f"panel/year={state['year']}/p.parquet"]),
            year_receipt=lambda con: [[dt.date(state["year"], 1, 4), *([1] * len(U.DAILY_COLS))]])
    monkeypatch.setattr(V, "native_adapter", adapter)
    calls = []
    def measured(con, native, daily):
        calls.append(len(daily))
        return {"gates": {"french_nyse": {"pass": False, "months_outside": 10, "months": 60}},
                "coverage": {"fixture": True}, "sample": {"fixture": True}}
    monkeypatch.setattr(V, "measurements", measured)
    partial = V.build(bindings, "bounded", years=[2018], max_new_years=1)
    assert partial["missing_years"] == list(range(2019, 2027)) and not partial["published"]
    assert not calls and not (tmp_path / V.STAGE / "current.json").exists()
    assert V.build(bindings, "bounded", max_new_years=0)["missing_years"] == partial["missing_years"]
    completed = V.build(bindings, "bounded", max_new_years=8)
    assert completed["gate_status"]["french_nyse"] == "FAIL"
    first = V.resolve_current(tmp_path)
    V.build(bindings, "continuous")
    second = V.resolve_current(tmp_path)
    assert calls == [9, 9]
    for year in V.REQUIRED:
        assert R.identity(first / f"year={year}/universe.parquet") == R.identity(second / f"year={year}/universe.parquet")
    db = C.connect(memory="128MB", threads=1, temp_dir=tmp_path / "check")
    try:
        V.validate_generation(tmp_path, second, db)
        V.rollback(tmp_path, db)
        assert V.resolve_current(tmp_path) == first
    finally:
        db.close()
