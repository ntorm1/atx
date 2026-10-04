"""Frozen replication and value diagnostics; no post-2022 return aggregation."""
from __future__ import annotations
import datetime as dt
import json
from pathlib import Path
import numpy as np
from . import common as C
from . import release_inputs as R
from . import char_monthly_kernels as K
from .char_monthly_registry import JKP153,BY_NAME,FREEZE_SHA256

BOUNDARY=dt.date(2023,1,1)


def qualify_formation(row,audit):
    allowed={"formation","security_id","cik","available_at","source_available_at","me","nyse","primary_security","certified"}
    if not audit:return {**row,"certified":False,"primary_security":False}
    if set(audit)-allowed:raise ValueError("protected or unknown formation audit columns")
    if (audit.get("formation"),audit.get("security_id"),audit.get("cik"))!=(row["session_date"],row["security_id"],row["cik"]):
        raise ValueError("formation audit issuer/key mismatch")
    cutoff=dt.datetime.combine(row["session_date"],dt.time(22),dt.UTC)
    if any(audit.get(k) is None or audit[k]>=cutoff for k in ("available_at","source_available_at")):
        raise ValueError("formation audit clock leaks or missing")
    if audit.get("me")!=row.get("issuer_me"):raise ValueError("formation audit issuer cap differs")
    return {**row,**{k:audit.get(k) for k in ("me","nyse","primary_security","certified")}}


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
    if any(r.get("entry_date") is None or r.get("exit_date") is None for r in rows):return None,"missing_eligible_complete_label_window"
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


def requested_axes(calendar):
    axes={}
    for n in range(120):
        y,m=divmod(2013*12+n,12);holding=(y,m+1)
        py,pm=divmod(y*12+m-1,12)
        candidates=[d for d in calendar if (d.year,d.month)==(py,pm+1)]
        axes[holding]=max(candidates) if candidates else None
    return axes


def replication(panel_months,labels,jkp,osap,osap_mapping=None,calendar=()):
    """All predeclared outcomes; factor matching is a frozen full153 identity."""
    # Boundary checked before grouping/ranking/aggregation or reading returns.
    if any(r["formation"]>=BOUNDARY or r["exit_date"]>=BOUNDARY or r["entry_date"]>=BOUNDARY for r in labels):raise ValueError("heldout labels supplied")
    if any(r["month"]>=BOUNDARY for r in jkp+osap):raise ValueError("heldout benchmark supplied")
    if not osap_mapping:raise ValueError("nonempty independently admitted OSAP overlap required")
    axes=requested_axes(calendar)
    formations={d:holding for holding,d in axes.items() if d is not None}
    if any(d not in formations for d in panel_months):raise ValueError("formation outside exact requested calendar axis")
    for r in labels:
        if set(r)-{"formation","cik","entry_date","exit_date","ret","return_certified"}:raise ValueError("protected or unknown label columns")
        holding=formations.get(r["formation"])
        if holding is None or any((r[k].year,r[k].month)!=holding for k in ("entry_date","exit_date")) or r["entry_date"]<=r["formation"] or r["exit_date"]<r["entry_date"]:
            raise ValueError("label outside exact holding/formation axes")
    by_label={(r["formation"],r["cik"]):r for r in labels}
    if len(by_label)!=len(labels):raise ValueError("ambiguous issuer label")
    own={};exclusions={}
    for formation in formations:
        rows=panel_months.get(formation,[])
        if formation>=BOUNDARY:raise ValueError("heldout formation supplied")
        issuers={}
        for row in rows:
            if row.get("univ_all_common") is True and row.get("primary_security") is True:
                if row["cik"] in issuers:raise ValueError("ambiguous primary security")
                label=by_label.get((formation,row["cik"]))
                if label is None:
                    label={"entry_date":None,"exit_date":None,"ret":None,"return_certified":False}
                issuers[row["cik"]]={**row,**label}
        for feature in JKP153:
            value,reason=tercile(list(issuers.values()),feature,formation)
            own[formation,feature]=value
            if reason:exclusions[f"{formation}/{feature}"]=reason
    def bench(rows):
        out={}
        for r in rows:
            if r["formation"] not in formations or (r["month"].year,r["month"].month)!=formations[r["formation"]]:raise ValueError("benchmark outside holding/formation axes")
            key=(r["formation"],r["feature"])
            if key in out:raise ValueError("ambiguous benchmark factor month")
            if r.get("unit")!="decimal" or r.get("weighting")!="vw_cap" or r.get("country")!="usa":raise ValueError("benchmark convention mismatch")
            out[key]=r["ret"]
        return out
    jb,ob=bench(jkp),bench(osap)
    if any(feature not in JKP153 for _,feature in jb) or any(feature not in {m["osap_name"] for m in osap_mapping.values()} for _,feature in ob):raise ValueError("benchmark outside frozen name inventory")
    outcomes={}
    for feature in JKP153:
        pairs=[(v,jb[k]) for k,v in own.items() if k[1]==feature and K.finite(v) and K.finite(jb.get(k))]
        match=osap_mapping.get(feature)
        other=[(v,ob[(k[0],match["osap_name"])]*match["sign"]) for k,v in own.items() if match and k[1]==feature and K.finite(v) and K.finite(ob.get((k[0],match["osap_name"])))]
        means=(float(np.mean([x for x,y in pairs])),float(np.mean([y for x,y in pairs]))) if pairs else (None,None)
        outcomes[feature]={"requested_months":120,"missing_calendar_months":sum(d is None for d in axes.values()),"months":len(pairs),"jkp_rho":_pearson(pairs) if len(pairs)>=60 else None,
                           "mean_sign_agrees":means[0]*means[1]>0 if all(K.finite(x) for x in means) else False,
                           "osap_months":len(other),"osap_rho":_pearson(other) if len(other)>=60 else None,
                           "status":"testable" if len(pairs)>=60 else "insufficient_overlap; original denominator retained"}
    good=sum(K.finite(r["jkp_rho"]) and r["jkp_rho"]>=.7 for r in outcomes.values())
    signs=sum(r["mean_sign_agrees"] and r["months"]>=60 for r in outcomes.values())
    osvalues=[outcomes[n]["osap_rho"] for n in osap_mapping if K.finite(outcomes[n]["osap_rho"])]
    median=float(np.median(osvalues)) if osvalues else None
    return {"registry_sha256":FREEZE_SHA256,"outcomes":outcomes,"exclusions":exclusions,
            "requested_slots":[{"holding_month":f"{y:04d}-{m:02d}","formation":str(d) if d else None,"status":"calendar_admitted" if d else "missing_calendar_FAIL"} for (y,m),d in axes.items()],
            "gates":{"jkp_matched_rho_share":{"pass":good/153>=.7,"measured":good/153,"numerator":good,"denominator":153},
                     "jkp_mean_sign_share":{"pass":signs/153>=.9,"measured":signs/153,"numerator":signs,"denominator":153},
                     "osap_median_rho":{"pass":len(osvalues)==len(osap_mapping) and median is not None and median>=.6,"measured":median,"matched":len(osvalues),"denominator":len(osap_mapping)}}}


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
    mapping=admit_osap_mapping(root,binding.get("osap_mapping"))
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
        if (date.year,date.month)<(2012,12) or (date.year,date.month)>(2022,11):continue
        rows=records(con,"SELECT * FROM "+sql([generation/relative]))
        for i,row in enumerate(rows):
            audit=quality.get((row["session_date"],row["security_id"]),{})
            rows[i]=qualify_formation(row,audit)
        if rows:panel[rows[0]["session_date"]]=rows
    calendar=[r[0] for r in con.execute("SELECT session_date FROM cm_calendar").fetchall()]
    result=replication(panel,labels,products["jkp"],products["osap"],mapping,calendar)
    result["osap_mapping_admission"]=binding["osap_mapping"]
    result["osap_mapping_inventory"]=reviewed_metadata(root,binding["osap_mapping"],"char_monthly_osap_mapping_v1")["entries"]
    result["input_files"]=pins;result["feature_manifest"]=R.identity(generation/"manifest.json")
    for name,row in binding["products"].items():
        for relative,pin in row["files"].items():
            if R.identity(R.safe_path(root,relative))!=pin:raise RuntimeError("evidence source changed")
    if admit_osap_mapping(root,binding["osap_mapping"])!=mapping:raise RuntimeError("mapping changed during evaluation")
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
    snapshot=C.read_json(generation/"snapshot.json")["source"]
    final_pin=snapshot["manifests"].get("ratios_v11/manifest.json")
    if final_pin is None or binding.get("manifest")!=final_pin or R.identity(root/"ratios_v11/manifest.json")!=final_pin:
        raise ValueError("comparator differs from independently admitted final ratio manifest")
    final=C.read_json(root/"ratios_v11/manifest.json")
    expected={"ratios_v11/"+p:{k:pin[k] for k in ("sha256","bytes")} for p,pin in final["files"].items() if p.startswith("monthly/") and p.endswith("ratios_monthly.parquet")}
    if not expected or binding.get("files")!=expected:raise ValueError("exact admitted final monthly ratio inventory required")
    basis=reviewed_metadata(root,doc.get("ratio_basis"),"char_monthly_ratio_basis_v1")
    basis_rows=pinned_records(con,root,basis["product"])
    basis_index={(r["session_date"],r["security_id"],r["formula"]):r for r in basis_rows}
    if len(basis_index)!=len(basis_rows):raise ValueError("ambiguous independent ratio basis")
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
                proof=basis_index.get((date,row["security_id"],name),{})
                causes=ratio_basis_reasons(row,r,proof,name,date)
                valid=not causes
                if valid and K.finite(x) and K.finite(y):pairs.append((x,y))
                if not valid or not K.finite(x) or not K.finite(y) or abs(x-y)>1e-10*max(1,abs(y)):
                    if not r:causes.append("missing_final_ratio_key")
                    if not valid:causes.append("issuer_or_information_clock_mismatch")
                    if row.get("accounting_datadate")!=r.get("datadate"):causes.append("fiscal_period_or_annual_TTM_basis")
                    if row.get("accounting_currency")!=r.get("curcd"):causes.append("currency")
                    if row.get("issuer_me")!=r.get("me"):causes.append("issuer_market_cap_basis")
                    if not K.finite(x):causes.append("missing_characteristic_primitive_or_history")
                    if not K.finite(y):causes.append("missing_ratio_primitive_or_history")
                    if not causes:causes.append("numerator_or_book_definition")
                    disagreements.append({"session_date":str(date),"security_id":row["security_id"],"cik":row["cik"],"formula":name,"characteristic":x,"ratio":y,"causes":causes})
            stats[name]={"rho":spearman(pairs) if len(pairs)>=10 else None,"paired":len(pairs),"eligible_denominator":len(rows),"complete_compatible_cohort":len(pairs)==len(rows)}
        dates[str(date)]=stats
        detail=output.parent/(output.stem+"-keyed")/f"month={date:%Y-%m}.json"
        C.write_json_atomic(detail,disagreements)
        diagnostic_files[detail.relative_to(output.parent).as_posix()]=R.identity(detail)
        disagreement_count+=len(disagreements)
    values=[r["rho"] for stats in dates.values() for r in stats.values()]
    full=len(dates)==72 and len(values)==288 and all(K.finite(v) for v in values) and all(r["complete_compatible_cohort"] for stats in dates.values() for r in stats.values())
    measured=min((v for v in values if K.finite(v)),default=None)
    result={"registry_sha256":FREEZE_SHA256,"dates":dates,"disagreement_count":disagreement_count,"disagreement_files":diagnostic_files,
            "gate":{"pass":full and measured>=.99,"measured":measured,"required":.99,"expected_dates":72,"expected_formula_dates":288},
            "input_files":pins,"feature_manifest":R.identity(generation/"manifest.json")}
    for p,pin in pins.items():
        if R.identity(R.safe_path(root,p))!=pin:raise RuntimeError("ratio comparison source changed")
    if R.identity(root/"ratios_v11/manifest.json")!=final_pin:raise RuntimeError("final ratio manifest changed")
    reviewed_metadata(root,doc["ratio_basis"],"char_monthly_ratio_basis_v1")
    pinned_records(con,root,basis["product"])
    C.write_json_atomic(output,result)
    return result


def reviewed_metadata(root,binding,version):
    """Caller SHA is an independently reviewed contract, never a producer flag."""
    if not isinstance(binding,dict) or set(binding)!={"path","identity","expected_sha256"}:raise ValueError("independently reviewed metadata contract required")
    path=R.safe_path(root,binding["path"])
    if binding["identity"].get("sha256")!=binding["expected_sha256"] or R.identity(path)!=binding["identity"]:raise ValueError("reviewed metadata identity mismatch")
    doc=read_metadata(path)
    if doc.get("version")!=version or doc.get("registry_sha256")!=FREEZE_SHA256:raise ValueError("reviewed metadata version/registry mismatch")
    return doc


def read_metadata(path):
    def unique(pairs):
        result={}
        for name,value in pairs:
            if name in result:raise ValueError("duplicate reviewed metadata key: "+name)
            result[name]=value
        return result
    return json.loads(path.read_text(encoding="utf-8"),object_pairs_hook=unique)


def admit_osap_mapping(root,binding):
    contract=reviewed_metadata(root,binding,"char_monthly_osap_mapping_v1")
    entries=contract.get("entries",{})
    if set(entries)!=set(JKP153):raise ValueError("complete original153 mapped-or-excluded inventory required")
    metadata=contract.get("authoritative_metadata",{})
    if not metadata:raise ValueError("pinned authoritative mapping metadata required")
    for path,pin in metadata.items():
        if R.identity(R.safe_path(root,path))!=pin:raise ValueError("authoritative mapping metadata changed")
    record=contract.get("admission_record",{})
    if set(record)!={"path","identity","expected_sha256"}:raise ValueError("immutable pre-return mapping admission required")
    path=R.safe_path(root,record["path"])
    if record["expected_sha256"]!=record["identity"].get("sha256") or R.identity(path)!=record["identity"]:raise ValueError("mapping admission pin mismatch")
    admission=read_metadata(path)
    if admission.get("registry_sha256")!=FREEZE_SHA256 or admission.get("mapping_sha256")!=R.digest(entries) or admission.get("phase")!="frozen_before_any_return_run":raise ValueError("mapping not independently frozen before returns")
    mapped={}
    for name,item in entries.items():
        if item.get("status")=="excluded":
            if set(item)!={"status","reason","metadata_id"} or not item["reason"] or item["metadata_id"] not in metadata:raise ValueError("unsupported mapping exclusion")
            continue
        if set(item)!={"status","osap_name","sign","unit","weighting","country","metadata_id"} or item["status"]!="mapped" or not isinstance(item["osap_name"],str) or not item["osap_name"] or type(item["sign"]) is not int or item["sign"] not in (-1,1) or (item["unit"],item["weighting"],item["country"])!=("decimal","vw_cap","usa") or item["metadata_id"] not in metadata:
            raise ValueError("unsupported mapping convention or identity")
        mapped[name]=item
    if not mapped or len({item["osap_name"] for item in mapped.values()})!=len(mapped):raise ValueError("empty or ambiguous OSAP overlap")
    return mapped


def ratio_basis_reasons(row,reference,proof,name,date):
    cutoff=dt.datetime.combine(date,dt.time(22),dt.UTC)
    causes=[]
    if not proof: return ["missing_independent_economic_basis"]
    if any(proof.get(k) is None or proof[k]>=cutoff for k in ("available_at","source_available_at","fund_available_at","cap_available_at")):
        causes.append("independent_basis_clock_missing_or_late")
    if reference.get("fund_available_at") is None or reference["fund_available_at"]>=cutoff or row.get("accounting_available_at") is None or row["accounting_available_at"]>=cutoff:
        causes.append("fundamental_information_clock_missing_or_late")
    if reference.get("cik")!=row["cik"] or proof.get("cik")!=row["cik"]:causes.append("issuer_mismatch")
    if (row.get("accounting_datadate")!=reference.get("datadate") or proof.get("datadate")!=row.get("accounting_datadate") or proof.get("flow_basis")!="genuine_annual" or proof.get("balance_basis")!="same_annual_snapshot"):
        causes.append("fiscal_period_or_annual_TTM_basis")
    if row.get("accounting_currency")!="USD" or reference.get("curcd")!="USD" or proof.get("currency")!="USD":causes.append("currency")
    if not K.finite(row.get("issuer_me")) or row.get("issuer_me")!=reference.get("me") or proof.get("issuer_me")!=row.get("issuer_me") or proof.get("cap_basis")!="all_eligible_issuer_classes":causes.append("issuer_market_cap_basis")
    if proof.get("fund_available_at")!=row.get("accounting_available_at") or reference.get("fund_available_at")!=row.get("accounting_available_at"):causes.append("fundamental_vintage_basis")
    expected={"bm":"seq+txditc-pstk","ep":"ni_including_extraordinary","sp":"annual_sale","gpa":"annual_sale_minus_cogs_over_annual_at"}
    if proof.get("definition")!=expected[name]:causes.append("numerator_or_book_definition")
    return causes
