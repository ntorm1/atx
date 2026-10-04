"""Record current replay and owner-added acceptance without promoting old gates."""
import copy
import hashlib
import json
from pathlib import Path

base = Path(__file__).resolve().parent
original = json.loads((base / 'readiness-matrix-v6.json').read_text(encoding='utf-8'))
result = copy.deepcopy(original)
result.update(schema='atx.tier1-v4.readiness/v7', version=7, client_date='2026-10-04')
result['checkpoint'].update(
    controller_commit='08ae493a',
    qualification='Three retained financial units passed current native typed replay and independently bound root audit; no final financial/source-economic/release gate passes. P9 owner request is additional required work.',
    financial_batches_admitted=3,
    financial_batches_independently_checked=[0, 1, 2],
    financial_additional_admissions_pending_current_replay=[],
    actual_data_jobs='RUNNING_NATIVE_FINANCIAL_CONTINUATION_SESSION86809_MAX_NEW4; ONLY_UNITS0_1_2_INDEPENDENTLY_CHECKED',
    engineering='Task15 R1 approved; Task17 R26 repair2 active. Native family75efb231 and notes1607b771 await combined independent review. Short and ownership exception helpers active. P9 indexes correction queued in existing Task1 repair2/5.',
)
record = {
    'artifact': '.superpowers/sdd/tier1-v4/t17-financial-progress-audit-v4.json',
    'supporting_artifacts': [
        '.superpowers/sdd/tier1-v4/logs/t17-runtime-replay-b0000-b0002-v1.receipt.json',
        '.superpowers/sdd/tier1-v4/logs/t17-financial-progress-audit-v4.receipt.json',
        '.superpowers/sdd/tier1-v4/review-financial-replay-plan-v4.md',
    ],
    'result': 'Successful unchanged R23 max-new-batches0 rederived and typed-compared retained0/1/2 in228.7s/599658496peakbytes, no cap. Root audit26.1s/150372352bytes binds each unit/admission/start/receipt/payload to independently pinned pre-replay inspection, exact recovery and successful replay, checks current frozen sources and complete admission frontier twice. Three of85 financial units now independently checked; no new unit created, final manifest/pointer absent and original economic/coverage/release gates open.',
    'checkpoint_version': 7, 'gate_promotion': False,
}
for row in result['requirements']:
    if row['id'] in {'D01', 'D06', 'D07', 'C03', 'C04', 'O01', 'O03', 'O04'}:
        row.setdefault('observations', []).append(copy.deepcopy(record))
assert len(result['requirements']) == len(original['requirements']) == 46
for old, new in zip(original['requirements'], result['requirements'], strict=True):
    assert {k: v for k, v in old.items() if k != 'observations'} == {k: v for k, v in new.items() if k != 'observations'}
    assert new.get('observations', [])[:len(old.get('observations', []))] == old.get('observations', [])

request = base / 'p9-al-data-request-v1.md'
request_sha = hashlib.sha256(request.read_bytes()).hexdigest()
assert request.stat().st_size == 23418 and request_sha == '1be5500839948b4e987075349959458e0e675b6aee04f9d4e364d40aa0b98adc'

def requirement(identifier, priority, description, threshold, source_sections, dependencies, checks):
    return {'id': identifier, 'priority': priority, 'requirement': description,
            'threshold': threshold, 'source_sections': source_sections,
            'dependencies': dependencies, 'status': 'NOT_MEASURED_IMPLEMENTATION_REQUIRED',
            'measurement': {'command': None, 'command_status': 'EXPLICIT_VERSIONED_MEASUREMENT_ENTRYPOINT_TO_IMPLEMENT', 'required_checks': checks},
            'evidence_artifacts': [], 'gate_promotion': False}

appendix = {
    'schema': 'atx.tier1-v4.p9-al-data-readiness/v1', 'version': 1,
    'source': {'original': 'C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/atx-db-request.md',
               'snapshot': '.superpowers/sdd/tier1-v4/p9-al-data-request-v1.md', 'bytes': 23418, 'sha256': request_sha},
    'authority': '.superpowers/sdd/tier1-v4/task-27-p9-al-data-goal-addition.md',
    'complete_request_is_binding': True,
    'release_status': 'NOT_READY', 'chosen_eligibility_remedy': 'R7(a)_DATED_EVIDENCE_ONLY',
    'return_evaluation_and_complete_label_cutoff_exclusive': '2023-01-01',
    'b2_form_adv': 'DEFERRED_NOT_IN_SCOPE',
    'consumer_trial_bars_unchanged': True,
    'requirements': [
        requirement('P9-A01', 'HIGH', 'Complete immutable rank-date selection vintages for both proxy families', 'Zero rows or values selected using post-rank facts; every selected member retained', ['A.3 R1', 'A.4 assertions1,4,6'], ['Task1 indexes', 'dated source inputs'], ['Exact planted sid7 presence', 'Full versus censored as-of equality at all five cuts', 'Russell and SP500P']),
        requirement('P9-A02', 'HIGH', 'Schedule-derived effective bounds independent of vendor extent', 'R1/R2/R3 scheduled ends2021-06-25/2022-06-24/2023-06-23 or permitted open vintages; identical rank rows on both data extents', ['A.3 R2', 'A.4 assertions2,5,6'], ['Task1 indexes', 'exchange calendar', 'schedule'], ['Both vendor end dates', 'No last_session in rank-vintage ends', 'Last reconstitution also stable']),
        requirement('P9-A03', 'HIGH', 'Separate known exit vintages', 'X/W/Z exits exactly2021-06-02/2021-06-11/2021-09-16 at22:00UTC; aliveV/Y none; latest strictly available vintage', ['A.3 R3', 'A.4 assertions3,4,6'], ['Task1 indexes', 'authenticated exit evidence/calendar'], ['No exit from non-NULL last_session alone', 'Unknown exits excluded with reasons']),
        requirement('P9-A04', 'HIGH', 'PIT summary rows and registered summary clock', 'Zero dependence on exits not known at rank; recon_summary clock=available_at; later realised aggregates in new vintage rows', ['A.3 R4', 'A.4 assertion4'], ['Task1 indexes/lake metadata'], ['R2 additions/deletions2/1 at planted cut', 'Turnover equivalence', 'No unclocked summary serving']),
        requirement('P9-A05', 'HIGH', 'Daily membership built only from then-available selections/exits', 'available_at=max(rank mark, prior-session22:00UTC); only vintages stamped at or before row stamp', ['A.3 R5', 'A.4 assertions4,6'], ['Task1 indexes', 'exchange calendar'], ['Complete as-of membership equality', 'Strict downstream read convention preserved']),
        requirement('P9-A06', 'HIGH', 'Dated eligibility removes directory survivor/classification channel', 'directory_decided_on_rank=0 for every rank2018-2023 underR7(a); all decisive flags use qualified dated evidence', ['A.2 channel4', 'A.3 R7', 'A.5'], ['Task1 snapshot', 'dated FINRA or other qualified type/ETF evidence', 'foreign panel read-only'], ['Per-rank eligibility decision counts', 'No directory_type/directory_etf downstream effect', 'Missing evidence exclusions and original coverage gates retained']),
        requirement('P9-A07', 'HIGH', 'Versioned partition-sealed indexes manifests and arrival receipts', 'Only russell-proxy-v2/sp500-proxy-v2; each rank_vintage_rows_on_recon equals counts_on_recon; exit_rows per index; available_at-year file bytes/SHA pins', ['A.3 R6,R8', 'A.5', 'A.6'], ['Task1 publication/manifest', 'P9 consumer handoff'], ['No mixed v1/v2', 'No2024+part read by pre2024 handoff', 'Root manifests/receipts only', 'P9 engine re-pin remains external lane responsibility']),
        requirement('P9-B01', 'LOW', 'B1 original annual and quarterly text cohort and comparator', 'Every specified original10-K-family/10-Q accepted2012-2023; same form/fiscal period prior year, earlier acceptance,180-550day gap; amendments neither current nor prior', ['B1 Ask', 'B1 Columns', 'B1 Comparator', 'B1 Availability'], ['Task15 extension', 'original SEC cohort/body/acceptance'], ['10-Q PartI Item2 and PartII Item1A', 'Full-primary term-frequency doc_sim_cosine using existing tokenizer', 'Every specified identity/prior/reason column', 'Prior clock strictness on every row']),
        requirement('P9-B02', 'LOW', 'B1 measured complete cohort and comparator coverage', 'Per acceptance-year2013-2023 landed/original SEC master-index counts; finite same-period-prior doc_sim coverage>=90%, excluding issuer first-period filings and20-F/40-F only', ['B1 Acceptance(ii)-(iv)', 'B1 History arithmetic'], ['P9-B01', 'original SEC master-index closure'], ['Publish all yearly denominators and exclusions', 'Both originals and strict prior_available_at<available_at', 'No new signal trial/bar override']),
        requirement('P9-B03', 'LOW', 'B3 standardized annual fundamentals history and columns', 'Every requested column/key; fiscal years ending>=2009-01-01 with10-K accepted<2024-01-01; optional2001-2008 remains optional', ['B3 Ask', 'B3 Availability'], ['Task3 export', 'qualified financial generation/vintages'], ['Complete requested xsga/xrd/gdwl/equity/assets/liability/template/currency columns', 'Restatements new vintages, no overwrite', 'Leavers retained']),
        requirement('P9-B04', 'LOW', 'B3 SG&A/R&D coverage and convention evidence', 'Print xsga first-reported nonfinancial coverage forFY2009-2022, joint xsga/xrd share, and original-filing convention evidence for50R&D issuers; convention is reported, not a PASS threshold', ['B3 Acceptance(ii)-(iii)'], ['P9-B03', '50original issuer filings'], ['Do not assume XBRL SG&A includes R&D', 'No invented zero/coverage cutoff']),
        requirement('P9-B05', 'LOW', 'Monthly CPI-U NSA if not already in house', 'BLS CUUR0000SA0 monthly values with authenticated release instants; first determine existing-house availability', ['B3 Ask'], ['Existing-source audit', 'original BLS publication evidence if missing'], ['No reconstructed/fabricated release clock', 'Keep condition explicit']),
        requirement('P9-B06', 'LOW', 'Shared B1/B3 row-clock, population and pinned partition contract', 'C1-C6 all hold: row-complete UTC clock or excluded NULL+reason/count, immutable vintages, exact available_at-year parts, no post2023 selection, complete PinnedStage metadata/files/status, leavers retained', ['B common C1-C6', 'B1 Acceptance(i)', 'B3 Acceptance(i),(iv)'], ['P9-B01', 'P9-B03', 'source clocks', 'export manifests'], ['schema/rule/clock_rule/staleness_days/files bytes+SHA/complete status', 'No part contains other-year stamps', 'P9 reader receives no2024+part', 'All pre2023 evaluation seals unchanged']),
    ],
}
appendix_path = base / 'readiness-matrix-p9-al-data-v1.json'
for path, value in ((appendix_path, appendix), (base / 'readiness-matrix-v7.json', result)):
    if path.exists():
        raise RuntimeError('Preserve prior versioned readiness artifacts')
result['additional_required_readiness'] = [
    {'path': '.superpowers/sdd/tier1-v4/readiness-matrix-p9-al-data-v1.json',
     'requirement_ids': [r['id'] for r in appendix['requirements']], 'all_applicable_requirements_required_for_goal_completion': True}
]
result['authority'].append('.superpowers/sdd/tier1-v4/task-27-p9-al-data-goal-addition.md')
for path, value in ((appendix_path, appendix), (base / 'readiness-matrix-v7.json', result)):
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
print(json.dumps({'old_requirements_preserved': 46, 'new_p9_requirements': len(appendix['requirements']),
                  'independently_checked_financial_units': 3, 'release_status': 'NOT_PRODUCTION_READY'}))
