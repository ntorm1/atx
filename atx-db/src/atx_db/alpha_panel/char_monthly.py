"""Native char_monthly stage: frozen registry, bounded monthly atomic generation.

All real commands require the controller's exclusive memory-guard allocation.
Source bindings are independently reviewed exact-file contracts. Neither source
quality nor predictive significance is certified by a successful byte validation.
"""
from __future__ import annotations
import argparse
from contextlib import contextmanager
import datetime as dt
import json
import os
from pathlib import Path
import uuid
from . import common as C
from . import release_inputs as R
from . import char_monthly_sources as N
from . import char_monthly_kernels as K
from .char_monthly_registry import REGISTRY, BY_NAME, JKP153, FREEZE, FREEZE_SHA256

STAGE="char_monthly"
SCHEMA="atx.alpha-panel.char-monthly/v1"
POPS=("member_equity","univ_all_common","univ_r3000p")
FEATURES=tuple(f.name for f in REGISTRY)
BASE=(("session_date","DATE"),("security_id","BIGINT"),("cik","BIGINT"),
      ("available_at","TIMESTAMPTZ"),("source_available_at","TIMESTAMPTZ"),
      ("accounting_datadate","DATE"),("accounting_available_at","TIMESTAMPTZ"),("accounting_currency","VARCHAR"),
      ("issuer_me","DOUBLE"),("issuer_cap_reason","VARCHAR"),
      *((p,"BOOLEAN") for p in POPS),("quality_reasons","VARCHAR"),("source_clocks","VARCHAR"))


def registry_document():return {"freeze_sha256":FREEZE_SHA256,**FREEZE}


@contextmanager
def writer_lock(stage):
    stage.mkdir(parents=True,exist_ok=True)
    path=stage/"writer.lock"
    fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    try:
        os.write(fd,str(os.getpid()).encode());os.close(fd);yield
    finally:path.unlink()


def native_month(con,date):
    """One month, one issuer/security history at a time; disk-backed SQL table."""
    defs=",".join(f'"{name}" {kind}' for name,kind in BASE)+","+",".join(f'"{f}" DOUBLE' for f in FEATURES)
    con.execute("DROP TABLE IF EXISTS cm_output")
    con.execute(f"CREATE TEMP TABLE cm_output({defs})")
    population=N.population(con,date)
    history_calendar=N.months(con,dt.date(date.year-7,1,1),date)
    if not population:raise RuntimeError("empty independent month-end population")
    placeholders=",".join("?" for _ in range(len(BASE)+len(FEATURES)))
    cache={};buffer=[]
    for row in population:
        cik=row["cik"]
        if cik not in cache:
            a,ac=N.accounting(con,cik,date,"funda")
            q,qc=N.accounting(con,cik,date,"fundq")
            history,hc=N.accounting(con,cik,date,"funda",all_vintages=True)
            cap,cc=N.cap(con,cik,date,max((r["datadate"] for r in a),default=None))
            sectors=N.records(con,"SELECT sector,available_at FROM cm_sector WHERE cik=? AND available_at<? QUALIFY row_number() OVER(ORDER BY available_at DESC)=1",[cik,N.clock(date)]) if cik is not None else []
            sectors=[r for r in sectors if r.get("available_at") is not None and r.get("sector") is not None]
            cache[cik]=(a,q,cap,ac+qc+cc+hc+[r["available_at"] for r in sectors],sectors[0]["sector"] if sectors else None,history)
            if len(cache)>128:
                saved=cache[cik];cache.clear();cache[cik]=saved
        a,q,cap,clocks,sector,history=cache[cik]
        bars,monthly,pc=N.price_history(con,row["security_id"],date)
        cap=dict(cap)
        if bars and bars[-1].get("price_certified") is True:
            cap["first_observation"]=bars[-1].get("first_observation")
        # Exact historical cap products, not a current-cap substitution.
        values,reasons=K.compute(a,q,bars,monthly,date,cap,sector,history,history_calendar)
        allclocks=clocks+pc
        source=max(allclocks) if allclocks else None
        if source is not None and source>=N.clock(date):raise RuntimeError("native source clock escaped strict cutoff")
        feature_clocks={f:source.isoformat() if values[f] is not None and source else None for f in FEATURES}
        annual_latest=max(a,key=lambda r:r["datadate"]) if a else {}
        data={"session_date":date,"security_id":row["security_id"],"cik":cik,"available_at":N.clock(date),
              "accounting_datadate":annual_latest.get("datadate"),"accounting_available_at":annual_latest.get("available_at"),
              "accounting_currency":annual_latest.get("curcd"),"issuer_me":cap.get("me"),"issuer_cap_reason":cap.get("cap_reason"),
              "source_available_at":source,**{p:row.get(p) for p in POPS},
              "quality_reasons":json.dumps(reasons,sort_keys=True),"source_clocks":json.dumps(feature_clocks,sort_keys=True)}
        buffer.append([data[name] for name,_ in BASE]+[values[f] for f in FEATURES])
        if len(buffer)==32:
            con.executemany(f"INSERT INTO cm_output VALUES({placeholders})",buffer);buffer=[]
    if buffer:con.executemany(f"INSERT INTO cm_output VALUES({placeholders})",buffer)
    # Cross sections gather only their small dependency sets, never the wide month.
    for f in REGISTRY:
        if f.implementation!="cross_section" or f.return_derived and date>=dt.date(2023,1,1):continue
        columns=",".join(f'"{d}"' for d in f.dependencies)
        records=N.records(con,f"SELECT security_id,cik,univ_all_common,{columns} FROM cm_output")
        rows=[{**r,"values":{d:r[d] for d in f.dependencies},"reasons":{}} for r in records]
        # Restrict the kernel registry iteration to this feature's dependency
        # closure via sequential registry order; earlier composites live in table.
        K.cross_section(rows)
        updates=[(r["values"][f.name],r["security_id"]) for r in rows if K.finite(r["values"].get(f.name))]
        if updates:
            con.executemany(f'UPDATE cm_output SET "{f.name}"=?,quality_reasons=json_merge_patch(quality_reasons,\'{{"{f.name}":null}}\') WHERE security_id=?',updates)
            con.execute(f"UPDATE cm_output SET source_clocks=json_merge_patch(source_clocks, json_object('{f.name}',CAST(source_available_at AS VARCHAR))) WHERE \"{f.name}\" IS NOT NULL")
    return validate_unit(con,"cm_output",date,population)


def validate_unit(con,relation,date,population=None):
    schema=[(r[0],r[1]) for r in con.execute(f"DESCRIBE {relation}").fetchall()]
    expected_schema=[(name,"TIMESTAMP WITH TIME ZONE" if kind=="TIMESTAMPTZ" else kind) for name,kind in BASE]+[(f,"DOUBLE") for f in FEATURES]
    if schema!=expected_schema:raise RuntimeError("monthly exact schema differs")
    if con.execute(f"SELECT 1 FROM {relation} GROUP BY session_date,security_id HAVING count(*)<>1 LIMIT 1").fetchone():raise RuntimeError("duplicate monthly keys")
    bad=con.execute(f"""SELECT count(*) FROM {relation} WHERE session_date<>? OR session_date IS NULL OR security_id IS NULL
        OR available_at<>? OR source_available_at>=? OR quality_reasons IS NULL OR source_clocks IS NULL""",[date,N.clock(date),N.clock(date)]).fetchone()[0]
    if bad:raise RuntimeError("monthly key/clock/quality violation")
    if population is None:population=N.population(con,date)
    expected={(r["session_date"],r["security_id"]) for r in population}
    actual={(r[0],r[1]) for r in con.execute(f"SELECT session_date,security_id FROM {relation}").fetchall()}
    if not expected or actual!=expected:raise RuntimeError("monthly independent universe keys differ")
    for p in POPS:
        expected_flags={r["security_id"]:r.get(p) for r in population}
        if any(expected_flags[sid] is not value for sid,value in con.execute(f"SELECT security_id,{p} FROM {relation}").fetchall()):raise RuntimeError("monthly universe flags differ")
    for f in REGISTRY:
        clock_sql=f"TRY_CAST(json_extract_string(source_clocks,'$.{f.name}') AS TIMESTAMPTZ)"
        forbidden=(date>=dt.date(2023,1,1) and f.return_derived) or f.implementation=="unsupported"
        condition=f'''("{f.name}" IS NOT NULL AND (NOT isfinite("{f.name}") OR source_available_at IS NULL OR {clock_sql} IS NULL OR {clock_sql}>source_available_at OR {clock_sql}>=available_at OR {'true' if forbidden else 'false'})) OR ("{f.name}" IS NULL AND json_extract_string(quality_reasons,'$.{f.name}') IS NULL)'''
        if con.execute(f"SELECT count(*) FROM {relation} WHERE {condition}").fetchone()[0]:raise RuntimeError(f"feature value/clock/missingness violation:{f.name}")
    counts={}
    for p in POPS:
        den=con.execute(f"SELECT count(*) FROM {relation} WHERE {p}").fetchone()[0]
        aggregate=','.join(f'count(*) FILTER(WHERE "{f.name}" IS NOT NULL)' for f in REGISTRY)
        numerators=con.execute(f"SELECT {aggregate} FROM {relation} WHERE {p}").fetchone()
        fields={f.name:{"numerator":n,"denominator":den} for f,n in zip(REGISTRY,numerators)}
        counts[p]={"eligible_cells":den,"unknown_membership":con.execute(f"SELECT count(*) FROM {relation} WHERE {p} IS NULL").fetchone()[0],"fields":fields}
    return {"rows":len(actual),"key_sha256":R.digest(sorted((d.isoformat(),sid) for d,sid in expected)),"counts":counts}


def coverage(receipts):
    out={}
    for year in range(2020,2026):
        units=[r for key,r in receipts.items() if key.startswith(str(year))]
        for p in POPS:
            fields={}
            for f in REGISTRY:
                num=sum(r["measurement"]["counts"][p]["fields"][f.name]["numerator"] for r in units)
                den=sum(r["measurement"]["counts"][p]["fields"][f.name]["denominator"] for r in units)
                fields[f.name]={"numerator":num,"denominator":den,"coverage":num/den if den else None}
            out[f"{year}/{p}"]={"months":len(units),"fields":fields,"unknown_membership":sum(r["measurement"]["counts"][p]["unknown_membership"] for r in units)}
    return out


def gates(evidence):
    result={"implemented_original153":{"pass":sum(BY_NAME[n].implementation!="unsupported" for n in JKP153)>=150,
             "measured":sum(BY_NAME[n].implementation!="unsupported" for n in JKP153),"denominator":153,"required":150}}
    for family,threshold in (("accounting",.9),("price",.97)):
        # All original fields retain denominator, including unimplemented fields
        # and return-derived nulls in the original 2023--2025 gate years.
        features=[BY_NAME[n] for n in JKP153 if (BY_NAME[n].implementation!="market")== (family=="accounting")]
        cells=[evidence[f"{y}/univ_r3000p"]["fields"][f.name] for y in range(2020,2026) for f in features]
        measured=min((c["coverage"] for c in cells if c["coverage"] is not None),default=None)
        complete=all(evidence[f"{y}/univ_r3000p"]["months"]==12 for y in range(2020,2026)) and all(c["coverage"] is not None for c in cells)
        result[family+"_coverage"]={"pass":complete and measured is not None and measured>=threshold,"measured":measured,"required":threshold,"fields":len(features),"years":list(range(2020,2026))}
    for name,required in (("jkp_matched_rho_share",.7),("jkp_mean_sign_share",.9),("osap_median_rho",.6),("ratios_spearman",.99)):
        result[name]={"pass":False,"measured":None,"required":required,"reason":"no actual return/value comparison measured"}
    return result


def build(root,bindings,generation_id,start,end,max_new_months=None):
    if not generation_id or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in generation_id):raise ValueError("safe explicit generation id required")
    if start>dt.date(2020,1,1) or end<dt.date(2025,12,31):raise ValueError("original 2020-2025 coverage window cannot be reduced")
    if max_new_months is not None and max_new_months<1:raise ValueError("positive monthly build budget required")
    stage=root/STAGE
    with writer_lock(stage):
        con=C.connect(memory="380MB",threads=1,db_file=f"char-monthly-{uuid.uuid4().hex}.duckdb")
        try:
            snapshot=N.admit(root,bindings,con)
            dates=N.months(con,start,end)
            generation=R.safe_path(stage/"generations",generation_id);generation.mkdir(parents=True,exist_ok=True)
            frozen={"source":snapshot,"bindings":bindings,"registry_sha256":FREEZE_SHA256,"code":{str(p):R.identity(p) for p in N.code_paths()},"months":[d.isoformat() for d in dates],"start":str(start),"end":str(end)}
            if (generation/"snapshot.json").exists():
                if C.read_json(generation/"snapshot.json")!=frozen:raise RuntimeError("resume inputs/code/config/window differ")
            else:C.write_json_atomic(generation/"snapshot.json",frozen)
            receipts={};new=0
            for date in dates:
                key=date.isoformat();unit=generation/f"month={date:%Y-%m}";payload=unit/"char_monthly.parquet";stamp=unit/"receipt.json"
                if stamp.exists():
                    receipt=C.read_json(stamp)
                    if receipt.get("snapshot_sha256")!=R.digest(frozen) or R.identity(payload)!=receipt.get("payload"):raise RuntimeError("stale monthly receipt")
                    con.execute(f"CREATE OR REPLACE TEMP VIEW cm_check AS SELECT * FROM {N.sql([payload])}")
                    if validate_unit(con,"cm_check",date)!=receipt["measurement"]:raise RuntimeError("monthly receipt measurement mismatch")
                else:
                    if max_new_months is not None and new>=max_new_months:return {"status":"partial","generation":str(generation),"months":len(receipts),"published":False}
                    measurement=native_month(con,date)
                    C.copy_to_parquet(con,"SELECT * FROM cm_output ORDER BY security_id",payload,2048)
                    receipt={"snapshot_sha256":R.digest(frozen),"payload":R.identity(payload),"measurement":measurement}
                    C.write_json_atomic(stamp,receipt);new+=1
                receipts[key]=receipt
            R.recheck(root,snapshot)
            evidence=coverage(receipts);criteria=gates(evidence)
            C.write_json_atomic(generation/"validation.json",{"coverage":evidence,"gates":criteria})
            files={f"month={d:%Y-%m}/char_monthly.parquet":receipts[d.isoformat()]["payload"] for d in dates}
            manifest={"stage":STAGE,"schema":SCHEMA,"status":"complete","readiness":"NOT_PRODUCTION_READY","source_quality_certified":False,
                      "files":files,"months":frozen["months"],"snapshot":R.identity(generation/"snapshot.json"),"validation":R.identity(generation/"validation.json"),
                      "registry_sha256":FREEZE_SHA256,"gate_status":{n:"PASS" if r["pass"] else "FAIL" for n,r in criteria.items()},
                      "input_manifests_sha256":{name:row["sha256"] for name,row in snapshot["manifests"].items()},
                      "receipts":{f"month={d:%Y-%m}/receipt.json":R.identity(generation/f"month={d:%Y-%m}"/"receipt.json") for d in dates}}
            C.write_json_atomic(generation/"manifest.json",manifest)
            validate_generation(root,generation,con)
            pointer={"generation":generation_id,"manifest":R.identity(generation/"manifest.json")}
            # One atomic pointer is the commit point. Prior immutable generations
            # remain intact; failed candidates never become consumer-visible.
            C.write_json_atomic(stage/"current.json",pointer)
            return {"status":"complete","generation":str(generation),"months":len(dates),"gate_status":manifest["gate_status"]}
        finally:con.close()


def validate_generation(root,generation,con):
    manifest_pin=R.identity(generation/"manifest.json");manifest=C.read_json(generation/"manifest.json")
    if any(manifest.get(k)!=v for k,v in {"stage":STAGE,"schema":SCHEMA,"status":"complete","registry_sha256":FREEZE_SHA256,"source_quality_certified":False,"readiness":"NOT_PRODUCTION_READY"}.items()):raise RuntimeError("unsupported monthly release")
    frozen=C.read_json(generation/"snapshot.json")
    snapshot=N.admit(root,frozen["bindings"],con)
    if snapshot!=frozen["source"] or frozen["registry_sha256"]!=FREEZE_SHA256:raise RuntimeError("monthly source snapshot changed")
    dates=N.months(con,dt.date.fromisoformat(frozen["start"]),dt.date.fromisoformat(frozen["end"]))
    if manifest["months"]!=[d.isoformat() for d in dates] or frozen["months"]!=manifest["months"]:raise RuntimeError("monthly date inventory differs")
    expected={f"month={d:%Y-%m}/char_monthly.parquet" for d in dates}
    if set(manifest["files"])!=expected or C.output_hashes(generation)!=manifest["files"]:raise RuntimeError("monthly file inventory differs")
    if set(manifest["receipts"])!={f"month={d:%Y-%m}/receipt.json" for d in dates}:raise RuntimeError("monthly receipt inventory differs")
    receipts={}
    for d in dates:
        unit=generation/f"month={d:%Y-%m}";payload=unit/"char_monthly.parquet";stamp=unit/"receipt.json"
        if R.identity(stamp)!=manifest["receipts"][stamp.relative_to(generation).as_posix()]:raise RuntimeError("monthly receipt changed")
        row=C.read_json(stamp)
        if row["snapshot_sha256"]!=R.digest(frozen) or row["payload"]!=R.identity(payload):raise RuntimeError("unqualified resumed unit")
        con.execute(f"CREATE OR REPLACE TEMP VIEW cm_check AS SELECT * FROM {N.sql([payload])}")
        if validate_unit(con,"cm_check",d)!=row["measurement"]:raise RuntimeError("monthly validation evidence differs")
        receipts[d.isoformat()]=row
    evidence={"coverage":coverage(receipts),"gates":gates(coverage(receipts))}
    if C.read_json(generation/"validation.json")!=evidence or manifest["gate_status"]!={n:"PASS" if r["pass"] else "FAIL" for n,r in evidence["gates"].items()}:raise RuntimeError("monthly gates differ")
    for field in ("snapshot","validation"):
        if R.identity(generation/f"{field}.json")!=manifest[field]:raise RuntimeError("monthly manifest metadata pin differs")
    R.recheck(root,snapshot)
    if R.identity(generation/"manifest.json")!=manifest_pin:raise RuntimeError("monthly generation changed during admission")
    return manifest


def resolve_current(root):
    stage=root/STAGE;pointer=C.read_json(stage/"current.json")
    generation=R.safe_path(stage/"generations",pointer["generation"])
    if R.identity(generation/"manifest.json")!=pointer["manifest"]:raise RuntimeError("monthly pointer pin differs")
    return generation


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command",choices=("registry","contract","build","validate","replicate","compare-ratios"))
    parser.add_argument("--root",type=Path)
    parser.add_argument("--bindings",type=Path)
    parser.add_argument("--generation")
    parser.add_argument("--output",type=Path)
    parser.add_argument("--start",type=dt.date.fromisoformat,default=dt.date(2020,1,1))
    parser.add_argument("--end",type=dt.date.fromisoformat,default=dt.datetime.now(dt.UTC).date().replace(day=1)-dt.timedelta(days=1))
    parser.add_argument("--max-new-months",type=int)
    args=parser.parse_args(argv)
    if args.command=="registry":print(json.dumps(registry_document(),indent=2))
    elif args.command=="contract":print(json.dumps({"version":"char_monthly_bindings_v1","products":list(N.PRODUCTS),"explicit_generations":["universe_release_v1","fundamentals_release_v1"],"semantic_stages":["fundamentals_v11","compustat_std_v11","ratios_v11"]},indent=2))
    else:
        root=(args.root or C.build_root()).resolve()
        if args.command=="build":
            if not args.bindings or not args.generation:parser.error("build requires --bindings and --generation")
            print(json.dumps(build(root,C.read_json(args.bindings),args.generation,args.start,args.end,args.max_new_months),indent=2))
        else:
            con=C.connect(memory="380MB",threads=1,db_file=f"char-check-{uuid.uuid4().hex}.duckdb")
            try:
                generation=R.safe_path(root/STAGE/"generations",args.generation) if args.generation else resolve_current(root)
                if args.command in ("replicate","compare-ratios"):
                    if not args.bindings or not args.output:parser.error("replicate requires --bindings and --output")
                    from .char_monthly_evidence import run,compare_ratios
                    if args.command=="compare-ratios":
                        print(json.dumps(compare_ratios(root,generation,args.bindings,args.output,con)["gate"],indent=2))
                        return 0
                    result=run(root,generation,args.bindings,args.output,con)
                    print(json.dumps(result["gates"],indent=2))
                else:print(json.dumps(validate_generation(root,generation,con),indent=2))
            finally:con.close()
    return 0


if __name__=="__main__":raise SystemExit(main())
