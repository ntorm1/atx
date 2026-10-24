"""Convert an observation or audit a replay; no numerical result is promoted."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import struct

ROUTES=["scalar_price","prepared_geometry_price","avx2_price","scalar_full_proof"]
INTS={"batch","index","side","n_collocation","n_quadrature","n_quad_price",
      "max_newton_iter","profile"}
BOOLS={"accepted","opts_explicit"}
FLOATS={"S","K","T","r","q","target","budget","tol","sigma","opts_tol"}
CLASSES={"continuation","exercise_plateau","european","expiry_limit","zero_vol_limit",
         "unsupported_regime"}


def require(condition,message):
    if not condition:raise ValueError(message)


def number(x):return type(x) in (int,float) and math.isfinite(x)


def model_key(r):
    return tuple(struct.pack("<d",r[k]) for k in ("T","r","q","opts_tol"))+tuple(
        r[k] for k in ("opts_explicit","n_collocation","n_quadrature","n_quad_price",
                        "max_newton_iter","profile"))


def requests(source):
    require(source.get("schema")=="atx.cold-seed.requests.v1","input schema")
    rows=source.get("requests")
    require(type(rows) is list and len(rows)>0,"empty input")
    previous=(-1,-1)
    for r in rows:
        require(set(r)==INTS|BOOLS|FLOATS,"input columns")
        require(all(type(r[k]) is int for k in INTS),"integer field")
        require(all(type(r[k]) is bool for k in BOOLS),"boolean field")
        require(all(number(r[k]) for k in FLOATS),"numeric field")
        key=(r["batch"],r["index"])
        require(key>previous and min(key)>=0,"identity order")
        previous=key
        require(r["side"] in (0,1) and r["profile"]==0,"side/profile")
        require(all(0<=r[k]<=65535 for k in ("n_collocation","n_quadrature","n_quad_price",
                                           "max_newton_iter")),"option narrowing")
        require(all(r[k]>0 for k in ("S","K","T","budget","tol","sigma")),"positive model")
        if not r["opts_explicit"]:
            require([r[k] for k in ("n_collocation","n_quadrature","n_quad_price",
                    "max_newton_iter","opts_tol")]==[12,24,48,6,1e-10],"null options resolution")
    return rows


def convert(path):
    output=[]
    with path.open(newline="",encoding="utf-8") as f:
        for row in csv.DictReader(f):
            require(set(row)==INTS|BOOLS|FLOATS,"capture columns")
            parsed={k:int(v) if k in INTS else float(v) for k,v in row.items() if k not in BOOLS}
            for k in BOOLS:
                require(row[k] in ("0","1"),"capture boolean")
                parsed[k]=row[k]=="1"
            output.append(parsed)
    source=dict(schema="atx.cold-seed.requests.v1",requests=output)
    requests(source)
    return source


def validate_value(v,r):
    require(type(v["price_bits"]) is str,"bits encoding")
    raw=int(v["price_bits"])
    require(0<=raw<2**64,"bits range")
    decoded=struct.unpack("<d",struct.pack("<Q",raw))[0]
    p=v["price"]
    require((p is None and not math.isfinite(decoded)) or
            (number(p) and struct.pack("<d",p)==struct.pack("<d",decoded)),"price/bits mismatch")
    require(v["class"] in CLASSES,"class")
    require(type(v["patched"]) is bool and type(v["price_gate"]) is bool,"value flags")
    require(v["boundary"] is None or number(v["boundary"]),"boundary")
    gate=(p is not None and v["class"] not in ("unsupported_regime","exercise_plateau")
          and abs(p-r["target"])<=r["budget"])
    require(v["price_gate"]==gate,"price gate mismatch")


def summarize(source,native):
    rs=requests(source)
    require(native.get("schema")=="atx.cold-seed.replay.v1","native schema")
    require(type(native["request_count"]) is int and native["request_count"]==len(rs),"count")
    require(native["routes"]==ROUTES,"routes")
    require(len(native["rows"])==len(rs),"row denominator")
    rep=native["repetitions"]
    require(type(rep) is int and 1<=rep<=1000,"repetitions")
    seen=set();times=[[] for _ in ROUTES]
    for t in native["timings"]:
        require(all(type(t[k]) is int for k in ("rep","route","order")),"timing identity")
        key=(t["rep"],t["route"])
        require(0<=key[0]<rep and 0<=key[1]<4 and key not in seen,"timing denominator")
        require(t["order"]==(t["route"]-t["rep"])%4,"rotated order")
        require(number(t["seconds"]) and t["seconds"]>=0,"clock")
        seen.add(key);times[key[1]].append(t["seconds"])
    require(len(seen)==4*rep,"timing coverage")
    result=dict(requests=len(rs),batches=len({r["batch"] for r in rs}),
        exact_expiry_models=len({model_key(r) for r in rs}),
        captured_accepted=sum(r["accepted"] for r in rs),captured_proof_mismatches=0,
        geometry_bit_mismatches=0,geometry_class_mismatches=0,avx2_nonfinite=0,
        handle_bit_mismatches=0,
        avx2_price_gate_mismatches=0,avx2_class_mismatches=0,avx2_patched=0,
        max_avx2_absolute_price_difference=0.,max_avx2_difference_budget_fraction=0.,
        min_scalar_price_budget_margin=None,production_promotion=False,
        timing_scope="Component diagnostics only; no batched full proof is implemented.",
        timing_seconds={name:dict(min=min(v),median=statistics.median(v),max=max(v))
                        for name,v in zip(ROUTES,times)},failures=[])
    margins=[]
    split=native["attribution"]
    require(type(split["requests"]) is int and split["requests"]==len(rs),"attribution requests")
    require(len(split["prices"])==len(rs),"attribution prices")
    require(type(split["models"]) is int and split["models"]==result["exact_expiry_models"],"model cardinality")
    handles={model_key(r)+(r["side"],struct.pack("<d",r["S"] if r["side"]==0 else r["K"])) for r in rs}
    require(type(split["handles"]) is int and split["handles"]==len(handles),"handle cardinality")
    clocks=("setup_and_destruction_s","boundary_solve_s","price_evaluation_s","other_s","inclusive_s")
    require(all(number(split[k]) and split[k]>=0 for k in clocks),"attribution clocks")
    require(abs(sum(split[k] for k in clocks[:-1])-split["inclusive_s"])<=1e-12*max(1.,split["inclusive_s"]),"attribution reconciliation")
    result["attribution"]={k:split[k] for k in clocks+("models","handles","nonfinite")}
    nonfinite=0
    for r,row,h in zip(rs,native["rows"],split["prices"]):
        require(row["request"]==r,"request join")
        require(type(row["scalar_full_proof"]) is bool,"proof type")
        for route in ("scalar","geometry","avx2"):validate_value(row[route],r)
        s,g,v=(row[k] for k in ("scalar","geometry","avx2"))
        require(type(h["price_bits"]) is str,"handle bits encoding")
        decoded=struct.unpack("<d",struct.pack("<Q",int(h["price_bits"])))[0]
        require((h["price"] is None and not math.isfinite(decoded)) or
            (number(h["price"]) and struct.pack("<d",h["price"])==struct.pack("<d",decoded)),"handle bits")
        nonfinite+=h["price"] is None
        reasons=[]
        flags={"captured_proof_mismatches":row["scalar_full_proof"]!=r["accepted"],
            "geometry_bit_mismatches":s["price_bits"]!=g["price_bits"],
            "geometry_class_mismatches":s["class"]!=g["class"],
            "handle_bit_mismatches":s["price_bits"]!=h["price_bits"],
            "avx2_nonfinite":v["price"] is None,
            "avx2_price_gate_mismatches":s["price_gate"]!=v["price_gate"],
            "avx2_class_mismatches":s["class"]!=v["class"]}
        for key,value in flags.items():
            result[key]+=int(value)
            if value:reasons.append(key)
        result["avx2_patched"]+=v["patched"]
        if s["price"] is not None:
            margins.append(r["budget"]-abs(s["price"]-r["target"]))
            if v["price"] is not None:
                delta=abs(v["price"]-s["price"])
                result["max_avx2_absolute_price_difference"]=max(result["max_avx2_absolute_price_difference"],delta)
                result["max_avx2_difference_budget_fraction"]=max(result["max_avx2_difference_budget_fraction"],delta/r["budget"])
        if reasons:result["failures"].append(dict(batch=r["batch"],index=r["index"],reasons=reasons))
    result["min_scalar_price_budget_margin"]=min(margins) if margins else None
    require(type(split["nonfinite"]) is int and split["nonfinite"]==nonfinite,"attribution failures")
    return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("command",choices=("convert","summarize"))
    parser.add_argument("input",type=Path);parser.add_argument("output",type=Path)
    parser.add_argument("--native",type=Path)
    args=parser.parse_args()
    result=convert(args.input) if args.command=="convert" else summarize(
        json.loads(args.input.read_text()),json.loads(args.native.read_text()))
    args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+"\n",encoding="utf-8")


if __name__=="__main__":main()
