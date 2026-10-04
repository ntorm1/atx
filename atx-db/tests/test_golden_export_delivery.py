import datetime as dt
import json
import numpy as np
import pytest
from atx_db.alpha_panel import golden_export as X, golden_sources as S, golden_panel as P, golden_registry as R


def role_fixture(tmp_path):
    directory=tmp_path/"role";directory.mkdir()
    d,n=400,2
    first=(dt.date(2019,1,1)-dt.date(1970,1,1)).days
    sessions=np.arange(first,first+d,dtype="<i8")*X.DAY_NS
    ids=np.array([1,2],dtype="<u8")
    present=np.ones((d,n),dtype="u1");members=present.copy();members[:63]=0
    values=np.full((d,n),10.0,dtype="<f8")
    for name,data in {"sessions.i64":sessions,"ids.u64":ids,"present.u8":present,"member.u8":members,
                      "close.f64":values,"raw_close.f64":values,"volume.f64":np.full((d,n),1e6,dtype="<f8")}.items():
        (directory/name).write_bytes(data.tobytes())
    recipe={"rule":"research-prior63-usd-adv-topn-v1","lookback_sessions":63,"lag_sessions":1,"min_raw_price_exclusive":5,
            "min_adv_exclusive":5000000,"ties":"securityID-ascending","missing":"complete-prior-calendar-window-required",
            "common_stock_verified":False,"top_n":2}
    m={"schema":X.ROLE_SCHEMA,"status":"complete","instrument_namespace":"spiderrock.securityID","close_basis":"f64(raw-f32-close)*f64-cumulReturnFactor",
       "volume_basis":"raw-share-volume","clock_recipe":"modeled-session+22h-mark+23h-decision-v1","common_stock_verified":False,
       "historical_vintage_verified":False,"dates":d,"instruments":n,"declared_output_bytes":26*d*n+8*d+8*n,
       "files":{name:S.pin(directory/name) for name in X.EXPECTED_FILES},"score_begin":383,"score_end":d,"score_start_ns":int(sessions[383]),
       "score_end_ns":int(sessions[-1]+X.DAY_NS),"source_sha256":"a"*64,"membership_recipe":json.dumps(recipe)}
    P.C.write_json_atomic(directory/"manifest.json",m)
    return directory


def test_exact_bundle_dtype_order_nan_binding_and_exclusions(tmp_path):
    role_dir=role_fixture(tmp_path);role_pin=S.pin(role_dir/"manifest.json")
    role=X.admit_role(role_dir,role_pin)
    con=P.connect(tmp_path/"db")
    try:
        generation=tmp_path/"generation";generation.mkdir()
        query="SELECT DATE '2020-01-19' session_date,2::BIGINT security_id,42::DOUBLE me,TIMESTAMPTZ '2020-01-17 22:00:00+00' me_source_at,TIMESTAMPTZ '2020-01-18 22:00:00+00' available_at,false univ_all_common"
        path=generation/"daily/year=2020/panel.parquet";P.C.copy_to_parquet(con,query,path)
        P.C.write_json_atomic(generation/"manifest.json",{"files":{"daily/year=2020/panel.parquet":S.pin(path)}})
        result=X.write_bundle(con,generation,role,["me"],tmp_path/"fields")
        assert result["role"]==role["role_binding"]
        values=np.fromfile(tmp_path/"fields/me.f64",dtype="<f8").reshape(400,2)
        assert values[383,1]==42 and np.isnan(values[383,0])
        assert np.count_nonzero(np.isfinite(values))==1
        assert result["alignment"]["role_member_outside_all_common"]==1
        assert len(result["excluded_fields"])==len(R.registry())-1
        assert X.validate_bundle(tmp_path/"fields",role_dir,role_pin)["runtime_acceptance"]=="PENDING"
        with (tmp_path/"fields/me.f64").open("ab") as f:f.write(b"changed")
        with pytest.raises(ValueError,match="hash/extent"):X.validate_bundle(tmp_path/"fields",role_dir,role_pin)
    finally:con.close()


def test_role_recipe_axes_source_and_actual_seal_failures(tmp_path):
    directory=role_fixture(tmp_path)
    m=P.C.read_json(directory/"manifest.json")
    for change in ({"common_stock_verified":True},{"historical_vintage_verified":True},{"score_begin":382},{"declared_output_bytes":1}):
        P.C.write_json_atomic(directory/"manifest.json",{**m,**change})
        with pytest.raises(ValueError):X.admit_role(directory,S.pin(directory/"manifest.json"))
    P.C.write_json_atomic(directory/"manifest.json",m)
    ids=np.array([2,1],dtype="<u8");(directory/"ids.u64").write_bytes(ids.tobytes())
    m["files"]["ids.u64"]=S.pin(directory/"ids.u64");P.C.write_json_atomic(directory/"manifest.json",m)
    with pytest.raises(ValueError,match="axes"):X.admit_role(directory,S.pin(directory/"manifest.json"))


def test_role_exact_membership_recomputed_not_manifest_asserted(tmp_path):
    directory=role_fixture(tmp_path);path=directory/"member.u8"
    blob=bytearray(path.read_bytes());blob[200]=0;path.write_bytes(blob)
    m=P.C.read_json(directory/"manifest.json");m["files"]["member.u8"]=S.pin(path);P.C.write_json_atomic(directory/"manifest.json",m)
    with pytest.raises(ValueError,match="membership replay"):X.admit_role(directory,S.pin(directory/"manifest.json"))


def test_bundle_field_limit_and_declared_nonpit(tmp_path,monkeypatch):
    with pytest.raises(ValueError,match="1..64"):
        X.write_bundle(None,None,None,list(R.registry())[:65],tmp_path/"never")


def test_public_export_native_cell_replay_and_atomic_publication(tmp_path,monkeypatch):
    role_dir=role_fixture(tmp_path);role_pin=S.pin(role_dir/'manifest.json')
    stage=tmp_path/'golden';generation=stage/'generations'/'fixture'
    con=P.connect(tmp_path/'source-db')
    try:
        query="SELECT DATE '2020-01-19' session_date,2::BIGINT security_id,42::DOUBLE me,TIMESTAMPTZ '2020-01-17 22:00:00+00' me_source_at,TIMESTAMPTZ '2020-01-18 22:00:00+00' available_at,false univ_all_common"
        path=generation/'daily/year=2020/panel.parquet';P.C.copy_to_parquet(con,query,path)
    finally:con.close()
    P.C.write_json_atomic(generation/'manifest.json',{'files':{'daily/year=2020/panel.parquet':S.pin(path)}})
    monkeypatch.setattr(P,'validate_generation',lambda stage,path,con:{'status':'valid_diagnostic'})
    output=tmp_path/'export'
    X.export(stage,'fixture',role_dir,role_pin,['me'],output)
    assert X.validate_export(output,role_dir,role_pin)['status']=='valid_native_replay'
    values=np.fromfile(output/'me.f64',dtype='<f8').reshape(400,2);values[383,1]=43
    (output/'me.f64').write_bytes(values.tobytes())
    manifest=P.C.read_json(output/'manifest.json');identity=S.pin(output/'me.f64')
    manifest['files']['me.f64']=identity;manifest['fields'][0]['sha256']=identity['sha256']
    P.C.write_json_atomic(output/'manifest.json',manifest)
    assert X.validate_bundle(output,role_dir,role_pin)['status']=='valid_offline_shape'
    with pytest.raises(ValueError,match='native export values differ'):X.validate_export(output,role_dir,role_pin)
    registry=R.registry();registry["me"]={**registry["me"],"point_in_time":False,"non_pit_aspects":["future presence"]}
    monkeypatch.setattr(R,"registry",lambda:registry)
    with pytest.raises(ValueError,match="non-PIT"):
        X.write_bundle(None,None,None,["me"],tmp_path/"never")
