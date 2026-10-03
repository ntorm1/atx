import json,math
from pathlib import Path
from atx_db.alpha_panel import common as C
artifacts=Path('../.superpowers/sdd/tier1-v4');jobs=[]
for p in (artifacts/'logs').glob('t4*.receipt.json'):
 r=json.loads(p.read_text());cmd=r.get('command',[])
 if 'handcheck' not in cmd:continue
 seconds=float(r.get('run_seconds',0));upper=math.ceil(5*(seconds+1))+1
 jobs.append({'receipt':str(p),'command':cmd,'run_seconds':seconds,'child_pid':r.get('child_pid'),'status':r.get('status'),'returncode':r.get('returncode'),'all_attempt_upper_bound':upper})
root=C.build_root();raw=root.parents[1]/'raw'/'sec_ratios_handcheck';receipts=[]
for p in raw.rglob('receipts.jsonl'):
 rows=[json.loads(x) for x in p.read_text().splitlines() if x.strip()];receipts.append({'path':str(p),'successful_primary_receipts':len(rows),'first':min((x.get('fetched_at','') for x in rows),default=None),'last':max((x.get('fetched_at','') for x in rows),default=None)})
out={'r2_new_sec_http_attempts':0,'non_sec_manual_web_tool_reads':1,'non_sec_manual_backend_hop_count':'not exposed; outside shared SEC budget','exact_historical_task4_all_attempts':None,'historical_upper_bound_recorded_guarded_handchecks':sum(x['all_attempt_upper_bound'] for x in jobs),'bound_method':'Every sec_get HTTP first try/retry/redirect takes a host-wide limiter token; ceiling5/s. Sum ceil(5*(recorded child execution seconds+1))+1 per network job; +1second covers decimal receipt rounding. Includes header, optional index, primary, retries(max6 per hop) and redirects(default requests max30). No uncounted success-only proxy. Bound depends on complete guarded execution inventory; unrecorded historical invocations cannot be proven absent from retained metadata.','historical_exact_count_unavailable_reason':'ATX_SEC_RATE_LOG was not enabled; primary document receipts omit header/index and failed/retried/redirect attempts. No task request-attempt ledger is retained.','guarded_network_jobs':jobs,'successful_document_receipt_sources':receipts,'limiter_source':'atx-db/src/atx_db/sec_http.py: token per attempt, ceiling5/s; SEC_MAX_ATTEMPTS=6; ratios.handcheck source includes header + optional index + primary; R2 audit scripts have no network functions'}
Path('../.superpowers/sdd/tier1-v4/t4-r2-http-budget.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
