"""Preserve every requirement while pinning the P9 count/provenance clarification."""
import copy
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
base = Path(__file__).resolve().parent
pins = {
    'readiness-matrix-v11.json': 'b685182b0e42f64b0334ab106d9263090289286f9d13708f35cc4b1ade3b3eaf',
    'readiness-matrix-p9-al-data-v2.json': '50941216df88ad011ee0747a641a008d21b9a7089cd89f5ba9be14380e26958c',
    'p9-al-data-request-v3.md': '8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321',
}
held = {}
for name, digest in pins.items():
    path = base / name
    if path.stat().st_size > 1024 * 1024:
        raise RuntimeError('Unexpected metadata size')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != digest:
        raise RuntimeError('Prior/source metadata pin changed')
    held[name] = raw
old_main = json.loads(held['readiness-matrix-v11.json'])
old_appendix = json.loads(held['readiness-matrix-p9-al-data-v2.json'])
main = copy.deepcopy(old_main)
appendix = copy.deepcopy(old_appendix)
appendix.update(schema='atx.tier1-v4.p9-al-data-readiness/v3', version=3,
    supersedes={'path': 'readiness-matrix-p9-al-data-v2.json', 'sha256': pins['readiness-matrix-p9-al-data-v2.json']},
    interpretation='All23 complete requirement records preserved. Current owner text clarifies counts versus full-row equality and limits of code labels. No threshold, source guard, status, seal or release change.')
appendix['source'] = {'original': old_appendix['source']['original'],
    'snapshot': '.superpowers/sdd/tier1-v4/p9-al-data-request-v3.md',
    'bytes': len(held['p9-al-data-request-v3.md']), 'sha256': pins['p9-al-data-request-v3.md'],
    'previous_snapshot_preserved': old_appendix['source']}
appendix['binding_interpretation_clarifications'] = [
    {'applies_to': ['P9-I01', 'P9-I02', 'P9-I05'], 'requirement': 'Equal seal-2025 counts do not imply equal links. B1 validation equals A and B12 equals B2 because they share rehearsal; B2 can move links without changing those counts. Full D0 and requested row differences remain mandatory.'},
    {'applies_to': ['P9-I01'], 'requirement': 'Alpha code metadata has raw/LF SHA per module and one git_head. Stage I code_version is only a label alongside input hashes; Stage I, backfill and name producers require independent supporting producing-commit evidence.'},
]
main.update(schema='atx.tier1-v4.readiness/v12', version=12,
    supersedes={'path': 'readiness-matrix-v11.json', 'sha256': pins['readiness-matrix-v11.json']},
    revision_reason='Owner-linked P9 request precision clarification; all46 original and23 P9 requirement records unchanged.')
main['prior_review_record_preserved'] = old_main['review']
main['review'] = {'status': 'PENDING_INDEPENDENT_CLARIFICATION_REVIEW_AT_GENERATION',
    'evidence': '.superpowers/sdd/tier1-v4/review-readiness-v12-findings.md',
    'scope': 'Exact46/23 preservation, count-versus-row and code-provenance clarifications, no status promotion'}
main['additional_required_readiness'] = {
    'path': '.superpowers/sdd/tier1-v4/readiness-matrix-p9-al-data-v3.json',
    'requirement_ids': [row['id'] for row in appendix['requirements']],
    'all_applicable_requirements_required_for_goal_completion': True,
    'prior_appendix_preserved': old_main['additional_required_readiness']}
assert main['requirements'] == old_main['requirements'] and len(main['requirements']) == 46
assert appendix['requirements'] == old_appendix['requirements'] and len(appendix['requirements']) == 23
for key in ('release_status', 'cutoff_scope'):
    assert main[key] == old_main[key]
for key in ('release_status', 'return_evaluation_and_complete_label_cutoff_exclusive', 'consumer_trial_bars_unchanged'):
    assert appendix[key] == old_appendix[key]
destinations = [(base / 'readiness-matrix-v12.json', main),
                (base / 'readiness-matrix-p9-al-data-v3.json', appendix)]
if any(path.exists() for path, _ in destinations):
    raise RuntimeError('Preserve previous outputs')
for name, raw in held.items():
    if (base / name).read_bytes() != raw:
        raise RuntimeError('Metadata changed before publication')
for path, data in destinations:
    with path.open('x', encoding='utf-8') as stream:
        json.dump(data, stream, indent=2)
        stream.write('\n')
print(json.dumps({'status': 'METADATA_CLARIFICATION_ONLY', 'original_records_preserved': 46,
                  'p9_records_preserved': 23, 'gate_promotion': False}), flush=True)
