"""Frozen bounded cross-sectional evidence; all contributing observations pre2023.

Pure computation kernels accept already admitted rows. They are not admission
interfaces and do not establish real-data or consumer acceptance.
"""
from __future__ import annotations
import datetime as dt
import json
import math
import os
import shutil
from pathlib import Path
import numpy as np
from . import common as C
from .golden_labels import guard, HORIZONS
from .golden_sources import digest, pin
from .golden_registry import evaluation_config


def load_config(path, expected_sha256):
    path = Path(path)
    config = C.read_json(path)
    if pin(path)["sha256"] != expected_sha256 or config != evaluation_config():
        raise ValueError("frozen executable evaluation configuration changed")
    return config


def ranks(values):
    order = np.argsort(values, kind="stable")
    result = np.empty(len(values), dtype=float)
    i = 0
    while i < len(order):
        j = i+1
        while j < len(order) and values[order[j]] == values[order[i]]:
            j += 1
        result[order[i:j]] = (i+j-1)/2 + 1
        i = j
    return result


def spearman(x, y):
    if len(x) < 2:
        return None
    a, b = ranks(np.asarray(x)), ranks(np.asarray(y))
    a -= a.mean(); b -= b.mean()
    denominator = math.sqrt(float(a@a)*float(b@b))
    return float(a@b)/denominator if denominator else None


def cap_weights(caps, maximum):
    caps = np.asarray(caps, dtype=float)
    if not len(caps) or len(caps)*maximum < 1-1e-12 or np.any(~np.isfinite(caps)) or np.any(caps <= 0):
        return None
    weights = np.zeros(len(caps))
    active = np.ones(len(caps), dtype=bool)
    left = 1.0
    for _ in range(len(caps)+1):
        if not active.any():
            break
        proposed = left*caps[active]/caps[active].sum()
        bound = proposed > maximum+1e-12
        if not bound.any():
            weights[active] = proposed
            break
        indices = np.flatnonzero(active)[bound]
        weights[indices] = maximum
        active[indices] = False
        left = 1-weights.sum()
    return weights if abs(weights.sum()-1) < 1e-9 and weights.max() <= maximum+1e-9 else None


def evaluate_date(rows, feature_date, horizon, direction, config, previous_weights=None):
    if horizon not in HORIZONS or direction not in (-1, 1):
        raise ValueError("invalid primary direction/horizon")
    # Validate EVERY supplied observation before filtering, ranking or aggregation.
    for row in rows:
        guard(feature_date, row["entry_date"], row["exit_date"], row.get("observation_dates", ()))
        for date in row.get("benchmark_dates", ()) + row.get("cost_dates", ()):
            if date >= dt.date(2023, 1, 1):
                raise ValueError("benchmark/cost input crosses pre2023 seal")
        if row.get("source_at") is not None and row["source_at"] >= row["cutoff"]:
            raise ValueError("forward source clock leak")
    if len({r["security_id"] for r in rows}) != len(rows):
        raise ValueError("duplicate evaluation security")
    exclusions = {}
    usable = []
    for r in rows:
        reason = None
        if r.get("label_qualified") is not True:
            reason = "missing_or_unqualified_label"
        elif r.get("source_at") is None or r.get("source_qualified") is not True:
            reason = "missing_or_unqualified_feature"
        elif any(r.get(k) is None or not math.isfinite(r[k]) for k in ("value", "ret", "me")) or r["me"] <= 0:
            reason = "null_or_nonfinite_feature_label_cap"
        elif r.get("me_at") is None or r["me_at"].date() >= r["entry_date"]:
            reason = "cap_not_pre_entry"
        if reason:
            exclusions[reason] = exclusions.get(reason, 0)+1
        else:
            usable.append(r)
    result = {"feature_date": str(feature_date), "horizon": horizon, "names": len(usable),
              "denominator": len(rows), "exclusions": exclusions, "IC": None, "spread": None,
              "equal_weight_spread": None, "turnover": None, "cost_sensitivity": {}, "weights": {}}
    if len(usable) < config["minimum_names"]:
        result["reason"] = "minimum_names"
        return result
    result["IC"] = direction * spearman([r["value"] for r in usable], [r["ret"] for r in usable]) if spearman([r["value"] for r in usable], [r["ret"] for r in usable]) is not None else None
    nyse = [r["value"] for r in usable if r.get("nyse") is True]
    if len(nyse) < config["nyse_minimum_names"]:
        result["reason"] = "insufficient_nyse_breakpoints"
        return result
    breaks = np.quantile(nyse, [.2, .4, .6, .8], method="linear")
    groups = [[] for _ in range(5)]
    for r in usable:
        groups[int(np.searchsorted(breaks, r["value"], side="left"))].append(r)
    quintiles, equal, portfolios = [], [], []
    for group in groups:
        weights = cap_weights([r["me"] for r in group], config["weight_cap"])
        if weights is None:
            result["reason"] = "empty_or_infeasible_capped_quintile"
            return result
        quintiles.append(float(weights @ np.asarray([r["ret"] for r in group])))
        equal.append(float(np.mean([r["ret"] for r in group])))
        portfolios.append({r["security_id"]: float(w) for r, w in zip(group, weights)})
    signed = {sid: direction*w for sid, w in portfolios[4].items()}
    signed.update({sid: -direction*w for sid, w in portfolios[0].items()})
    previous = previous_weights or {}
    turnover = sum(abs(signed.get(sid, 0)-previous.get(sid, 0)) for sid in signed.keys() | previous.keys())
    result.update(quintiles=quintiles, equal_weight_quintiles=equal, spread=direction*(quintiles[4]-quintiles[0]),
                  equal_weight_spread=direction*(equal[4]-equal[0]), weights=signed, turnover=turnover,
                  cost_sensitivity={str(b): direction*(quintiles[4]-quintiles[0])-turnover*b/10000 for b in config["cost_bps"]},
                  liquidity={"qualified_names": sum(r.get("adv_qualified") is True for r in usable),
                             "missing_names": sum(r.get("adv_qualified") is not True for r in usable)}, reason="computed")
    return result


def hac(values, horizon, minimum_dates=126):
    # Missing dates preserve positions: covariance uses actual session lags.
    x = np.asarray([np.nan if v is None else v for v in values], dtype=float)
    ok = np.isfinite(x); count = int(ok.sum())
    if count < minimum_dates:
        return {"status": "non_testable", "reason": "minimum_dates", "dates": count, "p": 1.0}
    mean = float(x[ok].mean()); centered = np.where(ok, x-mean, 0)
    lag = max(horizon-1, 21)
    variance = float(centered@centered)/count
    for k in range(1, min(lag, len(x)-1)+1):
        variance += 2*(1-k/(lag+1))*float(centered[k:]@centered[:-k])/count
    se = math.sqrt(max(variance, 0)/count)
    if not se:
        return {"status": "non_testable", "reason": "degenerate_variance", "dates": count, "mean": mean, "p": 1.0}
    t = mean/se
    return {"status": "tested", "dates": count, "mean": mean, "standard_error": se,
            "t": t, "p": math.erfc(abs(t)/math.sqrt(2)), "confidence_interval": [mean-1.95996398454*se, mean+1.95996398454*se],
            "lag": lag, "inference": "asymptotic normal HAC/Newey-West Bartlett; original session lags retained"}


def fdr(p_values):
    """Missing tests remain in family with p=1, rather than shrinking denominator."""
    values = [1.0 if p is None else p for p in p_values]
    if any(not math.isfinite(p) or p < 0 or p > 1 for p in values):
        raise ValueError("invalid p value")
    order = sorted(range(len(values)), key=lambda i: (values[i], i))
    adjusted = [1.0]*len(values); best = 1.0
    for position in range(len(order)-1, -1, -1):
        i = order[position]
        best = min(best, values[i]*len(values)/(position+1))
        adjusted[i] = best
    return adjusted


def summarize(outcomes, config, family="primary"):
    candidates = config["primary"] if family == "primary" else config["exploratory"]
    tests = []
    for candidate in candidates:
        name = candidate["name"]
        for h in config["horizons"]:
            series = outcomes.get((name, h), [])
            for endpoint in config["endpoints"]:
                evidence = hac([r.get(endpoint) for r in series], h, config["minimum_dates"])
                yearly = {str(y): hac([r.get(endpoint) for r in series if str(r["feature_date"]).startswith(str(y))], h, config["minimum_dates"]) for y in (2020,2021,2022)}
                tests.append({"candidate": name, "direction": candidate["direction"], "horizon": h, "endpoint": endpoint,
                              **evidence, "yearly": yearly, "names": sum(r.get("names",0) for r in series),
                              "yearly_sign_stability": {y:(e.get("mean")>0 if e.get("mean") is not None else None) for y,e in yearly.items()},
                              "unsupported": not bool(series), "coverage": [r.get("exclusions", {}) for r in series]})
    for test, q in zip(tests, fdr([t["p"] for t in tests])):
        test["q"] = q
    gates = {}
    for h in config["horizons"]:
        passed = []
        for c in candidates:
            paired = [t for t in tests if t["candidate"] == c["name"] and t["horizon"] == h]
            if len(paired) == 2 and all(t["status"] == "tested" and t.get("mean",0)>0 and t["q"]<=config["fdr_q"] for t in paired):
                passed.append(c["name"])
        gates[str(h)] = {"status": "PASS" if passed else "FAIL", "passing_candidates": passed}
    sensitivities = {name+":"+str(h): {"turnover": [r.get("turnover") for r in series],
        "cost_sensitivity": {str(b):hac([r.get("cost_sensitivity",{}).get(str(b)) for r in series],h,config["minimum_dates"]) for b in config["cost_bps"]},
        "liquidity": [r.get("liquidity",{}) for r in series],
        "equal_weight_sensitivity":hac([r.get("equal_weight_spread") for r in series],h,config["minimum_dates"]),
        "incremental": [r.get("incremental",{"status":"non_testable","reason":"controls_source_admission_pending"}) for r in series]}
        for (name,h),series in outcomes.items()}
    return {"family": family, "tests": tests, "family_size": len(tests), "gate_status": gates,"sensitivities":sensitivities,
            "history_qualification": config["history_qualification"], "net_profitability_claim": False}


def incremental_date(rows, controls):
    """Independent control evidence; complete cases only, never replaces primary raw IC."""
    if not controls:
        return {"status": "non_testable", "reason": "controls_not_declared"}
    valid = [r for r in rows if r.get("source_qualified") is True and r.get("label_qualified") is True and all(r.get(k) is not None and math.isfinite(r[k]) for k in ["value", "ret", *controls])]
    if len(valid) < max(100, len(controls)+3):
        return {"status": "non_testable", "reason": "complete_control_cases", "names": len(valid)}
    design = np.column_stack([np.ones(len(valid)), *[np.asarray([r[k] for r in valid]) for k in controls]])
    if np.linalg.matrix_rank(design) != design.shape[1]:
        return {"status": "non_testable", "reason": "singular_controls"}
    residuals = []
    for key in ("value", "ret"):
        target = np.asarray([r[key] for r in valid]); beta = np.linalg.lstsq(design, target, rcond=None)[0]
        residuals.append(target-design@beta)
    return {"status": "computed", "names": len(valid), "controls": controls, "partial_rank_IC": spearman(*residuals)}


def record_run(output, config_path, expected_config_sha256, outcomes, *, source_dependencies, actual=False):
    config = load_config(config_path, expected_config_sha256)
    if actual:
        raise ValueError("actual return run requires independently reviewed native complete label admission; pending")
    path = Path(output)
    if path.exists():
        raise ValueError("immutable evaluation run already exists")
    report = {"schema": "atx.golden-evaluation-run/v1", "scope": "offline_synthetic",
        "config": pin(config_path), "source_dependencies": source_dependencies,
        "primary": summarize(outcomes, config), "exploratory": summarize(outcomes, config, "exploratory"),
        "date_outcomes": {name+":"+str(h):series for (name,h),series in outcomes.items()},
        "data_integrity": "SYNTHETIC_ONLY", "feature_label_coverage": "ACTUAL_PENDING", "consumer_acceptance": "PENDING"}
    C.write_json_atomic(path, report)
    return report


def evaluate_native(golden_stage,golden_generation,label_stage,label_generation,config_path,expected_config_sha256,output):
    """Bounded native orchestration with full public panel/label admission.

The current native label stage carries NULLs/exclusions until actual window
sources qualify. It produces all non-testable outcomes and conservative FDR,
never claims that a diagnostic run measured predictive evidence.
    """
    from . import golden_panel as P, golden_labels as L, golden_sources as S
    golden_stage,label_stage=Path(golden_stage),Path(label_stage)
    golden=S.member(golden_stage/"generations",golden_generation)
    labels=S.member(label_stage/"generations",label_generation)
    config=load_config(config_path,expected_config_sha256)
    output=Path(output)
    if output.exists():raise ValueError("immutable evaluation output already exists")
    con=P.connect(output.parent/"_scratch")
    try:
        P.validate_generation(golden_stage,golden,con)
        label_manifest=L.validate_generation(label_stage,labels,con)
        source_pins={str(golden/"manifest.json"):S.pin(golden/"manifest.json"),str(labels/"manifest.json"):S.pin(labels/"manifest.json"),str(Path(config_path).resolve()):pin(config_path)}
        m=C.read_json(golden/"manifest.json")
        con.execute(f"CREATE OR REPLACE TEMP VIEW evaluation_features AS SELECT * FROM {P.sql([golden/n for n in m['files'] if n.startswith('daily/')])}")
        con.execute(f"CREATE OR REPLACE TEMP VIEW evaluation_labels AS SELECT * FROM {P.sql([labels/n for n in label_manifest['files']])}")
        # Reject even an excluded or NULL-label forbidden window before joins,
        # ranking, aggregation or inference.
        if con.execute("SELECT 1 FROM evaluation_labels WHERE feature_date>=DATE '2023-01-01' OR entry_date>=DATE '2023-01-01' OR exit_date>=DATE '2023-01-01' LIMIT 1").fetchone():raise ValueError("native label boundary violates seal")
        populations={}
        summaries={}
        for universe in config["universes"]:
            populations[universe]=[]
            for year,h,reason,count,nonnull in con.execute(f"SELECT year(feature_date),horizon,reason,count(*),count(value) FROM evaluation_labels WHERE {universe} GROUP BY 1,2,3 ORDER BY 1,2,3").fetchall():
                populations[universe].append({"year":year,"horizon":h,"reason":reason,"denominator":count,"qualified_labels":nonnull})
        # Qualification is explicit: no legacy or fabricated zero label enters
        # a ranking. Existing diagnostic native labels have zero qualifying rows.
        dates=[r[0] for r in con.execute("SELECT DISTINCT feature_date FROM evaluation_labels ORDER BY 1").fetchall()]
        estimated=len(dates)*(len(config["primary"])+len(config["exploratory"]))*len(config["horizons"])*len(config["universes"])*1024
        output.parent.mkdir(parents=True,exist_ok=True)
        if estimated>1024**3 and shutil.disk_usage(output.parent).free<40*1024**3:
            raise ValueError("C: free >=40GB required before >1GB evidence landing")
        date_output=output.with_name(output.stem+".date-outcomes.jsonl")
        if date_output.exists():raise ValueError("immutable date evidence already exists")
        measured=False
        with date_output.open("x",encoding="utf-8") as stream:
          for family,candidates in (("primary",config["primary"]),("exploratory",config["exploratory"])):
            for universe in config["universes"]:
                current={"tests":[],"sensitivities":{},"family":family}
                summaries[(family,universe)]=current
                for candidate in candidates:
                    name=candidate["name"]
                    direction=candidate["direction"]
                    if candidate.get("stored_sign") in (-1,1):direction*=candidate["stored_sign"]
                    for h in config["horizons"]:
                        series=[];previous={}
                        for date in dates:
                            # Full eligible feature population survives missing labels.
                            # Crossing-boundary NULL windows remain explicit exclusions.
                            rows=con.execute(f"SELECT f.security_id,f.{P.ident(name)},l.value,f.me,f.exchcd_like=1,l.entry_date,l.exit_date,f.{P.ident(name+'_source_at')},f.available_at,f.me_source_at,f.{P.ident(name+'_qualified')},f.me_qualified,l.label_qualified,f.ret_12_1,f.ret_12_1_qualified,f.rvol_21d,f.rvol_21d_qualified FROM evaluation_features f JOIN evaluation_labels l ON l.feature_date=f.session_date AND l.security_id=f.security_id WHERE f.session_date=? AND l.horizon=? AND f.{universe}",[date,h]).fetchall()
                            data=[]
                            for sid,v,ret,me,nyse,entry,exit,clock,cutoff,me_at,feature_ok,cap_ok,label_ok,momentum,mom_ok,risk,risk_ok in rows:
                                if entry is None or exit is None:
                                    continue # retained in exact population exclusion counts above
                                data.append({"security_id":sid,"value":v,"ret":ret,"me":me,"nyse":nyse,"entry_date":entry,"exit_date":exit,"source_at":clock,"cutoff":cutoff,"me_at":me_at,"source_qualified":bool(feature_ok and cap_ok),"label_qualified":label_ok,
                                    "log_me":math.log(me) if cap_ok and me is not None and me>0 else None,
                                    "ret_12_1":momentum if mom_ok else None,"rvol_21d":risk if risk_ok else None})
                            result=evaluate_date(data,date,h,direction,config,previous)
                            result["incremental"]=incremental_date(data,config["incremental_controls"])
                            previous=result.pop("weights") # retain only one bounded portfolio
                            series.append(result)
                            measured |= result.get("IC") is not None or result.get("spread") is not None
                            stream.write(json.dumps({"family":family,"universe":universe,"candidate":name,**result},sort_keys=True,default=str)+"\n")
                        # Only this candidate/horizon series is resident. All raw
                        # date evidence is already immutable-streamed to disk.
                        local={**config,"primary":[candidate],"exploratory":[candidate],"horizons":[h]}
                        summary=summarize({(name,h):series},local,family)
                        current["tests"].extend(summary["tests"])
                        sensitivity=summary["sensitivities"][name+":"+str(h)]
                        turns=[r["turnover"] for r in series if r.get("turnover") is not None]
                        sensitivity["turnover"]={"usable_dates":len(turns),"mean":float(np.mean(turns)) if turns else None}
                        sensitivity["liquidity"]={"qualified_name_observations":sum(r.get("liquidity",{}).get("qualified_names",0) for r in series),"missing_name_observations":sum(r.get("liquidity",{}).get("missing_names",0) for r in series)}
                        sensitivity["incremental"] = hac([r.get("incremental",{}).get("partial_rank_IC") for r in series],h,config["minimum_dates"])
                        current["sensitivities"][name+":"+str(h)]=sensitivity
                finalize_tests(current,config)
          stream.flush();os.fsync(stream.fileno())
        report={"schema":"atx.golden-evaluation-run/v1","scope":"native_conditional_pre2023","config":pin(config_path),"source_dependencies":source_pins,
            "primary":summaries["primary","univ_all_common"],"exploratory":summaries["exploratory","univ_all_common"],"populations":populations,
            "robustness":{family:summaries[family,"univ_r3000p"] for family in ("primary","exploratory")},
            "data_integrity":"DIAGNOSTIC_FULL_PUBLIC_REPLAY","feature_label_coverage":"FAIL","consumer_acceptance":"PENDING",
            "statistical_evidence_measured":measured,"actual_return_source_admission":"CONDITIONAL_NATIVE" if measured else "PENDING",
            "date_outcomes":{"path":str(date_output.resolve()),"identity":pin(date_output)}}
        S.check_pins(source_pins)
        C.write_json_atomic(output,report)
        return report
    finally:con.close()


def finalize_tests(summary,config):
    """Whole frozen family FDR, including every unsupported test with p=1."""
    tests=summary["tests"]
    expected=config["primary"] if summary["family"]=="primary" else config["exploratory"]
    keys={(c["name"],h,e) for c in expected for h in config["horizons"] for e in config["endpoints"]}
    actual=[(t["candidate"],t["horizon"],t["endpoint"]) for t in tests]
    if len(actual)!=len(keys) or set(actual)!=keys:raise ValueError("incomplete/duplicate frozen inference family")
    for test,q in zip(tests,fdr([t["p"] for t in tests])):test["q"]=q
    summary["family_size"]=len(tests)
    summary["gate_status"]={}
    for h in config["horizons"]:
        passed=[]
        for c in expected:
            pair=[t for t in tests if t["candidate"]==c["name"] and t["horizon"]==h]
            if all(t["status"]=="tested" and t.get("mean",0)>0 and t["q"]<=config["fdr_q"] for t in pair):passed.append(c["name"])
        summary["gate_status"][str(h)]={"status":"PASS" if passed else "FAIL","passing_candidates":passed}
    summary["history_qualification"]=config["history_qualification"]
    summary["net_profitability_claim"]=False
