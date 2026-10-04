"""Record successful audit7/creator8 and failed9; preserve every release gate."""
import copy
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
expected = {
    'readiness-matrix-v13.json': '3fc1186b9d5cf0de8dacab7604412dbe471e01dd83abe21ae19a0b201bfbec12',
    'readiness-matrix-p9-al-data-v3.json': 'd37d1e36d4d82b0520a9fa16831e30c0eac1e4f95dc9c430fae52aa8b6d33ba6',
    'p9-al-data-request-v3.md': '8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321',
    't17-financial-native-validation-b0007-v2.json': '091d50a7901abf5afcf5bc3e531a2ea655650a23182be45d40a51eca4d0cdd96',
    't17-financial-native-create-b0008-v1.json': '61285fbf2c346a748bce838d700bd50a6ab26c7bf19c3a6b54ee50c3b76b91bc',
    't17-financial-b0009-failure-inspection-v1.json': '82df3cf5ea14fb911f3c9cb70dcb1505aaae38e7accf307d837995de70d24fc7',
}
held = {}
def get(name):
    path = base/name
    if path.stat().st_size > 1 << 20:
        raise RuntimeError('Bounded metadata exceeded')
    raw = path.read_bytes()
    if name in expected and hashlib.sha256(raw).hexdigest() != expected[name]:
        raise RuntimeError('Independently pinned evidence differs')
    if name in held and raw != held[name]:
        raise RuntimeError('Metadata changed while reading')
    held[name] = raw
    return json.loads(raw)


old = get('readiness-matrix-v13.json')
appendix = get('readiness-matrix-p9-al-data-v3.json')
request = base/'p9-al-data-request-v3.md'
request_raw = request.read_bytes()
if hashlib.sha256(request_raw).hexdigest() != expected[request.name]:
    raise RuntimeError('Current owner request differs')
held[request.name] = request_raw
audit = get('t17-financial-native-validation-b0007-v2.json')
created = get('t17-financial-native-create-b0008-v1.json')
inspection = get('t17-financial-b0009-failure-inspection-v1.json')
for name,script,bid,tag,status,code in (
    ('t17-financial-native-validation-b0007-v2','validate',7,'v2','completed',0),
    ('t17-financial-native-create-b0008-v1','create',8,'v1','completed',0),
    ('t17-financial-native-create-b0009-v1','create',9,'v1','failed',1)):
    g = get('logs/'+name+'.receipt.json')
    command = ['.venv/Scripts/python.exe',f'C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-{script}-v1.py','--batch',str(bid),'--attempt-tag',tag]
    if (g['command'] != command or g['status'] != status or type(g['returncode']) is not int or g['returncode'] != code or
        g['cap_hit'] is not False or g['cap_hit_events'] != 0 or g.get('stop_reason') or
        g['job_limit_gb'] != .8 or g['heavy'] is not True or g['disk']['min_free_gb'] != 40 or
        g['thresholds']['test_override'] is not False or g['thresholds']['test_slot_dir'] is not False):
        raise RuntimeError('Exact original guard evidence mismatch')
if (audit['status'] != 'ONE_RETAINED_FINANCIAL_UNIT_NATIVE_TYPED_REPLAY_VERIFIED' or audit['batch'] != 7 or
    audit['observed_frontier_not_newly_admitted'] != list(range(8)) or
    created['status'] != 'ONE_NATIVE_FINANCIAL_UNIT_DOUBLE_REPLAY_ADMITTED' or created['batch'] != 8 or
    created['observed_frontier'] != list(range(9)) or inspection['prior_frontier'] != list(range(9)) or
    inspection['unchanged_member_count'] != 63 or inspection['new_admissions'] != 0):
    raise RuntimeError('Exact new financial observations differ')
for record in (audit,created,inspection):
    if record['publication'] is not False or record['gate_promotion'] is not False:
        raise RuntimeError('Unexpected promotion')
g = get('logs/t17-financial-b0009-failure-inspection-v1.receipt.json')
if g['status'] != 'completed' or g['returncode'] != 0 or g['cap_hit'] or g.get('stop_reason'):
    raise RuntimeError('Preservation observation guard did not complete')
main = copy.deepcopy(old)
main.update(schema='atx.tier1-v4.readiness/v14',version=14,
    supersedes={'path':'readiness-matrix-v13.json','sha256':expected['readiness-matrix-v13.json']},
    revision_reason='Audit7 passed, new8 double-replay admitted, new9 failed at internal DuckDB256MB during second replay. No gate changed.')
main['prior_checkpoint_record_preserved'] = old['checkpoint']
main['prior_review_record_preserved'] = old['review']
checkpoint = main['checkpoint']
for key in ('metadata_review_correction','r28_revision_observation'):
    checkpoint.pop(key,None)  # Exact original records remain in prior_checkpoint_record_preserved.
checkpoint.update(controller_commit='0f261e01',
    qualification='Nine native admissions0..8 retained and all63 exact unit members preserved after failed9. '
        'Units0..7 have separate successful original typed replay audits; unit8 completed unchanged native double '
        'derivation under a successful guard. Unit9 failed before admission in its second replay at DuckDB244.1MiB; '
        'its first-pass files and failed evidence remain unadmitted. Finalwhole85 and all source/economic/release gates remain open.',
    financial_batches_admitted=9,financial_batches_independently_checked=list(range(8)),
    financial_retained_native_admission_files=list(range(9)),
    financial_additional_admissions_pending_current_replay=[],
    financial_successful_double_replay_created_since_last_separate_audit=[8],
    financial_failed_unadmitted_batch=9,
    actual_data_jobs='NONE_ROOT_AT_CHECKPOINT; CREATOR9_NATIVE_DUCKDB_OOM; NEW_FIXTURES_HELD_BELOW_ORIGINAL40GIB_FLOOR',
    engineering='P9 capsule initial5 and subsequent3 focused nodes passed; later capsule-wide original-input binding '
        'correction awaits its new covering test. Root rows1-10 bounded comparison authored, NOTRUN. Cover original '
        'typed-output fixture passed after preserved precedence failure. SEC wrapper fixture refused before child '
        'for disk34.852GiB. Market/event/public/native source qualification remains incomplete.',
    evidence_pins={name:{'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw)} for name,raw in held.items()
                   if name not in ('readiness-matrix-v13.json','readiness-matrix-p9-al-data-v3.json','p9-al-data-request-v3.md')})
main['review']={'status':'PENDING_INDEPENDENT_REVIEW_AT_GENERATION',
    'evidence':'.superpowers/sdd/tier1-v4/review-readiness-v14-findings.md',
    'scope':'Exact new financial checkpoint; every original46/P9_23 record and release status/threshold unchanged'}
assert main['requirements'] == old['requirements'] and len(main['requirements']) == 46
assert len(appendix['requirements']) == 23
for key in ('release_status','cutoff_scope','status_policy','additional_required_readiness'):
    assert main[key] == old[key]
assert main['additional_required_readiness']['sha256'] == expected['readiness-matrix-p9-al-data-v3.json']
for name,raw in held.items():
    if (base/name).read_bytes() != raw:
        raise RuntimeError('Exact metadata changed before output')
output = base/'readiness-matrix-v14.json'
with output.open('x',encoding='utf-8') as stream:
    json.dump(main,stream,indent=2)
    stream.write('\n')
print(json.dumps({'status':'METADATA_CHECKPOINT_UPDATED_NO_GATE_CHANGE','retained':9,
                  'separately_audited':8,'new_failed_unadmitted':9,'gate_promotion':False,
                  'sha256':hashlib.sha256(output.read_bytes()).hexdigest()}))
