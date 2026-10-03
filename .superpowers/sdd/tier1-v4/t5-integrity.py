"""Post-producer streaming SHA/size and code identity verification; guarded, no return decoding."""
import argparse
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

p=argparse.ArgumentParser();p.add_argument('--stage',required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();stage=C.stage_dir(a.stage);mp=stage/'manifest.json'
if a.out.exists():raise RuntimeError('Refuse overwrite of prior integrity evidence')
before=C.sha256_file(mp);man=C.read_json(mp)
def files():
    result=[]
    for name,expected in sorted(man['files'].items()):
        path=(stage/name).resolve()
        if not path.is_relative_to(stage.resolve()):raise ValueError('Manifest path outside stage')
        actual={'file':name,'bytes':path.stat().st_size,'sha256':C.sha256_file(path)}
        actual['match']=actual['bytes']==expected['bytes'] and actual['sha256']==expected['sha256']
        result.append(actual)
    return result
first=files();second=files();after=C.sha256_file(mp)
code=[]
for name,pin in man.get('code',{}).items():
    if not name.endswith('.py'):continue
    path=C.PACKAGE_ROOT/'src/atx_db/alpha_panel'/name
    if not path.is_file():code.append({'file':name,'match':False});continue
    import hashlib
    raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest();lf=hashlib.sha256(raw.replace(b'\r\n',b'\n')).hexdigest()
    code.append({'file':name,'sha256':sha,'match':sha==pin.get('sha256') or lf==pin.get('sha256_lf')})
result={'stage':a.stage,'manifest_sha256':before,'manifest_unchanged':before==after,
        'files':first,'second_pass_identical':first==second,
        'physical_integrity_pass':bool(first) and first==second and before==after and all(r['match'] for r in first),
        'code':code,'current_code_pass':bool(code) and all(r['match'] for r in code),
        'producer_stats':man.get('stats'),'sources':man.get('sources'),
        'gate_status':man.get('gate_status'),'acceptance':'NOT_ESTABLISHED; integrity is not source/coverage qualification'}
C.write_json_atomic(a.out,result)
print(json.dumps({k:v for k,v in result.items() if k not in ('files','sources','producer_stats','code')}))
raise SystemExit(0 if result['physical_integrity_pass'] and result['current_code_pass'] else 2)
