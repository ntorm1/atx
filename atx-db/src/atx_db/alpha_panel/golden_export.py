"""Exact <=64-field bundles on pinned consumer role axes; consumer seals unchanged."""
from __future__ import annotations
import datetime as dt
import json
import os
from pathlib import Path
import numpy as np
from . import common as C, golden_panel as P, golden_registry as R, golden_sources as S

DAY_NS = 86400*10**9
SEAL = dt.date(2023, 1, 1)
ROLE_SCHEMA = "atx.recent-research-role/v1"
FIELDS_SCHEMA = "atx.research-role-fields/v1"
EXPECTED_FILES = {"sessions.i64": 8, "ids.u64": 8, "present.u8": 1, "member.u8": 1,
                  "close.f64": 8, "raw_close.f64": 8, "volume.f64": 8}


def admit_role(directory, expected_manifest, max_bytes=512*1024**2):
    """Read-only complete C++ recipe checks plus independent membership recomputation.

Memory mapped panels, bounded per-date slices. This is compatibility validation,
not historical source/common-stock certification or executable runtime acceptance.
    """
    directory = Path(directory).resolve()
    if S.pin(directory/"manifest.json") != expected_manifest:
        raise ValueError("independently pinned role manifest changed")
    m = C.read_json(directory/"manifest.json")
    fixed = {"schema": ROLE_SCHEMA, "status": "complete", "instrument_namespace": "spiderrock.securityID",
        "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
        "clock_recipe": "modeled-session+22h-mark+23h-decision-v1", "common_stock_verified": False,
        "historical_vintage_verified": False}
    if any(m.get(k) != v for k,v in fixed.items()) or type(m.get("common_stock_verified")) is not bool or type(m.get("historical_vintage_verified")) is not bool:
        raise ValueError("unsupported honest consumer role recipe")
    d, n = m["dates"], m["instruments"]
    if type(d) is not int or type(n) is not int or not 1<=d<=4096 or not 1<=n<=20000:
        raise ValueError("role axis limits")
    allocation = 26*d*n+8*d+8*n
    if m.get("declared_output_bytes") != allocation or allocation+16*1024**2 > max_bytes:
        raise ValueError("declared consumer budget")
    if set(m["files"]) != set(EXPECTED_FILES):
        raise ValueError("complete exact role inventory required")
    pins = {}
    for name, width in EXPECTED_FILES.items():
        expected_size = width*(d if name == "sessions.i64" else n if name == "ids.u64" else d*n)
        if S.pin(directory/name) != m["files"][name] or m["files"][name]["bytes"] != expected_size:
            raise ValueError("role payload hash/extent mismatch")
        pins[str(directory/name)] = m["files"][name]
    sessions = np.memmap(directory/"sessions.i64", dtype="<i8", mode="r")
    ids = np.memmap(directory/"ids.u64", dtype="<u8", mode="r")
    if np.any(sessions<=0) or np.any(np.diff(sessions)<=0) or np.any(sessions%DAY_NS) or np.any(ids==0) or np.any(ids[1:]<=ids[:-1]):
        raise ValueError("nonunique/unsorted role axes")
    seal_ns = (SEAL-dt.date(1970,1,1)).days*DAY_NS
    if sessions[-1]>=seal_ns or m["score_end_ns"]>seal_ns:
        raise ValueError("release role must be separately pinned wholly pre2023")
    begin, end = m["score_begin"], m["score_end"]
    if type(begin) is not int or type(end) is not int or not 383<=begin<end==d or np.searchsorted(sessions,m["score_start_ns"])!=begin or sessions[-1]>=m["score_end_ns"]:
        raise ValueError("role score/warmup axes mismatch")
    recipe = json.loads(m["membership_recipe"])
    required = {"rule":"research-prior63-usd-adv-topn-v1", "lookback_sessions":63, "lag_sessions":1,
        "min_raw_price_exclusive":5, "min_adv_exclusive":5000000, "ties":"securityID-ascending",
        "missing":"complete-prior-calendar-window-required", "common_stock_verified":False}
    if any(recipe.get(k)!=v for k,v in required.items()) or type(recipe.get("top_n")) is not int or not 2<=recipe["top_n"]<=20000:
        raise ValueError("unsupported exact membership recipe")
    if not isinstance(m.get("source_sha256"),str) or len(m["source_sha256"])!=64 or any(c not in "0123456789abcdef" for c in m["source_sha256"]):
        raise ValueError("role source identity missing")
    arrays = {name:np.memmap(directory/name,dtype="u1" if width==1 else "<f8",mode="r",shape=(d,n))
              for name,width in EXPECTED_FILES.items() if name not in ("sessions.i64","ids.u64")}
    for t in range(d):
        present, members = arrays["present.u8"][t], arrays["member.u8"][t]
        if np.any(present>1) or np.any(members>1):
            raise ValueError("nonbinary role masks")
        for name in ("close.f64","raw_close.f64","volume.f64"):
            values = arrays[name][t]
            if np.any(~np.isnan(values[present==0])) or np.any(~np.isfinite(values[present==1])) or np.any(values[present==1] < 0 if name=="volume.f64" else values[present==1]<=0):
                raise ValueError("role present/value invariants")
        expected = np.zeros(n,dtype="u1")
        if t>=63:
            raw = arrays["raw_close.f64"][t-63:t]
            volume = arrays["volume.f64"][t-63:t]
            complete = np.all(arrays["present.u8"][t-63:t]==1,axis=0)
            adv = np.mean(raw*volume,axis=0)
            eligible = np.flatnonzero(complete & (raw[-1]>5) & (adv>5000000))
            order = sorted(eligible,key=lambda i:(-adv[i],int(ids[i])))[:recipe["top_n"]]
            expected[order]=1
        if not np.array_equal(expected,members):
            raise ValueError("independent exact ADV membership replay differs")
    pins[str(directory/"manifest.json")] = expected_manifest
    S.check_pins(pins)
    return {"directory":directory, "manifest":m, "pins":pins, "sessions":sessions, "ids":ids,
            "arrays":arrays, "role_binding":{"manifest_sha256":expected_manifest["sha256"],
                "sessions_sha256":m["files"]["sessions.i64"]["sha256"], "ids_sha256":m["files"]["ids.u64"]["sha256"],
                "dates":d,"instruments":n}}


def write_bundle(con, generation, role, names, output):
    names = list(names); registry = R.registry(); output=Path(output)
    if not 1<=len(names)<=64 or len(set(names))!=len(names) or any(n not in registry for n in names):
        raise ValueError("unique explicit 1..64-field bundle required")
    if any(not registry[n]["point_in_time"] or registry[n]["non_pit_aspects"] for n in names):
        raise ValueError("consumer refuses declared non-PIT fields")
    if output.exists():
        raise ValueError("immutable export target already exists")
    generation=Path(generation)
    manifest=C.read_json(generation/"manifest.json")
    source_pin=S.pin(generation/"manifest.json")
    paths=[generation/n for n in manifest["files"] if n.startswith("daily/")]
    con.execute(f"CREATE OR REPLACE TEMP VIEW ge AS SELECT * FROM {P.sql(paths)}")
    output.mkdir(parents=True)
    d,n=role["manifest"]["dates"],role["manifest"]["instruments"]
    files,fields={},[]
    alignment={"role_member_cells":0,"role_member_missing_golden":0,"role_member_outside_all_common":0,
               "all_common_role_axis_cells":0,"all_common_outside_role_member":0,
               "golden_all_common_alignment":"separately verified by golden full native replay"}
    for t,session_ns in enumerate(role["sessions"]):
        date=dt.date(1970,1,1)+dt.timedelta(days=int(session_ns)//DAY_NS)
        members=role["arrays"]["member.u8"][t]
        rows=con.execute("SELECT security_id,univ_all_common FROM ge WHERE session_date=?",[date]).fetchall()
        lookup={sid:flag for sid,flag in rows}
        for i,sid in enumerate(role["ids"]):
            sid=int(sid)
            if members[i]:
                alignment["role_member_cells"]+=1
                alignment["role_member_missing_golden"]+=sid not in lookup
                alignment["role_member_outside_all_common"]+=sid in lookup and not lookup[sid]
            if lookup.get(sid):
                alignment["all_common_role_axis_cells"]+=1
                alignment["all_common_outside_role_member"]+=not bool(members[i])
    for name in names:
        path=output/(name+".f64"); finite=0; member_finite=0
        with path.open("xb") as stream:
            for t,session_ns in enumerate(role["sessions"]):
                date=dt.date(1970,1,1)+dt.timedelta(days=int(session_ns)//DAY_NS)
                data=np.full(n,np.nan,dtype="<f8")
                rows=con.execute(f"SELECT security_id,{P.ident(name)},{P.ident(name+'_source_at')},available_at FROM ge WHERE session_date=?",[date]).fetchall()
                seen=set()
                for sid,value,source_at,cutoff in rows:
                    if sid in seen:
                        raise ValueError("duplicate export golden key")
                    seen.add(sid)
                    i=int(np.searchsorted(role["ids"],sid))
                    if i>=n or int(role["ids"][i])!=sid or not role["arrays"]["present.u8"][t,i]:
                        continue
                    if value is not None and np.isfinite(value):
                        if source_at is None or source_at>=cutoff:
                            raise ValueError("export source availability leak")
                        data[i]=value
                finite+=int(np.isfinite(data).sum())
                member_finite+=int(np.count_nonzero(np.isfinite(data)&(role["arrays"]["member.u8"][t]!=0)))
                stream.write(data.tobytes())
            stream.flush();os.fsync(stream.fileno())
        identity=S.pin(path)
        if identity["bytes"]!=8*d*n:
            raise ValueError("field extent mismatch")
        files[path.name]=identity
        definition=registry[name]
        fields.append({"name":name,"file":path.name,"sha256":identity["sha256"],"dtype":"<f8","layout":"date-major","shape":[d,n],
            "units":definition["unit"], **{k:definition[k] for k in ("clock","staleness","source_columns","definition","point_in_time","non_pit_aspects")},
            "coverage":{"finite_cells":finite,"finite_member_cells":member_finite},
            "historical_vintage_verified":False,"common_stock_verified":False})
    S.check_pins(role["pins"])
    if S.pin(generation/"manifest.json")!=source_pin:
        raise ValueError("golden source changed during export")
    m={"schema":FIELDS_SCHEMA,"status":"complete","instrument_namespace":"spiderrock.securityID",
        "role":role["role_binding"],"fields":fields,"files":files,"producer":"atx_db.alpha_panel.golden_export",
        "seal":str(SEAL),"sources":{"golden_manifest":source_pin,"golden_generation":str(generation.resolve())},"alignment":alignment,
        "excluded_fields":{k:"explicit selected <=64-field bundle; retained in full panel/registry" for k in registry if k not in names},
        "source_quality_certified":False,"historical_vintage_verified":False,"common_stock_verified":False,
        "runtime_acceptance":"PENDING_ROOT_ATX_IMPL_LOAD_SMOKE"}
    C.write_json_atomic(output/"manifest.json",m)
    return m


def export(stage,generation_id,role_dir,role_manifest_pin,fields,output):
    import uuid
    stage=Path(stage).resolve();generation=S.member(stage/"generations",generation_id)
    output=Path(output).resolve()
    if output.exists():raise ValueError('immutable export already exists')
    pending=output.with_name('.'+output.name+'.pending-'+uuid.uuid4().hex)
    con=P.connect(stage/"_scratch"/"export")
    try:
        P.validate_generation(stage,generation,con)
        role=admit_role(role_dir,role_manifest_pin)
        result=write_bundle(con,generation,role,fields,pending)
        validate_export(pending,role_dir,role_manifest_pin,con=con)
        P.validate_generation(stage,generation,con)
        with P.writer_lock(output.parent):
            if output.exists():raise ValueError('immutable export publication collision')
            pending.rename(output)
        return result
    finally:
        con.close()


def validate_bundle(directory,role_dir,role_manifest_pin):
    directory=Path(directory);m=C.read_json(directory/"manifest.json")
    before=S.pin(directory/"manifest.json")
    role=admit_role(role_dir,role_manifest_pin)
    if m.get("schema")!=FIELDS_SCHEMA or m.get("status")!="complete" or m["role"]!=role["role_binding"]:
        raise ValueError("fields role binding mismatch")
    names=[f["name"] for f in m["fields"]]
    if not 1<=len(names)<=64 or len(set(names))!=len(names) or set(m["files"])!={n+".f64" for n in names}:
        raise ValueError("bundle inventory/field count")
    d,n=role["manifest"]["dates"],role["manifest"]["instruments"]
    for f in m["fields"]:
        contract=R.registry()[f["name"]]
        if any(f[k]!=contract["unit" if k=="units" else k] for k in ("units","clock","staleness","source_columns","definition","point_in_time","non_pit_aspects")):
            raise ValueError("field definition changed")
        if not f["point_in_time"] or f["non_pit_aspects"] or f["dtype"]!="<f8" or f["layout"]!="date-major" or f["shape"]!=[d,n]:
            raise ValueError("unsupported field contract")
        if S.pin(directory/f["file"])!=m["files"][f["file"]] or f["sha256"]!=m["files"][f["file"]]["sha256"] or m["files"][f["file"]]["bytes"]!=8*d*n:
            raise ValueError("field hash/extent mismatch")
        values=np.memmap(directory/f["file"],dtype="<f8",mode="r",shape=(d,n))
        for t in range(d):
            if np.any(np.isinf(values[t])) or np.any(~np.isnan(values[t][role["arrays"]["present.u8"][t]==0])):
                raise ValueError("field missing/NaN ordering invariant")
    S.check_pins(role["pins"])
    if S.pin(directory/"manifest.json")!=before:
        raise ValueError("bundle manifest changed during validation")
    return {"status":"valid_offline_shape","runtime_acceptance":"PENDING","fields":len(names)}


def validate_export(directory,role_dir,role_manifest_pin,*,con=None):
    """Public native replay of every exported cell, separately from shape QA."""
    directory=Path(directory).resolve()
    validate_bundle(directory,role_dir,role_manifest_pin)
    identity=S.pin(directory/'manifest.json');m=C.read_json(directory/'manifest.json')
    generation=Path(m['sources']['golden_generation']).resolve()
    if generation.parent.name!='generations':raise ValueError('foreign export source generation')
    stage=generation.parent.parent
    owned=con is None
    if owned:con=P.connect(stage/'_scratch'/'export-replay')
    try:
        if S.pin(generation/'manifest.json')!=m['sources']['golden_manifest']:raise ValueError('changed export source')
        P.validate_generation(stage,generation,con)
        native=C.read_json(generation/'manifest.json')
        role=admit_role(role_dir,role_manifest_pin)
        con.execute('CREATE OR REPLACE TEMP VIEW ge AS SELECT * FROM '+P.sql([generation/n for n in native['files'] if n.startswith('daily/')]))
        d,n=role['manifest']['dates'],role['manifest']['instruments']
        for field in m['fields']:
            name=field['name'];values=np.memmap(directory/field['file'],dtype='<f8',mode='r',shape=(d,n))
            for t,session_ns in enumerate(role['sessions']):
                date=dt.date(1970,1,1)+dt.timedelta(days=int(session_ns)//DAY_NS)
                expected=np.full(n,np.nan,dtype='<f8');seen=set()
                for sid,value,clock,cutoff in con.execute(f'SELECT security_id,{P.ident(name)},{P.ident(name+"_source_at")},available_at FROM ge WHERE session_date=?',[date]).fetchall():
                    if sid in seen:raise ValueError('duplicate native export replay key')
                    seen.add(sid);i=int(np.searchsorted(role['ids'],sid))
                    if i>=n or int(role['ids'][i])!=sid or not role['arrays']['present.u8'][t,i]:continue
                    if value is not None and np.isfinite(value):
                        if clock is None or clock>=cutoff:raise ValueError('native export clock leak')
                        expected[i]=value
                if not np.array_equal(expected,values[t],equal_nan=True):raise ValueError('complete native export values differ')
        S.check_pins(role['pins'])
        if identity!=S.pin(directory/'manifest.json') or S.pin(generation/'manifest.json')!=m['sources']['golden_manifest']:raise ValueError('export source changed during public replay')
        return {'status':'valid_native_replay','runtime_acceptance':'PENDING_ROOT_ATX_IMPL_LOAD_SMOKE','fields':len(m['fields'])}
    finally:
        if owned:con.close()
