"""Correct reviewed I1: bind exact existing appendix3 bytes; preserve failed v12."""
import copy
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
base = Path(__file__).resolve().parent
pins = {
    'readiness-matrix-v12.json': 'ed8590061f6c45f1d09ad5e07f983e118923f925bf22bca870cbc41f25def606',
    'readiness-matrix-p9-al-data-v3.json': 'd37d1e36d4d82b0520a9fa16831e30c0eac1e4f95dc9c430fae52aa8b6d33ba6',
    'p9-al-data-request-v3.md': '8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321',
}
held = {}
for name, digest in pins.items():
    path = base / name
    if path.stat().st_size > 1024 * 1024:
        raise RuntimeError('Unexpected metadata size')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != digest:
        raise RuntimeError('Exact prior/source bytes changed')
    held[name] = raw
old = json.loads(held['readiness-matrix-v12.json'])
appendix = json.loads(held['readiness-matrix-p9-al-data-v3.json'])
main = copy.deepcopy(old)
main.update(schema='atx.tier1-v4.readiness/v13', version=13,
    supersedes={'path': 'readiness-matrix-v12.json', 'sha256': pins['readiness-matrix-v12.json']},
    revision_reason='Correct independent Important I1: authenticate current unchanged appendix3 exact bytes. Failed v12 and every requirement/status remain preserved.')
main['prior_review_record_preserved'] = old['review']
main['review'] = {'status': 'PENDING_INDEPENDENT_I1_REREVIEW_AT_GENERATION',
    'evidence': '.superpowers/sdd/tier1-v4/review-readiness-v13-findings.md',
    'prior_failed_review': '.superpowers/sdd/tier1-v4/review-readiness-v12-findings.md',
    'scope': 'Current exact appendix3 SHA binding; unchanged46 original/23 P9 requirements and all cutoffs/statuses'}
binding = main['additional_required_readiness']
assert binding['path'] == '.superpowers/sdd/tier1-v4/readiness-matrix-p9-al-data-v3.json'
binding['sha256'] = hashlib.sha256(held['readiness-matrix-p9-al-data-v3.json']).hexdigest()
assert appendix['source']['sha256'] == pins['p9-al-data-request-v3.md']
assert binding['requirement_ids'] == [row['id'] for row in appendix['requirements']]
assert main['requirements'] == old['requirements'] and len(main['requirements']) == 46
assert len(appendix['requirements']) == 23
for key in ('release_status', 'cutoff_scope', 'checkpoint', 'status_policy'):
    assert main[key] == old[key]
out = base / 'readiness-matrix-v13.json'
if out.exists():
    raise RuntimeError('Preserve previous output')
for name, raw in held.items():
    if (base / name).read_bytes() != raw:
        raise RuntimeError('Source bytes changed before publication')
with out.open('x', encoding='utf-8') as stream:
    json.dump(main, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': 'METADATA_I1_CORRECTED_NO_GATE_CHANGE',
                  'current_appendix_sha256': binding['sha256'],
                  'original_records_preserved': 46, 'p9_records_preserved': 23,
                  'gate_promotion': False}), flush=True)
