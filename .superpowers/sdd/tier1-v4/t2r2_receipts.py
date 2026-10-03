"""Collect persisted own guard receipts, including failed and superseded runs."""
from pathlib import Path
import os,json
from atx_db.alpha_panel import common as C
directory=Path(os.environ['LOCALAPPDATA'])/'atx-memory-guard/receipts';rows=[]
for p in sorted(directory.glob('20261003T*.json')):
    if p.name<'20261003T164900':continue
    d=json.loads(p.read_text(encoding='utf-8'));cmd=d.get('command',[]);s=' '.join(cmd)
    if not any(x in s for x in ('t2r2_','atx_db.alpha_panel.issuer_history','atx_db.alpha_panel.issuer_classes','atx_db.alpha_panel.universe build')):continue
    rows.append({'receipt':str(p),'receipt_sha256':C.sha256_file(p),**{k:d.get(k) for k in ('command','status','job_limit_gb','native_peak_job_memory_gb','run_seconds','returncode','cap_hit','guard_pid','child_pid')},'wait_seconds':d.get('admission',{}).get('wait_seconds')})
C.write_json_atomic(Path('../.superpowers/sdd/tier1-v4/t2r2-guard-receipts.json'),{'rows':rows,'limits':'Persisted own matching round2 command receipts; failed/stopped/superseded runs are retained. Foreign guard workers excluded. Metadata collection only, no dataset SQL, build, HTTP or return values.'});print('guard receipts',len(rows))
