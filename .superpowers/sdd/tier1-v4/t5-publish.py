"""Task5 guarded phases: nonzero child exits stop; measured gate FAIL exits2 after honest publication."""
import argparse
import json
import os
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import fund_depth as FD

def call(module,args):
    result=module.main(args)
    if result not in (None,0):
        raise SystemExit(result)

def new_only(stage):
    dest=C.stage_dir(stage)
    if (dest/'manifest.json').exists() or any(dest.glob('*.parquet')):
        raise RuntimeError(f'Refuse overwrite of existing stage {stage}; coordinate explicit recovery')
    return dest

def fund_gates(doc):
    cov=doc['coverage'];bal=doc['balance']
    gates={'coverage_105_of_117':cov['items_counted']==117 and cov['items_pass_covered']>=105,
           'balance_985':bal['issuer_periods']>0 and bal['ok']/bal['issuer_periods']>=.985,
           'benchmark':doc['benchmark']['pass'],'cutoff':doc['cutoff']['pass']}
    for name,target in {'gp_ttm':.90,'sale_ttm':.96,'capx_ttm':.95,'txt_q':.95,'xrd_ttm':.95}.items():
        actual=doc['exit']['fundamentals_v11']['pooled'][name]['linked_usd']
        gates['exit_'+name]=actual is not None and actual>=target
    return {k:'PASS' if v else 'FAIL' for k,v in gates.items()}

def finish(stage,gates):
    if not gates:
        raise RuntimeError('Check phase produced no measured gate status')
    required={'compustat_std_v11':('coverage','rdq','sum4','balance','fsds','cutoff','handcheck'),
              'ratios_v11':('coverage','coverage_univ_all_common','coverage_univ_r3000p','spearman','ranges','handcheck')}
    gates=dict(gates)
    for name in required.get(stage,()):gates.setdefault(name,'NOT_MEASURED')
    dest=C.stage_dir(stage);man=C.read_json(dest/'manifest.json')
    man['gate_status']=gates
    C.write_json_atomic(dest/'manifest.json',man)
    failed=any(v!='PASS' for v in gates.values())
    outcome={'execution_status':'SUCCESS','acceptance_status':'FAIL' if failed else 'NOT_ESTABLISHED',
             'measured_gate_status':'FAIL' if failed else 'PASS',
             'gate_status':gates,'unmeasured_dependencies':'Independent source/composite/comparison qualifications remain open'}
    C.write_json_atomic(dest/'task5_check_status.json',outcome)
    print(json.dumps(outcome),flush=True)
    return 2 if failed else 0

def main(argv=None):
    p=argparse.ArgumentParser();p.add_argument('phase',choices=('fund','fund-check','std','std-check','ratios','ratios-check'))
    phase=p.parse_args(argv).phase;os.environ['ATX_FUND_STAGE']='fundamentals_v11'
    if phase=='fund':
        from atx_db.alpha_panel import fundamentals as F
        dest=new_only('fundamentals_v11')
        print(json.dumps(FD.reuse_extract(C.stage_dir('fundamentals_v10'),dest)),flush=True)
        call(F,['build-all'])
    elif phase=='fund-check':
        from atx_db.alpha_panel import fundamentals as F, fund_validate as V
        call(F,['validate']);call(F,['validate-cutoff']);call(V,['coverage','benchmark','balance','cutoff','exit'])
        gates=fund_gates(C.read_json(V.out_path()))
        cut=C.read_json(C.stage_dir('fundamentals_v11')/'validation_cutoff.json')['by_cutoff']
        if not cut:raise RuntimeError('No producer cutoff measurements')
        keys=('events_missing','events_mismatched','history_rows_not_reproduced','history_rows_extra_in_rebuild')
        gates['producer_cutoff']='PASS' if all(r[k]==0 for r in cut.values() for k in keys) else 'FAIL'
        return finish('fundamentals_v11',gates)
    elif phase=='std':
        from atx_db.alpha_panel import compustat_std as S
        new_only('compustat_std_v11');call(S,['build','--fund-stage','fundamentals_v11','--stage','compustat_std_v11'])
    elif phase=='std-check':
        from atx_db.alpha_panel import compustat_std as S
        call(S,['validate','--stage','compustat_std_v11']);call(S,['cutoff','--stage','compustat_std_v11'])
        return finish('compustat_std_v11',S.publish_gate_status('compustat_std_v11'))
    elif phase=='ratios':
        from atx_db.alpha_panel import ratios as R
        new_only('ratios_v11');call(R,['build','--std-stage','compustat_std_v11','--stage','ratios_v11'])
    else:
        from atx_db.alpha_panel import ratios as R
        call(R,['validate','--std-stage','compustat_std_v11','--stage','ratios_v11'])
        return finish('ratios_v11',R.publish_gate_status('ratios_v11'))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
