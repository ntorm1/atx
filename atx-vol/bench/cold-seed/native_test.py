"""Independent European premium control for the native request/replay seam."""
import json
import math
from pathlib import Path
import subprocess
import sys

from replay import summarize


def bs(S,K,T,sigma,r,q,call):
    v=sigma*math.sqrt(T)
    d1=(math.log(S/K)+(r-q+.5*sigma*sigma)*T)/v
    d2=d1-v
    cdf=lambda x:.5*math.erfc(-x/math.sqrt(2.))
    return (S*math.exp(-q*T)*cdf(d1)-K*math.exp(-r*T)*cdf(d2)) if call else (
        K*math.exp(-r*T)*cdf(-d2)-S*math.exp(-q*T)*cdf(-d1))


def main():
    binary=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    requests=[]
    for i,(S,K,T,sigma,call) in enumerate(((100,100,1,.2,True),(100,110,.2,.4,False),
            (99,100,.02,.3,True),(110,100,2,.25,False))):
        r,q=(.03,0.) if call else (0.,.03)
        requests.append(dict(batch=0,index=i,S=S,K=K,T=T,r=r,q=q,side=0 if call else 1,
            target=bs(S,K,T,sigma,r,q,call),budget=.001,tol=1e-7,sigma=sigma,accepted=True,
            opts_explicit=False,n_collocation=12,n_quadrature=24,n_quad_price=48,
            max_newton_iter=6,opts_tol=1e-10,profile=0))
    source=dict(schema="atx.cold-seed.requests.v1",requests=requests)
    inp=out/"european-input.json";native=out/"european-native.json"
    inp.write_text(json.dumps(source,indent=2)+"\n")
    subprocess.run([str(binary.resolve()),str(inp.resolve()),str(native.resolve()),"2"],check=True)
    result=json.loads(native.read_text());summary=summarize(source,result)
    assert summary["captured_proof_mismatches"]==0
    assert summary["geometry_bit_mismatches"]==0
    assert summary["handle_bit_mismatches"]==0
    assert summary["avx2_price_gate_mismatches"]==0
    for row in result["rows"]:
        for route in ("scalar","geometry","avx2"):
            assert row[route]["class"]=="european"
            assert abs(row[route]["price"]-row["request"]["target"])<2e-12
    (out/"european-summary.json").write_text(json.dumps(summary,indent=2)+"\n")
    print("4 independent European controls, two repetitions, all routes passed")


if __name__=="__main__":main()
