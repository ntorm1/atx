"""Pinned first-role metadata/axes/mask audit; never opens numerical fields."""
from pathlib import Path
import datetime as dt
import hashlib
import json
import numpy as np
ROOT=Path('C:/atx-wt/pool-2/build-equity/recent-dev-smoke-v1')
OUT=Path('C:/atx-wt/pool-3/.superpowers/sdd/w2')
PIN='900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b'
b=(ROOT/'manifest.json').read_bytes(); assert hashlib.sha256(b).hexdigest()==PIN
m=json.loads(b); D,N=m['dates'],m['instruments']; assert 0<D<=4096 and 0<N<=20000 and D*N<8_000_000
arrays={}; bindings={}
for name,dtype,count in [('sessions.i64','<i8',D),('ids.u64','<u8',N),('present.u8','u1',D*N),('member.u8','u1',D*N)]:
 raw=(ROOT/name).read_bytes(); sha=hashlib.sha256(raw).hexdigest(); spec=m['files'][name]
 assert len(raw)==spec['bytes']==np.dtype(dtype).itemsize*count and sha==spec['sha256']
 arrays[name]=np.frombuffer(raw,dtype=dtype); bindings[name]={'bytes':len(raw),'sha256':sha,'verified':True}
sessions=arrays['sessions.i64']; ids=arrays['ids.u64']; P=arrays['present.u8'].reshape(D,N); M=arrays['member.u8'].reshape(D,N)
assert (sessions>0).all() and (np.diff(sessions)>0).all() and (sessions%86400000000000==0).all()
assert ids[0]>0 and (ids[1:]>ids[:-1]).all()
assert ((P==0)|(P==1)).all() and ((M==0)|(M==1)).all()
first,end=m['score_begin'],m['score_end']; recipe=json.loads(m['membership_recipe'])
assert end==D and first==int(np.searchsorted(sessions,m['score_start_ns'])) and sessions[-1]<m['score_end_ns']
assert first>=383 and not M[:63].any(); counts=M.sum(axis=1)
assert np.array_equal(counts[first:end],m['score_member_counts']) and (counts<=recipe['top_n']).all()
assert M.any(axis=0).all()
iso=lambda x:dt.datetime.fromtimestamp(int(x)/1e9,dt.timezone.utc).date().isoformat()
missing=M.astype(bool)&~P.astype(bool); eligible=M.astype(bool)&P.astype(bool)
score_decisions=np.arange(first,end-2); scheduled=score_decisions[(score_decisions-first)%5==0]
def gaps(index):
 entry=eligible[index]&~P[index+1].astype(bool)
 endpoint=eligible[index]&~P[index+2].astype(bool)
 either=entry|endpoint
 return {'decision_dates':len(index),'observed_eligible_name_decisions':int(eligible[index].sum()),
  'missing_entry_d1_name_decisions':int(entry.sum()),'missing_endpoint_d2_name_decisions':int(endpoint.sum()),
  'missing_either_name_decisions':int(either.sum()),'dates_with_any_missing_entry':int(entry.any(axis=1).sum()),
  'dates_with_any_missing_endpoint':int(endpoint.any(axis=1).sum()),'unique_affected_ids':int(either.any(axis=0).sum())}
daily=[]
for d in range(D):
 row={'session':iso(sessions[d]),'member_names':int(counts[d]),'present_names':int(P[d].sum()),'member_current_absent':int(missing[d].sum()),'scored':bool(first<=d<end)}
 if d<end-2:
  row['observed_eligible_missing_d1']=int((eligible[d]&~P[d+1].astype(bool)).sum())
  row['observed_eligible_missing_d2']=int((eligible[d]&~P[d+2].astype(bool)).sum())
 daily.append(row)
receipt={'scope':'read-only pinned manifest+axes+presence/member masks; no prices/returns/performance',
 'manifest_sha256':PIN,'shape':[D,N],'source_sha256_declared':m['source_sha256'],'bindings':bindings,
 'session_first':iso(sessions[0]),'session_last':iso(sessions[-1]),'score_start_declared':iso(m['score_start_ns']),
 'score_end_exclusive_declared':iso(m['score_end_ns']),'score_first_session':iso(sessions[first]),'score_last_session':iso(sessions[end-1]),
 'warmup_dates':first,'initial_member_unready_dates':63,'usable_member_warmup_dates':first-63,'score_dates':end-first,
 'daily_member_counts_match_manifest':True,'stable_union_equals_all_time_member_union':True,
 'warmup_only_member_ids':int((M[:first].any(axis=0)&~M[first:].any(axis=0)).sum()),
 'score_member_min':int(counts[first:].min()),'score_member_max':int(counts[first:].max()),'score_member_mean':float(counts[first:].mean()),
 'member_current_absent_all_name_dates':int(missing.sum()),'member_current_absent_score_name_dates':int(missing[first:].sum()),
 'score_dates_with_member_current_absence':int(missing[first:].any(axis=1).sum()),
 'all_mature_score_decisions':gaps(score_decisions),'scheduled5_mature_score_decisions':gaps(scheduled),
 'limitations':['Future presence QA is diagnostic only and must not alter earlier membership or candidate selection.',
 'Observed eligible means member AND current physical presence; DSL finiteness and nonzero target/held positions were not inspected.',
 'Missing potential entry/endpoint does not prove a particular strategy trade fails; actual zero/capped decisions and held state determine execution.',
 'Mask hashes/metadata verified only; numeric field bytes and empirical prior-ADV arithmetic were intentionally not read.',
 'Common-stock and historical-vintage qualifications remain false.'], 'daily':daily}
p=OUT/'recent-dev-smoke-mask-audit.json';p.write_text(json.dumps(receipt,sort_keys=True,indent=2)+'\n',encoding='utf8',newline='\n')
print(json.dumps({k:v for k,v in receipt.items() if k not in ('daily','limitations','bindings')},indent=2))
print('receipt_sha256',hashlib.sha256(p.read_bytes()).hexdigest())
