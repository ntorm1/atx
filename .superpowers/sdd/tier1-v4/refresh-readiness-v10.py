"""Correct reviewed current-frontier metadata, preserving rejected v9 evidence."""
import copy
import hashlib
import json
from pathlib import Path

base = Path(__file__).resolve().parent
output = base / 'readiness-matrix-v10.json'
if output.exists():
    raise RuntimeError('Preserve prior readiness version')
prior_bytes = (base / 'readiness-matrix-v9.json').read_bytes()
prior = json.loads(prior_bytes)
inspection_bytes = (base / 't17-interruption-inspection-v5.json').read_bytes()
if hashlib.sha256(inspection_bytes).hexdigest() != '6090f38683aadb68a5409cf850ff339447a2a26db87ac9f0b4a69616cd4aaf95':
    raise RuntimeError('Independent retained frontier evidence changed')
inspection = json.loads(inspection_bytes)
if (prior['checkpoint']['financial_retained_native_admission_files'] != [0,1,2] or
        prior['checkpoint']['financial_batches_admitted'] != 8 or
        prior['checkpoint']['financial_batches_independently_checked'] != list(range(7)) or
        inspection['retained_admission_batch_ids'] != list(range(8))):
    raise RuntimeError('Unexpected reviewed metadata correction boundary')
result = copy.deepcopy(prior)
result.update(schema='atx.tier1-v4.readiness/v10', version=10)
result['checkpoint']['financial_retained_native_admission_files'] = list(range(8))
result['checkpoint']['metadata_review_correction'] = {
    'finding': 'readinessv9 ImportantI1: current-named retained file frontier was inherited as0..2',
    'prior_version_preserved': 'readiness-matrix-v9.json',
    'prior_sha256': hashlib.sha256(prior_bytes).hexdigest(),
    'corrected_current_inspected_frontier': list(range(8)),
    'independently_audited_frontier_unchanged': list(range(7)),
    'gate_promotion': False,
}
assert result['requirements'] == prior['requirements']
assert result['additional_required_readiness'] == prior['additional_required_readiness']
assert len(result['requirements']) == 46
assert result['release_status'] == 'NOT_PRODUCTION_READY'
if (base / 'readiness-matrix-v9.json').read_bytes() != prior_bytes or (base / 't17-interruption-inspection-v5.json').read_bytes() != inspection_bytes:
    raise RuntimeError('Correction evidence changed')
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'old_contracts_and_observations_preserved':46,'p9_appendix_unchanged':True,
                  'retained_files':list(range(8)),'independently_audited':list(range(7)),
                  'rejected_v9_preserved':True,'gate_promotion':False}))
