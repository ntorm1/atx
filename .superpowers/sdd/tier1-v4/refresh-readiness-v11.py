"""Add round3 P9 requirements without changing any existing acceptance record."""
import copy
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
pins = {
    'readiness-matrix-v10.json': 'd0f9ee4014787e7e6e21f6dbbc7c7f4a55ea9970de1000ccc426b1c58fe987b1',
    'readiness-matrix-p9-al-data-v1.json': '992833a1a00c7fa781bfb184c2c5d6e9c360026a1c7bca8a8e2471a46a9f195d',
    'p9-al-data-request-v2.md': 'd2182547eaafda33d33c51502c07a4622f36d38fb2ca219fbef4eea95019ab02',
}
inputs = {}
for name, expected in pins.items():
    payload = (base / name).read_bytes()
    if len(payload) > 1024 * 1024 or hashlib.sha256(payload).hexdigest() != expected:
        raise RuntimeError('Exact prior/source metadata changed')
    inputs[name] = payload
main_path = base / 'readiness-matrix-v11.json'
appendix_path = base / 'readiness-matrix-p9-al-data-v2.json'
if main_path.exists() or appendix_path.exists():
    raise RuntimeError('Preserve earlier readiness outputs')
old_main = json.loads(inputs['readiness-matrix-v10.json'])
old_appendix = json.loads(inputs['readiness-matrix-p9-al-data-v1.json'])
additions = json.loads("[[\"P9-I01\",\"A0 first: recover published identity provenance and D0 baseline\",\"Report every requested published code field against dec03454 and the supported build commit; full rebuilt A versus published r4 except created_at, pit and link_table equality and Stage I expected/got/pass. Nonzero reproduction remains reported, never hidden.\",\"A0.3 D0\"],[\"P9-I02\",\"Four isolated rebuilt arms and exact check-only switches\",\"A/B1 share full A rehearsal; B2/B12 share no-ticker reconstruction; every arm retains candidates directory/ticker inputs. Exact S1-S4, same code, all original input/hash guards and fresh isolated directories; export-001 succeeds first attempt. S4 reports failed validation only in diagnostic arms and never authorizes production.\",\"A0.3 arms, switches and procedure\"],[\"P9-I03\",\"Nonvacuous arm sanity and immutable complete source pins\",\"A/B2 backfill positive; B1/B12 zero; A symbol_snapshot_match positive/B2 zero; every name_tier true and validation entry present. Pin preparation receipt, OPS/document review and every original unchanged identity-independent input.\",\"A0.3 sanity and procedure\"],[\"P9-I04\",\"Complete four-way consumer-state comparison\",\"B1/A, B2/A, B12/A headline, B12/B1 clean ticker measure. Exact per-session d22UTC reader predicates through2023-12-29; distinct CIK count/unique CIK/any P/class-status set. Full rows1-6 and requested identifiers with original denominators.\",\"A0.3 consumer view and rows1-6\"],[\"P9-I05\",\"Actual eligible panel cells, row seals and panel-CIK effects\",\"All row7 A-only/B-only/A-total counts for2017-2019 and each2020-2023, correct common/share-class test and panel member flag; full-row rows8/9 separated by availability seal; row10 panel CIK per2018-2023 using exact tier-dependent evidence_at/available_at semantics; row11 sanity per arm.\",\"A0.3 rows7-11\"],[\"P9-I06\",\"Honest attribution and owner-only counts/identifier handoff\",\"Preserve b1/b2/b3 distinction: b2 A-only is not necessarily look-ahead; zero does not clear later-evidence residual or held-fixed sources. D0 precedes B interpretation. Send requested sr_id/cik and affected cells only; annotate existing P9 freeze, no P9 rerun or pin edit.\",\"A0.1 exclusions; A0.3 interpretation; A0.4; OD5-1\"],[\"P9-I07\",\"PIT-a/PIT-c export-local primary and name precedence\",\"Strict/pit primary uses own tiers and links known by d22UTC; name days withheld only by strict links in pit, never backfill. T1 preserves strict X primary and unsplit rows; T2 retains dated name Z under overlapping backfill.\",\"A0.4 PIT-a/PIT-c; A0.5 T1/T2\"],[\"P9-I08\",\"PIT-b complete clock-specific reconstruction and versioned acceptance\",\"Acceptance/dominance/segment rules depend only on items known at clock c; later items create new versions/valid_until without rewriting earlier ones. Exact class-proof T3/T4 including2.5share+.5symbol and b2/b3 fixtures; fixed B12/A zero rows1-4,7,8 with D0/sanity; new bridge/link-table rule versions.\",\"A0.4 PIT-b; A0.5 T3/T4/fixed differential/version\"],[\"P9-A08\",\"R7.2(d): dated company grouping and operating identity\",\"Both company keys and is_operating use independently qualified strict/name links. backfill_cik_on_rank selected_company_keys and excluded_only_is_operating are both0 per rank2018-2023; no certification from tier strings alone. R7.1(a) directory count0 remains mandatory.\",\"A.3 R7.1/R7.2; A.5\"],[\"P9-A09\",\"Preserve measured legacy A.4 failure and apply corrected specification\",\"Old observed baseline remains immutable. New prose correctly distinguishes legacy X missingboth/other3fullonly/asofR2(4,0) from the fixed(2,1) in both runs; every original as-of, extent, exit and schedule assertion still required for both proxy families.\",\"A.4 assertions4-6\"]]")
appendix = copy.deepcopy(old_appendix)
appendix.update(schema='atx.tier1-v4.p9-al-data-readiness/v2', version=2,
    authority='.superpowers/sdd/tier1-v4/task-28-p9-identity-request-revision.md',
    chosen_eligibility_remedy='R7.1(a)_DATED_EVIDENCE_ONLY_AND_R7.2(d)_QUALIFIED_STRICT_NAME_CIK',
    supersedes={'path': 'readiness-matrix-p9-al-data-v1.json', 'sha256': pins['readiness-matrix-p9-al-data-v1.json']},
    interpretation='Full round3 snapshot is binding; preserve all13 original requirement records and add10 required groups. No source/gate promotion. Diagnostic S4 never changes production acceptance. A0 has P9 priority.')
appendix['source'] = {
    'original': old_appendix['source']['original'],
    'snapshot': '.superpowers/sdd/tier1-v4/p9-al-data-request-v2.md',
    'bytes': len(inputs['p9-al-data-request-v2.md']), 'sha256': pins['p9-al-data-request-v2.md'],
    'previous_snapshot_preserved': old_appendix['source'],
}
for ident, requirement, threshold, section in additions:
    appendix['requirements'].append({
        'id': ident, 'priority': 'FIRST_WITHIN_P9' if ident.startswith('P9-I') else 'HIGH',
        'requirement': requirement, 'threshold': threshold, 'source_sections': [section],
        'dependencies': ['R28 pinned originals and diagnostic implementation'] if ident.startswith('P9-I') else ['Task1 repair2/5 and independently qualified dated identity'],
        'status': 'NOT_MEASURED_IMPLEMENTATION_REQUIRED',
        'measurement': {'command': None, 'command_status': 'EXPLICIT_VERSIONED_MEASUREMENT_ENTRYPOINT_TO_IMPLEMENT',
                        'required_checks': [threshold]},
        'evidence_artifacts': [], 'gate_promotion': False,
    })
main = copy.deepcopy(old_main)
main.update(schema='atx.tier1-v4.readiness/v11', version=11,
    supersedes={'path': 'readiness-matrix-v10.json', 'sha256': pins['readiness-matrix-v10.json']},
    revision_reason='Same owner-linked P9 request advanced to round3, adding A0 and R7.2; no original requirement, threshold, observation or status changed.')
main['prior_review_record_preserved'] = old_main['review']
main['review'] = {'status': 'PENDING_INDEPENDENT_R28_SCOPE_REVIEW_AT_GENERATION',
                  'scope': 'Exact original46 and prior13 preservation, complete new round3 scope, source pins and no acceptance promotion',
                  'evidence': '.superpowers/sdd/tier1-v4/review-readiness-v11-r28-findings.md',
                  'historical_v10_review': '.superpowers/sdd/tier1-v4/review-readiness-v9-findings.md'}
main['additional_required_readiness'] = {
    'path': '.superpowers/sdd/tier1-v4/readiness-matrix-p9-al-data-v2.json',
    'requirement_ids': [row['id'] for row in appendix['requirements']],
    'all_applicable_requirements_required_for_goal_completion': True,
    'prior_appendix_preserved': old_main['additional_required_readiness'],
}
main['checkpoint']['r28_revision_observation'] = {
    'original_request_bytes': 59567, 'original_request_sha256': pins['p9-al-data-request-v2.md'],
    'a0_actual_rebuilds_run': False, 'a0_d0_reproduction_measured': False,
    'financial_retained': list(range(8)), 'financial_independently_audited': list(range(7)),
    'host_window_v10': {'completed': True, 'scheduling_condition_met': False,
                       'evidence': 'logs/host-headroom-window-v10.json'},
    'orphan_spill_cleanup_observed_free_gain_bytes': 7451750400,
    'cleanup_evidence': 'logs/orphan-duckdb-spill-applied-v2.json',
    'gate_promotion': False,
}
assert main['requirements'] == old_main['requirements'] and len(main['requirements']) == 46
assert appendix['requirements'][:13] == old_appendix['requirements'] and len(appendix['requirements']) == 23
assert len(set(main['additional_required_readiness']['requirement_ids'])) == 23
assert main['release_status'] == 'NOT_PRODUCTION_READY' and appendix['release_status'] == 'NOT_READY'
assert main['checkpoint']['financial_batches_admitted'] == 8
assert main['checkpoint']['financial_batches_independently_checked'] == list(range(7))
assert appendix['return_evaluation_and_complete_label_cutoff_exclusive'] == '2023-01-01'
for name, payload in inputs.items():
    if (base / name).read_bytes() != payload:
        raise RuntimeError('Frozen metadata changed before output')
appendix_bytes = (json.dumps(appendix, indent=2) + '\n').encode('utf-8')
main['additional_required_readiness']['sha256'] = hashlib.sha256(appendix_bytes).hexdigest()
with appendix_path.open('xb') as stream:
    stream.write(appendix_bytes)
with main_path.open('x', encoding='utf-8') as stream:
    json.dump(main, stream, indent=2)
    stream.write('\n')
print(json.dumps({'original_requirements_unchanged':46, 'original_p9_records_unchanged':13,
                  'new_required_groups':10, 'total_p9_groups':23,
                  'release_status':main['release_status'], 'gate_promotion':False}))
