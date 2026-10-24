"""Explicit-step FD diagnostics. No derivative budget or latency qualification."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import platform
import shutil
import subprocess
import sys

import greek_report as base

HERE = Path(__file__).resolve().parent
RISK_FIELDS = ['delta','gamma','vega','theta','rho','vanna','volga','charm']
PROFILES = {'default': base.DEFAULT_OPTIONS,
            **{k: dict(v, explicit=True) for k,v in base.PROFILES.items()}}
DESIGNS = ['legacy','spot_scaled','time_relative','spot_time']
SCALES = [1.,.5,.25,.125,.0625]
OFFSETS = [[0,0,0,0], [1,0,0,0], [-1,0,0,0], [0,1,0,0], [0,-1,0,0],
           [0,0,1,0], [0,0,-1,0], [0,0,0,1], [0,0,0,-1],
           [1,1,0,0], [1,-1,0,0], [-1,1,0,0], [-1,-1,0,0],
           [1,0,0,1], [-1,0,0,1], [1,0,0,-1], [-1,0,0,-1]]
REASONS = ['NotRequested','ComputedEstimate','Exercised','UnsupportedAxis','StencilCrossing','Nonfinite']
require, finite = base.require, base.finite


def identities():
    return [(p,d,s) for p in PROFILES for d in DESIGNS for s in (SCALES if d!='legacy' else [1.])]


def steps(c, design, scale):
    return [scale*min(.001*c['S'],.02*c['S']*c['sigma']*math.sqrt(c['T']))
            if design in ['spot_scaled','spot_time'] else .001*c['S'],
            .001,.0001,scale*.002*c['T'] if design in ['time_relative','spot_time'] else .001]


def reconstruct(samples, h, rho_forward, time_forward):
    dt = h[3] if time_forward else 2*h[3]
    dr = h[2] if rho_forward else 2*h[2]
    formulas = {
        'delta': ([1,2],[1,-1],2*h[0]),
        'gamma': ([1,0,2],[1,-2,1],h[0]*h[0]),
        'vega': ([3,4],[1,-1],2*h[1]),
        'theta': ([7,8],[-1,1],dt),
        'rho': ([5,6],[1,-1],dr),
        'vanna': ([9,10,11,12],[1,-1,-1,1],4*h[0]*h[1]),
        'volga': ([3,0,4],[1,-2,1],h[1]*h[1]),
        'charm': ([13,15,14,16],[-1,1,1,-1],2*h[0]*dt),
    }
    out = {}
    for field,(ids,weights,denominator) in formulas.items():
        prices = [samples[i]['price'] for i in ids]
        valid = all(finite(x) for x in prices)
        numerator = math.fsum(w*p for w,p in zip(weights,prices)) if valid else None
        magnitude = math.fsum(abs(w*p) for w,p in zip(weights,prices)) if valid else None
        out[field] = dict(value=numerator/denominator if valid else None,
            roundoff_indicator=math.ulp(1.)*magnitude/denominator if valid else None,
            cancellation_ratio=magnitude/abs(numerator) if valid and numerator else None,
            evidence_ids=sorted(set(ids)|{0}))
    return out


def sample_regime(sample, side):
    require(len(sample['model'])==6 and all(finite(x) for x in sample['model']), 'sample model')
    require(all(type(sample[k]) is bool for k in ['boundary_required','exercised','evaluated']), 'sample booleans')
    p,b = sample['price'],sample['internal_boundary']
    require(p is None or finite(p), 'sample price representation')
    require(b is None or finite(b), 'boundary representation')
    valid = finite(p) and (not sample['boundary_required'] or (finite(b) and b>0))
    S,K = sample['model'][:2]
    intrinsic = K-S if side=='put' else S-K
    exercised = valid and intrinsic>0 and ((finite(b) and (S if side=='put' else K)<=b) or p<=intrinsic)
    require(sample['exercised']==exercised, 'price/boundary exercise evidence')
    return ('Exercised' if exercised else 'Continuation') if valid else 'Nonfinite'


def european_greeks(c):
    """Independent closed form; caller establishes a strict no-exercise interior."""
    S,K,T,v,r,q=(c[k] for k in ['S','K','T','sigma','r','q'])
    root=math.sqrt(T); d1=(math.log(S/K)+(r-q+.5*v*v)*T)/(v*root); d2=d1-v*root
    cdf=lambda x: .5*math.erfc(-x/math.sqrt(2.))
    density=math.exp(-.5*d1*d1)/math.sqrt(2*math.pi)
    dq,dr=math.exp(-q*T),math.exp(-r*T)
    put=c['side']=='put'; n1=cdf(d1)-(1. if put else 0.); n2=cdf(d2)-(1. if put else 0.)
    vega=S*dq*density*root
    return dict(delta=dq*n1,gamma=dq*density/(S*v*root),vega=vega,
        theta=-S*dq*density*v/(2*root)+q*S*dq*n1-r*K*dr*n2,
        rho=K*T*dr*n2,vanna=-dq*density*d2/v,volga=vega*d1*d2/v,
        charm=q*dq*n1-dq*density*(2*(r-q)*T-d2*v*root)/(2*T*v*root))


def validate_evaluation(e, c):
    profile,design,scale = e['profile'],e['design'],e['scale']
    require((profile,design,scale) in identities() and type(scale) is float, 'evaluation identity')
    require(e['options']==PROFILES[profile] and type(e['options']['explicit']) is bool, 'option identity')
    h = steps(c,design,scale)
    require(e['requested_steps']==h and all(finite(x) for x in e['requested_steps']), 'declared steps')
    if c['sigma']-h[1]<=0: h[1]=.5*c['sigma']
    require(e['actual_steps']==h and all(finite(x) and x>0 for x in h), 'actual steps')
    time_forward = c['T']-h[3]<=1e-8
    rate,yield_ = (c['q'],c['r']-h[2]) if c['side']=='call' else (c['r']-h[2],c['q'])
    rho_forward = rate<0 and rate>yield_
    require(type(e['time_forward']) is bool and e['time_forward']==time_forward and
            type(e['rho_forward']) is bool and e['rho_forward']==rho_forward, 'one-sided rule')
    samples=e['samples']
    require(len(samples)==17, 'sample denominator')
    aliases={}
    if rho_forward: aliases[6]=0
    if time_forward: aliases.update({8:0,15:1,16:2})
    regimes=[]; stopped=False
    for i,sample in enumerate(samples):
        d=OFFSETS[aliases.get(i,i)]
        expected=[c['S']+d[0]*h[0],c['K'],c['T']+d[3]*h[3],
                  c['sigma']+d[1]*h[1],c['r']+d[2]*h[2],c['q']]
        require(sample['model']==expected, 'actual operand identity')
        if i in aliases:
            require(sample['evaluated'] is False, 'alias not evaluated')
        else:
            require(not (stopped and sample['evaluated']), 'sample after short circuit')
            stopped = stopped or not sample['evaluated']
        regimes.append(sample_regime(sample,c['side']))
        if i in aliases:
            require({k:v for k,v in sample.items() if k!='evaluated'} ==
                    {k:v for k,v in samples[aliases[i]].items() if k!='evaluated'}, 'alias evidence')
    work=e['work']
    require(set(work)=={'sample_attempts','boundary_cold_solves','boundary_warm_solves',
                       'boundary_price_calls','public_price_calls','european_price_calls','degenerate_samples'}, 'work schema')
    require(all(type(v) is int and v>=0 for v in work.values()), 'work counters')
    require(work['sample_attempts']==sum(s['evaluated'] for s in samples) and
            samples[0]['evaluated'] is True and work['boundary_warm_solves']==0 and
            work['boundary_cold_solves']<=7 and sum(work[k] for k in
            ['boundary_price_calls','public_price_calls','european_price_calls','degenerate_samples'])
            <=work['sample_attempts'], 'work accounting')
    require(finite(e['wall_s']) and e['wall_s']>=0, 'wall clock')
    require(type(e['result_success']) is bool, 'result status')
    for key in ['requested','supported']:
        require(type(e[key]) is int and 0<=e[key]<=511, 'availability masks')
    require(len(e['values'])==len(e['reasons'])==9 and
            all(type(v) is int and 0<=v<len(REASONS) for v in e['reasons']), 'field schema')
    reconstructed=reconstruct(samples,h,rho_forward,time_forward)
    if not e['result_success']:
        require(e['requested']==e['supported']==0 and e['reasons']==[0]*9 and
                e['values']==[None]*9 and 'error' in e, 'refused bundle')
    else:
        require(work['sample_attempts']==17-len(aliases), 'successful sample denominator')
        require(e['requested']==511, 'complete outward request')
        for i,field in enumerate(RISK_FIELDS+['price']):
            value,reason=e['values'][i],REASONS[e['reasons'][i]]
            supported=bool(e['supported']&(1<<i))
            require(supported==(reason in ['ComputedEstimate','Exercised']) and
                    (finite(value) if supported else value is None), 'availability/value consistency')
            if field=='price':
                require(supported and reason=='ComputedEstimate' and value==samples[0]['price'], 'price identity')
                continue
            calc=reconstructed[field]
            states={regimes[j] for j in calc['evidence_ids']}
            expected=('Nonfinite' if 'Nonfinite' in states else 'StencilCrossing' if len(states)>1
                      else 'Exercised' if 'Exercised' in states else 'ComputedEstimate')
            require(reason==expected, 'field regime admission')
            if reason=='Exercised':
                require(value==((-1. if c['side']=='put' else 1.) if field=='delta' else 0.), 'exercise derivative')
            elif reason=='ComputedEstimate':
                require(finite(calc['value']) and abs(value-calc['value']) <=
                        32*calc['roundoff_indicator']+8*math.ulp(value), 'stencil formula reconstruction')
    return reconstructed


def summarize(report, corpus, reference=None):
    require(report['schema']=='atx-american-fd-refinement-v1', 'schema')
    for key in ['qualified','latency_qualified','warm_start','reference_independent_method']:
        require(report[key] is False, 'unqualified scope')
    require(report['derivative_tolerances'] is None and report['continuous_carry_only'] is True and
            report['convention']==corpus['convention'], 'model/Greek contract')
    inputs={c['id']:c for c in corpus['cases']}
    ids=report['selected_ids']
    base.validate_requested_ids(report,ids)
    require(type(report['case_count']) is int and report['case_count']==len(report['cases'])==len(ids)
            and type(report['input_case_count']) is int and
            report['input_case_count']==len(inputs)==corpus['case_count'], 'case denominator')
    require(set(ids)<=set(inputs), 'unknown case')
    refs={}
    if reference is not None:
        base.validate_report(reference,corpus)
        refs={r['input']['id']:r for r in reference['cases']}
        require(set(ids)<=set(refs), 'reference omitted selected case')
    totals={}; details=[]; seen=set()
    for row in report['cases']:
        c=row['input']; cid=c['id']
        require(cid in ids and cid not in seen and c==inputs[cid], 'frozen case join'); seen.add(cid)
        entries=row['evaluations']
        require(len(entries)==48 and len({(e['profile'],e['design'],e['scale']) for e in entries})==48,
                'evaluation denominator')
        reference_fields={}
        rate,yield_=(c['q'],c['r']) if c['side']=='call' else (c['r'],c['q'])
        euro=european_greeks(c) if rate<0 and rate<yield_ else None
        if cid in refs:
            for p in ['refined24','refined32']:
                reference_fields[p]=[base.derivatives(level) for level in refs[cid]['references'][p]['stencils']]
        out=[]
        for e in entries:
            reconstruction=validate_evaluation(e,c)
            key=f"{e['profile']}/{e['design']}/{e['scale']}"
            tally=totals.setdefault(key,dict(requested=0,supplied=0,reasons=Counter(),work=Counter()))
            tally['work'].update(e['work'])
            fields={}
            for i,field in enumerate(RISK_FIELDS+['phi']):
                supplied=e['result_success'] and field!='phi' and bool(e['supported']&(1<<i))
                reason=('NotInApi' if field=='phi' else REASONS[e['reasons'][i]]
                        if e['result_success'] else 'ApiRefused')
                value=e['values'][i] if supplied else None
                tally['requested']+=1; tally['supplied']+=int(supplied); tally['reasons'][reason]+=1
                item=dict(supplied=supplied,reason=reason,value=value)
                if field!='phi':
                    item.update(reconstruction[field])
                    item['value']=value
                    item['independent_european_value']=euro[field] if euro else None
                    item['abs_error_vs_independent_european']=(abs(value-euro[field])
                        if euro and supplied else None)
                if field in base.FIELDS and reference_fields:
                    ref=[x[field]['value'] for x in reference_fields['refined32']]
                    other=reference_fields['refined24'][-1][field]['value']
                    item['reference_finest']=ref[-1]
                    item['reference_last_step']=abs(ref[-1]-ref[-2]) if all(finite(x) for x in ref[-2:]) else None
                    item['reference_profile_drift']=abs(ref[-1]-other) if finite(ref[-1]) and finite(other) else None
                    item['reference_intrinsic_changed']=any(x[field]['intrinsic_band_changed'] for x in reference_fields['refined32'])
                    item['abs_error_vs_reference']=abs(value-ref[-1]) if supplied and finite(ref[-1]) else None
                    # Explicit shock coefficients measure Taylor contribution differences,
                    # not full repricing P&L or an accepted derivative budget.
                    ds=.001*c['S']; dt=60/(365*24*60*60); dv=dr=.0001
                    shock={'delta':ds,'gamma':.5*ds*ds,'vega':dv,'theta':dt,'rho':dr,
                           'vanna':ds*dv,'volga':.5*dv*dv,'charm':ds*dt,'phi':dr}[field]
                    item['shock_contribution_abs_difference']=(shock*item['abs_error_vs_reference']
                                                              if item['abs_error_vs_reference'] is not None else None)
                fields[field]=item
            out.append(dict(profile=e['profile'],design=e['design'],scale=e['scale'],fields=fields,
                            work=e['work'],wall_s_unqualified=e['wall_s']))
        trends={}
        for profile in PROFILES:
            for design in DESIGNS[1:]:
                levels=[next(e for e in out if (e['profile'],e['design'],e['scale'])==(profile,design,s))
                        for s in SCALES]
                per_field={}
                for field in RISK_FIELDS:
                    values=[e['fields'][field]['value'] for e in levels]
                    changes=[abs(b-a) if finite(a) and finite(b) else None for a,b in zip(values,values[1:])]
                    per_field[field]=dict(values=values,changes=changes,
                        reasons=[e['fields'][field]['reason'] for e in levels],
                        last_change_decreased=changes[-1]<=changes[-2]
                            if all(finite(x) for x in changes[-2:]) else None,
                        note='Step drift only; finer is not automatically more accurate.')
                trends[f'{profile}/{design}']=per_field
        details.append(dict(input=c,evaluations=out,trends=trends))
    for tally in totals.values():
        tally['reasons']=dict(tally['reasons']); tally['work']=dict(tally['work'])
    return dict(schema='atx-american-fd-refinement-summary-v1',case_count=len(details),
        field_requests=sum(t['requested'] for t in totals.values()),qualified=False,derivative_tolerances=None,
        latency_qualified=False,reference_independent_method=False,
        shock_convention=dict(spot_fraction=.001,calendar_seconds=60,year_seconds=31536000,
                              volatility=.0001,rate=.0001,note='Taylor contribution differences, not full P&L'),
        totals=totals,cases=details)


def run(binary, output, selection, reference):
    output=Path(output).resolve(); output.mkdir(parents=True,exist_ok=False)
    corpus=HERE/'corpus-v1.json'; require(base.digest(corpus)==base.CORPUS_SHA,'frozen corpus hash')
    data=json.loads(corpus.read_text()); expected=json.loads(Path(selection).read_text())
    binary=Path(binary).resolve(); archived=output/'atx-vol-fd-refinement.exe'
    shutil.copy2(binary,archived); shutil.copy2(selection,output/'selection.json')
    native=output/'native.json'
    command=[str(archived),str(corpus),str(output/'selection.json'),str(native)]
    with (output/'native.log').open('w') as log:
        subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT)
    require(base.digest(binary)==base.digest(archived),'binary changed during run')
    report=json.loads(native.read_text()); base.validate_requested_ids(report,expected)
    ref=json.loads(Path(reference).read_text()) if reference else None
    result=summarize(report,data,ref)
    (output/'summary.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    root=HERE.parents[2]
    sources=[HERE/x for x in ['fd_refinement_main.cpp','fd_refinement.py','test_fd_refinement.py','greek_report.py']]
    sources += [root/'atx-vol/CMakeLists.txt',root/'atx-vol/src/pricing/american.cpp',
                root/'atx-vol/src/pricing/american_greek_fd_audit.hpp']
    manifest=dict(schema='atx-american-fd-refinement-run-v1',utc=datetime.now(timezone.utc).isoformat(),
        source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        source_status=subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True),
        python=sys.version,platform=platform.platform(),command=command,corpus_sha256=base.digest(corpus),
        expected_case_ids=expected,derivative_tolerances=None,latency_qualified=False,
        reference=dict(path=str(reference),sha256=base.digest(reference)) if reference else None,
        files={p.name:base.digest(p) for p in output.iterdir() if p.is_file()},
        sources={str(p.relative_to(root)):base.digest(p) for p in sources})
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary'); parser.add_argument('--report'); parser.add_argument('--output',required=True)
    parser.add_argument('--selection',default=str(HERE/'refinement-v1.json')); parser.add_argument('--reference')
    args=parser.parse_args()
    if args.report:
        require(base.digest(HERE/'corpus-v1.json')==base.CORPUS_SHA,'frozen corpus hash')
        native=json.loads(Path(args.report).read_text())
        base.validate_requested_ids(native,json.loads(Path(args.selection).read_text()))
        result=summarize(native,json.loads((HERE/'corpus-v1.json').read_text()),
                         json.loads(Path(args.reference).read_text()) if args.reference else None)
        Path(args.output).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    else:
        require(bool(args.binary),'binary required')
        result=run(args.binary,args.output,args.selection,args.reference)
    print(json.dumps(dict(case_count=result['case_count'],field_requests=result['field_requests'],qualified=False)))
