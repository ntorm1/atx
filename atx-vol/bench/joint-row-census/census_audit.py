"""Independent row-census integrity reader; no QP solve or timing claim.

The recorded clang-cl build contracts ordered multiply/add expressions. Each
FMA is reconstructed with exact rational operands and one binary64 rounding.
This checks observation integrity for that build, not cross-solver identity.
"""
import argparse
from collections import Counter
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct

LIMITS = dict(slices=64, nodes=7000, coordinates=14000, variables=14000, rows=60000)
TOLERANCE = 1e-10

def require(condition, message):
    if not condition:
        raise ValueError(message)

def integer(value, lo, hi, label):
    require(type(value) is int and lo <= value <= hi, label)
    return value

def bits(value):
    return struct.pack('>d', value).hex()

def number(value):
    require(type(value) is str and len(value) == 16 and
            all(c in '0123456789abcdef' for c in value), 'binary64 spelling')
    result = struct.unpack('>d', bytes.fromhex(value))[0]
    require(math.isfinite(result), 'nonfinite binary64')
    return result

def fma(a, b, c):
    require(all(type(x) in (float, int) and math.isfinite(x) for x in (a,b,c)), 'FMA input')
    value = Fraction(a)*Fraction(b)+Fraction(c)
    if value == 0:
        negative_product = math.copysign(1., a) != math.copysign(1., b)
        return -0. if (a == 0 or b == 0) and c == 0 and negative_product and math.copysign(1., c) < 0 else 0.
    try:
        result = float(value)
    except OverflowError as exc:
        raise ValueError('FMA overflow') from exc
    require(math.isfinite(result), 'FMA overflow')
    return result

def exact(a, b, label):
    require(bits(a) == bits(b), label)

def fields(item, keys):
    require(type(item) is dict and set(item) == set(keys.split()), 'object fields: '+keys)

def numbers(values, count):
    require(type(values) is list and len(values) == count, 'numeric array length')
    return [number(v) for v in values]

def graph(rows, variables):
    # Match NewtonSystem's stored index graph, even for a merged zero coefficient.
    parent = list(range(variables)); sizes = [1]*variables
    edges = set(); touched = set()
    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for row in rows:
        ids = row['indices']; touched.update(ids)
        for k,a in enumerate(ids):
            for b in ids[:k]:
                if a == b: continue
                edges.add((min(a,b),max(a,b)))
                x,y = find(a),find(b)
                if x != y:
                    if sizes[x] < sizes[y]: x,y = y,x
                    parent[y] = x; sizes[x] += sizes[y]
    return dict(matrix_nonzeros=variables+len(edges), off_diagonal_edges=len(edges),
                component_sizes=sorted([sizes[i] for i in range(variables) if find(i)==i], reverse=True),
                touched_variables=len(touched))

def nondividend(bounds):
    spot,_,dmax,pmin,pmax = bounds
    return pmin == spot and pmax == spot and dmax == 1.

def build_problem(slices, nodes):
    """Rebuild all constraints from model inputs, never from captured rows."""
    coordinates=[]; variables=[]; rows=[]
    for s in slices:
        spot,dmin,dmax,pmin,pmax = s['bounds']
        for node in nodes[s['node_offset']:s['node_offset']+s['nodes']]:
            k=node['strike']
            for side in (0,1):
                df=s['terminal_discount']
                if df is None: df=dmax if side==0 else dmin
                prepaid=node['terminal_prepaid'] if s['has_terminal_prepaid'] else (pmin if side==0 else pmax)
                floor=max((spot-k) if side==0 else (k-spot),0.)
                floor=max(floor, fma(-df,k,prepaid) if side==0 else fma(df,k,-prepaid))
                if side==0 and nondividend(s['bounds']): floor=max(floor,fma(-dmin,k,spot))
                lo=max(node['box'][2*side],floor)
                hi=min(node['box'][2*side+1],pmax if side==0 else dmax*k)
                require(lo<=hi, 'contradictory captured effective box')
                coordinates.append(dict(variable=-1 if lo==hi else len(variables),fixed=lo))
                if lo<hi: variables.append(dict(lower=lo,upper=hi,target=node['selected'][side]))
    def term(row,index,value):
        if value == 0.: return
        for j,i in enumerate(row['indices']):
            if i==index:
                row['coefficients'][j] += value
                return
        row['indices'].append(index);row['coefficients'].append(value)
    def add(terms,rhs,kind):
        original=dict(indices=[],coefficients=[])
        for i,a in terms: term(original,i,a)
        row=dict(indices=[],coefficients=[],rhs=rhs,kind=kind)
        for i,a in zip(original['indices'],original['coefficients']):
            c=coordinates[i]
            if c['variable']<0: row['rhs']=fma(-a,c['fixed'],row['rhs'])
            else: term(row,c['variable'],a)
        low=high=-row['rhs']; norm=0.
        for i,a in zip(row['indices'],row['coefficients']):
            v=variables[i]
            low=fma(a,v['lower'] if a>0 else v['upper'],low)
            high=fma(a,v['upper'] if a>0 else v['lower'],high)
            norm=fma(a,a,norm)
        require(high>=-TOLERANCE,'contradictory captured row')
        if low>=0. or norm==0.: return
        scale=1./math.sqrt(norm)
        row['coefficients']=[a*scale for a in row['coefficients']]
        row['rhs']*=scale
        rows.append(row)
    def interp(s,side,k,sign):
        start=s['node_offset']; count=s['nodes']; offset=s['coordinate_offset']
        left,right=0,count
        while left<right:
            mid=(left+right)//2
            if nodes[start+mid]['strike']<k: left=mid+1
            else: right=mid
        right=left
        if right<count and nodes[start+right]['strike']==k: return [(offset+2*right+side,sign)]
        require(0<right<count,'interpolation support')
        left=right-1
        f=(k-nodes[start+left]['strike'])/(nodes[start+right]['strike']-nodes[start+left]['strike'])
        return [(offset+2*left+side,sign*(1.-f)),(offset+2*right+side,sign*f)]
    for s in slices:
        ns=nodes[s['node_offset']:s['node_offset']+s['nodes']]
        spot,dmin,dmax,pmin,pmax=s['bounds']; offset=s['coordinate_offset']; count=len(ns)
        def coordinate(i,side): return offset+2*i+side
        for side in (0,1):
            for i in range(1,count-1):
                left=ns[i]['strike']-ns[i-1]['strike'];right=ns[i+1]['strike']-ns[i]['strike']
                add([(coordinate(i-1,side),right/(left+right)),(coordinate(i,side),-1.),
                     (coordinate(i+1,side),left/(left+right))],0.,'convexity')
            call_min=-(dmin if nondividend(s['bounds']) else dmax)
            add([(coordinate(1,side),1.),(coordinate(0,side),-1.)],
                (call_min if side==0 else 0.)*(ns[1]['strike']-ns[0]['strike']),'front_slope')
            add([(coordinate(count-2,side),1.),(coordinate(count-1,side),-1.)],
                -(0. if side==0 else dmax)*(ns[-1]['strike']-ns[-2]['strike']),'back_slope')
        for i,node in enumerate(ns):
            add([(coordinate(i,0),1.),(coordinate(i,1),-1.)],fma(-node['strike'],dmax,pmin),'parity_lower')
            add([(coordinate(i,0),-1.),(coordinate(i,1),1.)],-fma(-node['strike'],dmin,pmax),'parity_upper')
        if nondividend(s['bounds']):
            for i in range(count-1):
                scale=ns[i]['strike']/(ns[i+1]['strike']-ns[i]['strike'])
                add([(coordinate(i,0),scale+1.),(coordinate(i,1),-scale-1.),
                     (coordinate(i+1,0),-scale),(coordinate(i+1,1),scale)],spot,'nondividend_lower')
            low=ns[0]['strike']; high=dmin*ns[-1]['strike']
            for node in ns:
                for k in (node['strike'],dmin*node['strike']):
                    if low<=k<=high:
                        add(interp(s,0,min(k/dmin,ns[-1]['strike']),1.)+interp(s,1,k,-1.),spot-k,'nondividend_upper')
    for a,near in enumerate(slices):
        bridge_low=-math.inf; bridge_high=math.inf
        for b in range(a+1,len(slices)):
            far=slices[b]
            if b>a+1:
                middle=slices[b-1]
                bridge_low=max(bridge_low,nodes[middle['node_offset']]['strike'])
                bridge_high=min(bridge_high,nodes[middle['node_offset']+middle['nodes']-1]['strike'])
            nk=[n['strike'] for n in nodes[near['node_offset']:near['node_offset']+near['nodes']]]
            fk=[n['strike'] for n in nodes[far['node_offset']:far['node_offset']+far['nodes']]]
            low=max(nk[0],fk[0]); high=min(nk[-1],fk[-1])
            for k in sorted(set(nk+fk)):
                if not low<=k<=high or (b>a+1 and bridge_low<=k<=bridge_high): continue
                for side in (0,1): add(interp(far,side,k,1.)+interp(near,side,k,-1.),0.,'calendar')
    for i,v in enumerate(variables):
        rows.append(dict(indices=[i],coefficients=[1.],rhs=v['lower'],kind='bound_lower'))
        rows.append(dict(indices=[i],coefficients=[-1.],rhs=-v['upper'],kind='bound_upper'))
    return coordinates,variables,rows

def slack(row, values):
    value=-row['rhs']
    for i,a in zip(row['indices'],row['coefficients']): value=fma(a,values[i],value)
    return value

def validate_report(r):
    require(type(r) is dict and r.get('schema')=='atx-joint-row-census-v1','schema')
    require(r.get('valid') is True and r.get('error')==0 and type(r.get('error')) is int,'capture refusal')
    for key in ('attempts','built_events','published_events'): integer(r.get(key),1,1,key)
    require(r.get('timing_qualifying') is False,'census cannot qualify latency')
    require(r.get('row_arithmetic')=='ordered_fma_binary64_nearest_v1' and
            r.get('arithmetic_evidence_required') is True,'row arithmetic')
    for key in ('counts','capacity'):
        fields(r[key], 'slices nodes coordinates variables rows')
        for name,cap in LIMITS.items(): integer(r[key][name],0,cap,key+name)
    counts=r['counts']; capacity=r['capacity']
    for name in LIMITS:
        require(type(r[name]) is list and len(r[name])==counts[name]<=capacity[name],name+' denominator')
    require(counts['slices']>0 and counts['nodes']>=2*counts['slices'] and
            counts['coordinates']==2*counts['nodes'],'slice/node/coordinate denominator')
    require(number(r['tolerance'])==TOLERANCE,'tolerance contract')
    budget=integer(r['iteration_budget'],1,2**32-1,'iteration budget')
    integer(r['sweeps'],0,budget,'sweeps')
    d=r['diagnostics']; fields(d,'variables constraints certificate_passed primal_residual dual_residual duality_gap')
    require(type(d['variables']) is int and d['variables']==counts['variables'] and
            type(d['constraints']) is int and d['constraints']==counts['rows'] and
            d['certificate_passed'] is True,'diagnostics denominator/certificate')
    for name in ('primal_residual','dual_residual','duality_gap'): number(d[name])
    slices=[];nodes=[];offset=0
    for s in r['slices']:
        fields(s,'node_offset nodes coordinate_offset bounds terminal_discount has_terminal_prepaid')
        require(type(s['node_offset']) is int and s['node_offset']==offset and
                type(s['coordinate_offset']) is int and s['coordinate_offset']==2*offset,'slice offsets')
        count=integer(s['nodes'],2,counts['nodes'],'slice nodes');offset+=count
        bounds=numbers(s['bounds'],5);spot,dmin,dmax,pmin,pmax=bounds
        require(spot>0 and 0<dmin<=1<=dmax and 0<=pmin<=spot<=pmax,'model bounds')
        df=None if s['terminal_discount'] is None else number(s['terminal_discount'])
        require(df is None or dmin<=df<=dmax,'terminal discount')
        require(type(s['has_terminal_prepaid']) is bool and
                (not s['has_terminal_prepaid'] or df is not None),'terminal prepaid mode')
        slices.append(dict(s,bounds=bounds,terminal_discount=df))
    require(offset==counts['nodes'],'slice coverage')
    for n in r['nodes']:
        fields(n,'strike selected raw published terminal_prepaid box')
        nodes.append(dict(strike=number(n['strike']),selected=numbers(n['selected'],2),
            raw=numbers(n['raw'],2),published=numbers(n['published'],2),
            terminal_prepaid=number(n['terminal_prepaid']),box=numbers(n['box'],4)))
    for s in slices:
        previous=0.
        for n in nodes[s['node_offset']:s['node_offset']+s['nodes']]:
            require(n['strike']>previous,'strike order');previous=n['strike']
            require(n['box'][0]<=n['box'][1] and n['box'][2]<=n['box'][3],'raw effective boxes')
            if s['has_terminal_prepaid']:
                p=n['terminal_prepaid'];allowance=64*2.**-52*max(1.,s['bounds'][4])
                require(p>=0 and s['bounds'][3]-allowance<=p<=s['bounds'][4]+allowance,'prepaid envelope')
    expected_coordinates,expected_variables,expected_rows=build_problem(slices,nodes)
    require(len(expected_variables)==counts['variables'] and len(expected_rows)==counts['rows'],'rebuilt full denominator')
    for c,expected in zip(r['coordinates'],expected_coordinates):
        fields(c,'variable fixed');integer(c['variable'],-1,max(-1,counts['variables']-1),'coordinate variable')
        require(c['variable']==expected['variable'],'coordinate identity')
        exact(number(c['fixed']),expected['fixed'],'fixed coordinate bits')
    variables=[]
    for v,expected in zip(r['variables'],expected_variables):
        fields(v,'lower upper target initial iterate published')
        value={k:number(x) for k,x in v.items()}
        for key in ('lower','upper','target'): exact(value[key],expected[key],'rebuilt variable '+key)
        exact(value['initial'],min(max(value['target'],value['lower']),value['upper']),'actual initial clamp')
        exact(value['published'],min(max(value['iterate'],value['lower']),value['upper']),'actual publication clamp')
        variables.append(value)
    for node_index,n in enumerate(nodes):
        for side in (0,1):
            c=expected_coordinates[2*node_index+side]
            v=c['fixed'] if c['variable']<0 else variables[c['variable']]['published']
            exact(n['published'][side],v,'node publication map')
    initial=[v['initial'] for v in variables];published=[v['published'] for v in variables]
    rows=[];initial_values=[];published_values=[];arities=[0]*5
    for index,(row,expected) in enumerate(zip(r['rows'],expected_rows)):
        fields(row,'count indices coefficients rhs initial_slack')
        count=integer(row['count'],1,4,'row arity');arities[count]+=1
        require(type(row['indices']) is list and len(row['indices'])==4,'row index storage')
        for i in row['indices']: integer(i,0,max(0,counts['variables']-1),'row index')
        coefs=numbers(row['coefficients'],4)
        require(row['indices'][:count]==expected['indices'] and count==len(expected['indices']),'rebuilt row indices '+str(index))
        require(row['indices'][count:]==[0]*(4-count),'unused row indices')
        for a,b in zip(coefs[:count],expected['coefficients']): exact(a,b,'rebuilt row coefficient '+str(index))
        for a in coefs[count:]: exact(a,0.,'unused row coefficient')
        rhs=number(row['rhs']); exact(rhs,expected['rhs'],'rebuilt row RHS '+str(index))
        decoded=dict(indices=row['indices'][:count],coefficients=coefs[:count],rhs=rhs,kind=expected['kind'])
        value=slack(decoded,initial);exact(value,number(row['initial_slack']),'native initial slack '+str(index))
        rows.append(decoded);initial_values.append(value);published_values.append(slack(decoded,published))
    full_graph=graph(rows,counts['variables']); p=r['profile']
    if p is not None:
        fields(p,'row_arities bound_rows row_storage_bytes matrix_nonzeros factor_nonzeros factorizations directions centrality_enabled')
        require(type(p['row_arities']) is list and len(p['row_arities'])==5 and
                all(type(x) is int for x in p['row_arities']) and p['row_arities']==arities,'row arity profile')
        for k in ('bound_rows','row_storage_bytes','matrix_nonzeros','factor_nonzeros','factorizations','directions'):
            integer(p[k],0,2**63-1,'profile '+k)
        require(p['bound_rows']==2*counts['variables'] and p['row_storage_bytes']>=64*counts['rows'],'profile storage/bounds')
        require(type(p['centrality_enabled']) is bool and not p['centrality_enabled'],'census original direction mode')
        if p['matrix_nonzeros']:
            require(p['matrix_nonzeros']==full_graph['matrix_nonzeros'],'full graph/profile join')
        else: require(r['sweeps']==0 and p['factorizations']==p['directions']==0,'missing factor graph')
    violated=[i for i,v in enumerate(initial_values) if v < -TOLERANCE]
    bound_start=len(rows)-2*counts['variables']
    selected=sorted(set(violated)|set(range(bound_start,len(rows))))
    final_violations=[dict(row=i,slack=v,kind=rows[i]['kind']) for i,v in enumerate(published_values) if v < -TOLERANCE]
    return dict(schema='atx-joint-row-census-summary-v1',integrity_passed=True,timing_qualifying=False,
        arithmetic_evidence_verified=False,counts=counts,bounds_retained=2*counts['variables'],
        row_kinds=dict(Counter(row['kind'] for row in rows)),row_arities=arities,
        initial_violated=len(violated),initial_violated_ids=violated,
        initial_near_boundary=sum(abs(v)<=TOLERANCE for v in initial_values),
        selected_rows=len(selected),selected_row_ids=selected,
        graph_full=full_graph,graph_selected=graph([rows[i] for i in selected],counts['variables']),
        final_violations=final_violations,final_full_row_scan_passed=not final_violations,
        final_min_slack=min(published_values,default=0.),
        raw_selected_different_coordinates=sum(bits(n['raw'][s])!=bits(n['selected'][s]) for n in nodes for s in (0,1)),
        selected_objective_half_squared=math.fsum(.5*(v['published']-v['target'])**2 for v in variables),
        no_reduced_solver_executed=True)

def strict_json(path):
    path=Path(path);require(path.stat().st_size<=64*1024*1024,'JSON byte cap')
    def pairs(items):
        out={}
        for key,value in items:
            require(key not in out,'duplicate JSON key');out[key]=value
        return out
    def constant(_): raise ValueError('nonfinite JSON')
    def floating(value):
        result=float(value);require(math.isfinite(result),'nonfinite JSON');return result
    return json.loads(path.read_text(encoding='utf-8'),object_pairs_hook=pairs,
                      parse_constant=constant,parse_float=floating)

def bind_artifacts(capture, evidence_path):
    """Hash-bind reviewed codegen and observed executable; never infer it from a label.

    The component reviewer reads the frozen disassembly. This function verifies
    that those reviewed artifacts remain the ones used by this capture.
    """
    proof=strict_json(evidence_path)
    fields(proof,'schema row_arithmetic artifacts row_value_observation')
    require(proof['schema']=='atx-joint-row-arithmetic-evidence-v1' and
            proof['row_arithmetic']==capture['row_arithmetic'],'arithmetic evidence schema')
    require(type(proof['row_value_observation']) is str and proof['row_value_observation'], 'reviewed instruction observation')
    artifacts=proof['artifacts']
    fields(artifacts,'executable object source disassembly cache')
    for name,record in artifacts.items():
        fields(record,'path bytes sha256')
        require(type(record['path']) is str and type(record['sha256']) is str,'artifact metadata')
        path=Path(record['path'])
        if not path.is_absolute(): path=Path(evidence_path).parent/path
        data=path.read_bytes()
        integer(record['bytes'],0,2**31-1,'artifact bytes')
        require(len(data)==record['bytes'] and hashlib.sha256(data).hexdigest()==record['sha256'],'artifact hash '+name)
    require(capture.get('executable_sha256')==artifacts['executable']['sha256'],'capture/executable join')
    require(capture.get('build_info',{}).get('joint_row_census',{}).get('enabled') is True,'linked capture mode')
    return hashlib.sha256(Path(evidence_path).read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--arithmetic-evidence',type=Path)
    args=p.parse_args();require(not args.output.exists(),'output already exists')
    capture=strict_json(args.capture)
    out=validate_report(capture)
    if 'executable_sha256' in capture:
        require(args.arithmetic_evidence is not None,'tool capture requires reviewed arithmetic artifacts')
    if args.arithmetic_evidence:
        out['arithmetic_evidence_sha256']=bind_artifacts(capture,args.arithmetic_evidence)
        out['arithmetic_evidence_verified']=True
    out['capture_sha256']=hashlib.sha256(args.capture.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(out,indent=2,allow_nan=False)+'\n',encoding='utf-8')

if __name__=='__main__': main()
