import datetime as dt
import json
import duckdb
import pytest
from atx_db.alpha_panel import char_monthly as M,char_monthly_sources as N,release_inputs as R,common as C


def setup(monkeypatch,tmp_path):
    dates=[dt.date(2022,6,30),dt.date(2022,7,29)]
    source=tmp_path/"fixture-source.txt";source.write_text("frozen synthetic source",encoding="utf-8")
    frozen={"fixture":R.identity(source),"manifests":{}}
    def connect(**kw):
        con=duckdb.connect(config={"memory_limit":"128MB","threads":"1"});con.execute("SET TimeZone='UTC'");return con
    def admit(root,bindings,con):
        if R.identity(source)!=frozen["fixture"]:raise RuntimeError("fixture independently bound source changed")
        return frozen
    def population(con,date):return [{"session_date":date,"security_id":1,"cik":10,**{p:True for p in M.POPS}}]
    def unit(con,date):
        defs=','.join(f'"{name}" {kind}' for name,kind in M.BASE)+','+','.join(f'"{f}" DOUBLE' for f in M.FEATURES)
        con.execute("DROP TABLE IF EXISTS cm_output");con.execute(f"CREATE TEMP TABLE cm_output({defs})")
        row={"session_date":date,"security_id":1,"cik":10,"available_at":N.clock(date),
             **{p:True for p in M.POPS},"quality_reasons":json.dumps({f:"fixture_unknown" for f in M.FEATURES}),"source_clocks":"{}"}
        params=[row.get(name) for name,_ in M.BASE]+[None for _ in M.FEATURES]
        con.execute("INSERT INTO cm_output VALUES("+','.join('?' for _ in params)+")",params)
        return M.validate_unit(con,"cm_output",date,population(con,date))
    monkeypatch.setattr(C,"connect",connect);monkeypatch.setattr(N,"admit",admit)
    monkeypatch.setattr(N,"code_paths",lambda:[source]);monkeypatch.setattr(N,"months",lambda *_:dates)
    monkeypatch.setattr(N,"population",population);monkeypatch.setattr(M,"native_month",unit)
    monkeypatch.setattr(R,"recheck",lambda root,snapshot:admit(root,{},None))
    return dates,source


def test_bounded_resume_atomic_pointer_and_corruption_rejection(monkeypatch,tmp_path):
    dates,source=setup(monkeypatch,tmp_path)
    result=M.build(tmp_path,{},"fixture-g",dt.date(2020,1,1),dt.date(2025,12,31),max_new_months=1)
    assert result["status"]=="partial" and result["published"] is False
    stage=tmp_path/M.STAGE;generation=stage/"generations"/"fixture-g"
    assert not (stage/"current.json").exists()
    stamp=generation/"month=2022-06"/"receipt.json"
    original=stamp.read_bytes()
    result=M.build(tmp_path,{},"fixture-g",dt.date(2020,1,1),dt.date(2025,12,31),max_new_months=1)
    assert result["status"]=="complete"
    assert stamp.read_bytes()==original
    assert M.resolve_current(tmp_path)==generation
    pointer=(stage/"current.json").read_bytes()
    source.write_text("changed synthetic source",encoding="utf-8")
    with pytest.raises(RuntimeError,match="bound source changed"):
        M.build(tmp_path,{},"replacement",dt.date(2020,1,1),dt.date(2025,12,31))
    assert (stage/"current.json").read_bytes()==pointer
    payload=generation/"month=2022-06"/"char_monthly.parquet"
    payload.write_bytes(payload.read_bytes()+b"corrupt")
    source.write_text("frozen synthetic source",encoding="utf-8")
    con=C.connect()
    try:
        with pytest.raises(RuntimeError,match="inventory differs"):M.validate_generation(tmp_path,generation,con)
    finally:con.close()


def test_writer_lock_is_exclusive_and_does_not_remove_foreign_lock(tmp_path):
    stage=tmp_path/"stage"
    with M.writer_lock(stage):
        with pytest.raises(FileExistsError):
            with M.writer_lock(stage):pass
        assert (stage/"writer.lock").exists()
    assert not (stage/"writer.lock").exists()
