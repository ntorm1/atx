"""Frozen replication and value diagnostics; no post-2022 return aggregation."""
from __future__ import annotations
import datetime as dt
from pathlib import Path
import numpy as np
from . import common as C
from . import release_inputs as R
from . import char_monthly_kernels as K
from .char_monthly_registry import JKP153,BY_NAME,FREEZE_SHA256

BOUNDARY=dt.date(2023,1,1)


def _pearson(pairs):
    if len(pairs)<2:return None
    x,y=zip(*pairs)
    if K.std(x)<=0 or K.std(y)<=0:return None
    return float(np.corrcoef(x,y)[0,1])


def spearman(pairs):
    if len(pairs)<2:return None
    def ranks(values):return [sum(x<v for x in values)+(sum(x==v for x in values)+1)/2 for v in values]
    x,y=zip(*pairs)
    return _pearson(list(zip(ranks(x),ranks(y))))


def tercile(rows,feature,formation):
    """Rows are distinct, primary issuer observations with complete future windows.

    Population is fixed before label completeness. Any missing eligible return
    invalidates that month's portfolio, preserving its original denominator.
    """
    if formation>=BOUNDARY:raise ValueError("formation outside return-statistic window")
    if any(r["entry_date"]<=formation or r["exit_date"]>=BOUNDARY or r["entry_date"]>=BOUNDARY or r["exit_date"]<r["entry_date"] for r in rows):raise ValueError("incomplete/heldout label window before return aggregation")
    if len({r["cik"] for r in rows})!=len(rows):raise ValueError("duplicate issuer formation")
    if any(K.finite(r.get(feature)) and (r.get("certified") is not True or not K.finite(r.get("me")) or r["me"]<=0) for r in rows):return None,"invalid_or_uncertified_eligible_issuer_cap"
    nyse=[r["me"] for r in rows if r.get("nyse") is True and K.finite(r.get("me")) and r["me"]>0]
    if len(nyse)<10:return None,"insufficient_NYSE_cap_breakpoints"
    lowcap,cap=np.quantile(nyse,[.2,.8])
    pool=[r for r in rows if K.finite(r.get(feature)) and K.finite(r.get("me")) and r["me"]>0 and r.get("certified") is True]
    breakpoints=[r[feature] for r in pool if r["me"]>lowcap]
    if len(breakpoints)<10:return None,"insufficient_nonmicro_breakpoints"
    lo,hi=np.quantile(breakpoints,[1/3,2/3])
    legs=([r for r in pool if r[feature]<=lo],[r for r in pool if r[feature]>hi])
    if any(len(leg)<5 for leg in legs):return None,"insufficient_leg_names"
    if any(not K.finite(r.get("ret")) or r["ret"]< -1 or r.get("return_certified") is not True for leg in legs for r in leg):return None,"missing_or_uncertified_eligible_return"
    returns=[]
    for leg in legs:
        weights=[min(r["me"],cap) for r in leg]
        returns.append(sum(w*r["ret"] for w,r in zip(weights,leg))/sum(weights))
    return (returns[1]-returns[0])*BY_NAME[feature].economic_sign,None


def replication(panel_months,labels,jkp,osap):
    """All predeclared outcomes; factor matching is a frozen full153 identity."""
    # Boundary checked before grouping/ranking/aggregation or reading returns.
    if any(r["formation"]>=BOUNDARY or r["exit_date"]>=BOUNDARY or r["entry_date"]>=BOUNDARY for r in labels):raise ValueError("heldout labels supplied")
    if any(r["month"]>=BOUNDARY for r in jkp+osap):raise ValueError("heldout benchmark supplied")
    by_label={(r["formation"],r["cik"]):r for r in labels}
    if len(by_label)!=len(labels):raise ValueError("ambiguous issuer label")
    own={};exclusions={}
    for formation,rows in panel_months.items():
        if formation>=BOUNDARY:raise ValueError("heldout formation supplied")
        issuers={}
        for row in rows:
            if row.get("univ_all_common") is True and row.get("primary_security") is True:
                if row["cik"] in issuers:raise ValueError("ambiguous primary security")
                label=by_label.get((formation,row["cik"]))
                if label is None:
                    label={"entry_date":formation+dt.timedelta(days=1),"exit_date":formation+dt.timedelta(days=1),"ret":None,"return_certified":False}
                issuers[row["cik"]]={**row,**label}
        for feature in JKP153:
            value,reason=tercile(list(issuers.values()),feature,formation)
            own[formation,feature]=value
            if reason:exclusions[f"{formation}/{feature}"]=reason
    def bench(rows):
        out={}
        for r in rows:
            key=(r["formation"],r["feature"])
            if key in out:raise ValueError("ambiguous benchmark factor month")
            if r.get("unit")!="decimal" or r.get("weighting")!="vw_cap" or r.get("country")!="usa":raise ValueError("benchmark convention mismatch")
            out[key]=r["ret"]
        return out
    jb,ob=bench(jkp),bench(osap)
    outcomes={}
    for feature in JKP153:
        pairs=[(v,jb[k]) for k,v in own.items() if k[1]==feature and K.finite(v) and K.finite(jb.get(k))]
        other=[(v,ob[k]) for k,v in own.items() if k[1]==feature and K.finite(v) and K.finite(ob.get(k))]
        means=(float(np.mean([x for x,y in pairs])),float(np.mean([y for x,y in pairs]))) if pairs else (None,None)
        outcomes[feature]={"months":len(pairs),"jkp_rho":_pearson(pairs) if len(pairs)>=60 else None,
                           "mean_sign_agrees":means[0]*means[1]>0 if all(K.finite(x) for x in means) else False,
                           "osap_months":len(other),"osap_rho":_pearson(other) if len(other)>=60 else None,
                           "status":"testable" if len(pairs)>=60 else "insufficient_overlap; original denominator retained"}
    good=sum(K.finite(r["jkp_rho"]) and r["jkp_rho"]>=.7 for r in outcomes.values())
    signs=sum(r["mean_sign_agrees"] and r["months"]>=60 for r in outcomes.values())
    osvalues=[r["osap_rho"] for r in outcomes.values() if K.finite(r["osap_rho"])]
    median=float(np.median(osvalues)) if osvalues else None
    return {"registry_sha256":FREEZE_SHA256,"outcomes":outcomes,"exclusions":exclusions,
            "gates":{"jkp_matched_rho_share":{"pass":good/153>=.7,"measured":good/153,"numerator":good,"denominator":153},
                     "jkp_mean_sign_share":{"pass":signs/153>=.9,"measured":signs/153,"numerator":signs,"denominator":153},
                     "osap_median_rho":{"pass":len(osvalues)==153 and median is not None and median>=.6,"measured":median,"matched":len(osvalues),"denominator":153}}}


def pinned_records(con,root,binding,boundary_fields=()):
    from .char_monthly_sources import sql,records
    if not all(binding.get(k) for k in ("source","clock","admission","files")):raise ValueError("independent evidence binding required")
    paths=[]
    for relative,pin in binding["files"].items():
        path=R.safe_path(root,relative)
        if R.identity(path)!=pin:raise RuntimeError("evidence source pin differs")
        paths.append(path)
    relation=sql(paths)
    for field in boundary_fields:
        if field not in ("entry_date","exit_date","formation","month"):raise ValueError("unsupported boundary column")
        if con.execute(f'SELECT count(*) FROM {relation} WHERE "{field}" IS NULL OR "{field}">=?',[BOUNDARY]).fetchone()[0]:raise ValueError("evidence source exceeds complete pre2023 window")
    return records(con,"SELECT * FROM "+relation)


def run(root,generation,bindings_path,output,con):
    """Immutable feature release plus explicitly bounded independent evidence."""
    from .char_monthly import validate_generation
    from .char_monthly_sources import sql,records
    manifest=validate_generation(root,generation,con)
    binding=C.read_json(bindings_path)
    if binding.get("registry_sha256")!=FREEZE_SHA256 or set(binding.get("products",{}))!={"labels","jkp","osap","formation_quality"}:raise ValueError("frozen evidence product contract required")
    pins={name:{p:R.identity(R.safe_path(root,p)) for p in row["files"]} for name,row in binding["products"].items()}
    bounds={"labels":("formation","entry_date","exit_date"),"jkp":("month",),"osap":("month",),"formation_quality":("formation",)}
    products={name:pinned_records(con,root,row,bounds[name]) for name,row in binding["products"].items()}
    labels=products["labels"]
    # These admitted evidence products must already be strictly bounded. No
    # reader filter can turn an unqualified complete-window source into evidence.
    if any(r["exit_date"]>=BOUNDARY or r["entry_date"]>=BOUNDARY for r in labels):raise ValueError("evidence holding-period boundary")
    quality={(r["formation"],r["security_id"]):r for r in products["formation_quality"]}
    if len(quality)!=len(products["formation_quality"]):raise ValueError("duplicate formation quality")
    panel={}
    for relative in manifest["files"]:
        date=dt.date.fromisoformat(relative.split("/")[0].split("=")[1]+"-01")
        if date>=BOUNDARY:continue
        rows=records(con,"SELECT * FROM "+sql([generation/relative]))
        for row in rows:
            audit=quality.get((row["session_date"],row["security_id"]),{})
            if audit and (audit.get("available_at") is None or audit["available_at"]>=dt.datetime.combine(row["session_date"],dt.time(22),dt.UTC)):
                raise ValueError("formation audit clock leaks")
            row.update(audit)
        if rows:panel[rows[0]["session_date"]]=rows
    result=replication(panel,labels,products["jkp"],products["osap"])
    result["input_files"]=pins;result["feature_manifest"]=R.identity(generation/"manifest.json")
    for name,row in binding["products"].items():
        for relative,pin in row["files"].items():
            if R.identity(R.safe_path(root,relative))!=pin:raise RuntimeError("evidence source changed")
    C.write_json_atomic(output,result)
    return result


def compare_ratios(root,generation,bindings_path,output,con):
    """Full same-date member value gate plus keyed disagreement causes.

    Annual characteristic vs quarterly/TTM ratio basis differences are reported,
    never filtered into a smaller binding gate cohort. NI comparison uses NIX
    (including extraordinary items), preserving final WFRS NI numerator.
    """
    from .char_monthly import validate_generation
    from .char_monthly_sources import sql,records
    manifest=validate_generation(root,generation,con)
    doc=C.read_json(bindings_path)
    binding=doc.get("ratios")
    if doc.get("registry_sha256")!=FREEZE_SHA256 or not binding:raise ValueError("frozen final ratio comparison binding required")
    if any(Path(p).parts[0]!="ratios_v11" for p in binding["files"]):raise ValueError("explicit final ratios_v11 source required")
    pins={p:R.identity(R.safe_path(root,p)) for p in binding["files"]}
    if pins!=binding["files"]:raise RuntimeError("ratio source pin differs")
    paths=[R.safe_path(root,p) for p in binding["files"]]
    con.execute(f"CREATE OR REPLACE TEMP VIEW cm_ratios_compare AS SELECT * FROM {sql(paths)}")
    if con.execute("SELECT 1 FROM cm_ratios_compare GROUP BY session_date,security_id HAVING count(*)<>1 LIMIT 1").fetchone():raise ValueError("duplicate ratio comparison keys")
    mapping={"bm":("be_me","bm"),"ep":("nix_me","pe_issuer_ni"),"sp":("sale_me","ps"),"gpa":("gp_at","gprof")}
    dates={};diagnostic_files={};disagreement_count=0
    for relative in manifest["files"]:
        y=int(relative.split("/")[0].split("=")[1][:4])
        if y not in range(2020,2026):continue
        rows=records(con,"SELECT * FROM "+sql([generation/relative])+" WHERE member_equity")
        if not rows:continue
        date=rows[0]["session_date"]
        disagreements=[]
        reference={r["security_id"]:r for r in records(con,"SELECT * FROM cm_ratios_compare WHERE session_date=?",[date])}
        stats={}
        for name,(feature,ratio) in mapping.items():
            pairs=[]
            for row in rows:
                r=reference.get(row["security_id"],{})
                x=row.get(feature);v=r.get(ratio)
                y=1/v if name in ("ep","sp") and K.finite(v) and v>0 else v if name in ("bm","gpa") else None
                cutoff=dt.datetime.combine(date,dt.time(22),dt.UTC)
                valid=(r.get("cik")==row["cik"] and r.get("fund_available_at") is not None and r["fund_available_at"]<cutoff)
                if valid and K.finite(x) and K.finite(y):pairs.append((x,y))
                if not valid or not K.finite(x) or not K.finite(y) or abs(x-y)>1e-10*max(1,abs(y)):
                    causes=[]
                    if not r:causes.append("missing_final_ratio_key")
                    if not valid:causes.append("issuer_or_information_clock_mismatch")
                    if row.get("accounting_datadate")!=r.get("datadate"):causes.append("fiscal_period_or_annual_TTM_basis")
                    if row.get("accounting_currency")!=r.get("curcd"):causes.append("currency")
                    if row.get("issuer_me")!=r.get("me"):causes.append("issuer_market_cap_basis")
                    if not K.finite(x):causes.append("missing_characteristic_primitive_or_history")
                    if not K.finite(y):causes.append("missing_ratio_primitive_or_history")
                    if not causes:causes.append("numerator_or_book_definition")
                    disagreements.append({"session_date":str(date),"security_id":row["security_id"],"cik":row["cik"],"formula":name,"characteristic":x,"ratio":y,"causes":causes})
            stats[name]={"rho":spearman(pairs) if len(pairs)>=10 else None,"paired":len(pairs),"eligible_denominator":len(rows)}
        dates[str(date)]=stats
        detail=output.parent/(output.stem+"-keyed")/f"month={date:%Y-%m}.json"
        C.write_json_atomic(detail,disagreements)
        diagnostic_files[detail.relative_to(output.parent).as_posix()]=R.identity(detail)
        disagreement_count+=len(disagreements)
    values=[r["rho"] for stats in dates.values() for r in stats.values()]
    full=len(dates)==72 and len(values)==288 and all(K.finite(v) for v in values)
    measured=min((v for v in values if K.finite(v)),default=None)
    result={"registry_sha256":FREEZE_SHA256,"dates":dates,"disagreement_count":disagreement_count,"disagreement_files":diagnostic_files,
            "gate":{"pass":full and measured>=.99,"measured":measured,"required":.99,"expected_dates":72,"expected_formula_dates":288},
            "input_files":pins,"feature_manifest":R.identity(generation/"manifest.json")}
    for p,pin in pins.items():
        if R.identity(R.safe_path(root,p))!=pin:raise RuntimeError("ratio comparison source changed")
    C.write_json_atomic(output,result)
    return result
