"""Version release dependencies/evidence without changing acceptance criteria."""
import copy
import json
from pathlib import Path

workspace = Path(__file__).resolve().parent
old = json.loads((workspace / 'readiness-matrix-v2.json').read_text())
out = workspace / 'readiness-matrix-v3.json'
if out.exists():
    raise RuntimeError('Preserve prior matrix version')
new = copy.deepcopy(old)
new.update(schema='atx.tier1-v4.readiness/v3', version=3,
           supersedes='.superpowers/sdd/tier1-v4/readiness-matrix-v2.json',
           review={'status': 'PENDING_SCOPED_REVIEW', 'scope': 'Dependency/measurement/evidence update; all original thresholds and populations preserved.'},
           revision_reason='R15/R17 independently authenticated candidate generations replace unsafe native cache admission; current code reviews and partial operational evidence are distinct from final acceptance.')
new['authority'] += ['.superpowers/sdd/tier1-v4/task-2-cap-ruling.md',
                     '.superpowers/sdd/tier1-v4/task-5-release-admission-ruling.md',
                     '.superpowers/sdd/tier1-v4/task-11-cohort-ruling.md']
new['workflows']['tier1_v4']['chain'] = [
    'T2 native classification through T16 universe_release_v1 complete generation',
    'T5 source-backed v11 through T16 fundamentals_release_v1 explicit generation/fundamentals_v11 leaf',
    'T3 compustat_std_v11 from explicit validated fundamental generation',
    'T4 ratios_v11 from explicit final standardized stage', 'T6 factors/risk',
    'T7 char_monthly', 'T9-15 qualified sources',
    'T16 golden_panel/evaluation/export', 'T17 catalog/qualification']
new['refresh_order'] = [
    'Complete reviewed universe_release_v1, full source/code/key/counter/gate validation, then final GC3 evidence',
    'Complete independent fundamentals_release_v1 generation; explicitly bind v11 leaf before standardized/ratio v11 rebuilds and Task6/7',
    'Task9 final rdq binds final standardized v11; Task11 R16 PIT population and original-clock lineage remain mandatory',
    'Task16 verifies complete quiescent actual source/output identities before build, publication and consumer use',
    'Task17 final catalog/consumer/determinism/restart/rollback and whole-branch review']
rows = {row['id']: row for row in new['requirements']}
universe_command = 'python ../.superpowers/sdd/tier1-v4/t16-validate-generation-v1.py --generation <explicit-generation> --manifest-sha256 <independently-pinned-manifest-sha256>'
for rid in ('D04', 'C01', 'C02'):
    row = rows[rid]
    row['historical_measurement'] = copy.deepcopy(row['measurement'])
    row['measurement']['command'] = universe_command
    row['measurement']['command_status'] = 'DEFINED_FOR_COMPLETE_EXPLICIT_PINNED_GENERATION; PARTIAL_CANDIDATE_NOT_READY'
    row['evidence_artifact'] = 'universe_release_v1/generations/<pinned-generation>/validation.json (complete candidate required)'
    if 'T16' not in row['dependencies']:
        row['dependencies'].append('T16')
    row['note'] += ' R15: native generic cache remains unqualified. The explicit wrapper requires caller-pinned generation/manifest SHA before full validation and rechecks afterward; native CLI validate alone selects current.json and its --generation is unused. First2018unit is partial operational evidence only.'
rows['D03']['note'] = 'Task2 local repair capped at5 with source-identity/key defects carried to R15; independent classification source quality remains unestablished. Current source code review is distinct from original-source certification.'
rows['D01']['observations'] = [
    {'artifact': '.superpowers/sdd/tier1-v4/task16-reader-byte-audit-v1.json',
     'result': '161 files/7,046,096,278 bytes and13manifest identities, two equal actual-byte passes, zero mismatches; retained bytes only, not whole-release quiescence/clock/economic qualification.'},
    {'artifact': '.superpowers/sdd/tier1-v4/task16-unit-2018-inspection-v1.json',
     'result': '2,006,722 keys/251sessions; build checked exact schema/keys/clocks and native counters. One of9years; no finalmanifest/pointer/gates.'}]
for rid in ('D06', 'D09', 'C04'):
    row = rows[rid]
    row['historical_measurement'] = copy.deepcopy(row['measurement'])
    row['measurement']['command'] = None
    row['measurement']['command_status'] = 'R17_GENERATION_VALIDATION_ENTRYPOINT_UNDER_IMPLEMENTATION; LEGACY_DEFAULT_NOT_FINAL_ACCEPTANCE'
    row['note'] += ' R17: require explicit fully validated fundamentals_release_v1 generation with semantic and physical fundamentals_v11 leaf. Legacy0000/0001 parts NOT_ADMITTED; final85batch inventory and original financial gates remain required.'
    if 'T16' not in row['dependencies']:
        row['dependencies'].append('T16')
for rid in ('D07', 'D08', 'D10', 'C03', 'C05', 'C06', 'C07'):
    rows[rid]['note'] += ' Final standardized manifest must persist explicit validated fundamental generation dependency; ratios must pin final standardized files. Historical/default source paths do not establish release acceptance.'
rows['D05']['observations'] = [{'artifact': '.superpowers/sdd/tier1-v4/review-T6-prebuild-r2-findings.md',
                              'result': 'Scoped code Approved; no actual cap-source certification, final binding, returns or coverage measurements.'}]
rows['C08']['note'] += ' Full Task7 native library implementation started; registry definitions and trials freeze before return measurements, no final source binding yet.'
rows['C12']['note'] += ' R16 separates PIT prior-visible/arrived populations from retrospective archive completeness. Native checkpoint has four open cutoff/source-qualification/lineage findings under repair; submissions pilot138filings is not13Ffilingbronze completeness.'
rows['O01']['observations'] = [
    {'artifact': '.superpowers/sdd/tier1-v4/logs/t16-universe-year2018-v1.receipt.json', 'result': 'Completed464.5s/.4713GiB under.8cap; only first candidate year.'},
    {'artifact': '.superpowers/sdd/tier1-v4/logs/t5-new-batch-0001-v2.receipt.json', 'result': 'Completed97.4s/.5538GiB under.8cap; new native batch remains NOT_ADMITTED.'}]
rows['O03']['observations'] = [{'artifact': '.superpowers/sdd/tier1-v4/review-T16-foundation-r1-findings.md',
                              'result': 'Scoped universe pointer/journal/source/contract/counter fixtures Approved; full actual release fault-injection and final acceptance pending.'}]
rows['O04']['note'] += ' First reviewed universe unit completed; guarded next-year continuation is in progress. No complete-release restart/rollback acceptance yet.'
assert len(rows) == len(old['requirements']) == 46
for before, after in zip(old['requirements'], new['requirements']):
    assert before['id'] == after['id']
    for key in ('category', 'requirement', 'threshold', 'status'):
        assert before[key] == after[key], (before['id'], key)
assert new['release_status'] == 'NOT_PRODUCTION_READY'
out.write_text(json.dumps(new, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'version': 3, 'requirements': len(rows), 'thresholds_and_statuses_unchanged': True, 'review': 'PENDING', 'path': str(out)}))
