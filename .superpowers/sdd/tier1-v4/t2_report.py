from pathlib import Path
import json
from atx_db.alpha_panel import common as C
root=C.build_root();art=Path('../.superpowers/sdd/tier1-v4');v=C.read_json(root/'validation/universe.json');a=C.read_json(art/'t2-after-verification.json');r=C.read_json(art/'t2-french-reconciliation.json');b=C.read_json(art/'t2-before-audit.json')
def tab(head,rows):return '\n| '+' | '.join(head)+' |\n|'+ '|'.join(['---']*len(head))+'|\n'+''.join('| '+' | '.join(str(x) for x in row)+' |\n' for row in rows)+'\n'
def guard(script,cap=0.9,module=True):return f".venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb {cap} --wait-minutes 60 -- .venv/Scripts/python.exe {'-m ' if module else ''}{script}"
g=v['gates'];u=C.read_json(root/'universe/manifest.json');market=C.read_json(root/'market/market_index_manifest.json');idx=C.read_json(root/'indexes/manifest.json')
summary=[['daily_band','PASS' if g['daily_band']['pass'] else 'FAIL',json.dumps({y:x['mean'] for y,x in g['daily_band']['per_year'].items()}),guard('atx_db.alpha_panel.universe build')],['French +/-5%','PASS' if g['french_nyse']['pass'] else 'FAIL',f"{g['french_nyse']['months']-g['french_nyse']['months_outside']}/{g['french_nyse']['months']} inside; missing={g['french_nyse']['missing_months']}",guard('atx_db.alpha_panel.universe build')],['R2000 effective companies','PASS' if g['r2000']['pass'] else 'FAIL',json.dumps(g['r2000']['companies']),guard('atx_db.alpha_panel.universe build')],['codes >=99%','PASS' if g['codes_on_all_common']['pass'] else 'FAIL',json.dumps(g['codes_on_all_common']['per_year']),guard('atx_db.alpha_panel.universe build')],['hand precision >=95%','PASS' if g['hand_check']['pass'] else 'FAIL',f"{g['hand_check'].get('correct')}/100; per stratum {g['hand_check'].get('by_stratum')}",guard('atx_db.alpha_panel.universe build')]]
s='''
## Fix round 1 (2026-10-03): Task 2 plus Task 2x dependency

Status: DONE_WITH_CONCERNS. Implementer: Codex, controller-selected gpt-6.1-sol under R1; historical GC6 trailer retained as required, not a claim of an Anthropic runtime. I1, I2, I3 and I5 fixed; I4 remains FAIL with measured residuals requiring controller ruling/remediation. No PIT/accuracy threshold amended. No foreign panel code or stage changed/rebuilt. No shared registry or progress.md edits.

Code commits: `d306e7768f7d72293aa600718a396814c98caba9` (Task 2x), `f0ab20f50a5d9fa4e4e8d0a68b23beeba90f51f4` (Task 2 classifiers/sample binding). Documentation/report commit is listed in the final controller response; use these specific commits, never BASE..HEAD.

- I1: incorporation uses dated cover facts strictly before cutoff; undated profile snapshot removed, earlier state unknown. Later-foreign-snapshot fixture protects history.
- I2: SIC 6770 supplements SPAC only with zero/missing panel sale_ttm, matching established panel behavior. Positive-revenue retained-SIC de-SPAC fixture admitted.
- I3: authoritative SC/BZX mapping and dated NULL-panel exchange fallback in codes, market and index eligibility. Company caps/ranks/bands/buffers/intervals fully recomputed; universe rebuilt after dependency. Task 2x details in task-2x-report.md.
- I5: latest qualifying 814- status event before each cutoff replaces global min-election/max-withdrawal. Withdrawal/re-election/future-withdrawal fixture proves no appended-future revision; withdrawals win a tied event timestamp.
- Also closes validation incompleteness risk: expected French months must exist, missing R2000 year counts fail; precision requires unique keys, matching seed/rule/sample hash. 17 newly selected sample keys manually rechecked against primary SEC descriptions/closing chronology, 83 unchanged identity/stratum claims retain prior judgments. The same-classifier zero-suspect result below is internal consistency, not independent accuracy validation.

### Gates
'''+tab(['criterion','PASS/FAIL','measured','command'],summary)
s+='\nRule `universe-all-common-v2`; sample SHA-256 `'+C.read_json(root/'validation/universe_hand_check_sample.json')['sample_sha256']+'`. ADR remains 9/12 despite pooled 97/100; no recall claim. Actual beta/idiosyncratic-volatility coverage remains Task 6; the 30-field table includes clearly labeled input sufficiency. Annual counts and all three coverage universes still cover 2020-2025.\n'
s+='\n### Built artifacts\n'+tab(['year','rows','bytes'],[(y,n,(root/'universe'/f'year={y}'/'universe.parquet').stat().st_size) for y,n in u['receipt']['rows'].items()])
s+='\n### Month-by-month French reconciliation\n\nBenchmark count unit and +/-5% line-count gate are retained. Company counts are diagnostic only. Exclusive classes partition observed NYSE panel lines in t2-french-reconciliation.json; source coverage counters overlap separately. Zero admitted non-common names under the existing FINRA classifier is not external validation.\n'
s+=tab(['month','French','admitted lines','companies','residual lines','otherwise eligible missing shares','admitted unknown incorporation'],[[str(x['session_date'])[:7],x['french'],x['admitted'],x['companies'],x['residual'],x.get('otherwise_eligible_missing_shares'),x['unknown_inc']] for x in r['monthly']])
s+='\nExact failed months/deviations: `'+json.dumps(g['french_nyse']['outside'],sort_keys=True)+'`.\n'
s+='''
Measured unresolved causes and next work: early negative residuals coexist with dated shares/source-coverage holes among lines otherwise satisfying the rule. Those holes are measured here, but the French aggregate contains no identities, so not every missing candidate is established as a missing benchmark member. Repairing authorized market_shares SEC/vendor coverage is a feasible upstream step; it must use original clocks, not future share backfill. Removal of the newly introduced non-PIT incorporation fallback also exposes early positive residuals in 2018-05..2019-07, when dated incorporation covers are missing. Those cannot be closed by restoring the snapshot; an authorized dated historical incorporation source is a feasible remedy. Later positive residuals persist after internal fixes. Multi-class lines explain only line-minus-company counts; admitted incorporation-unknown counts quantify whether missing dated incorporation could explain excess. No named class reconciliation establishes the remaining counterpart; do not assert licensed CRSP is necessary or that broad counts validate accuracy. Feasible next remediation is an independent dated audit of the residual candidate cohort (US domicile, security/share type, blank-check chronology, filer regimes, vendor/link validity), plus a documented benchmark-definition comparison and issuer-level reconciliation using free source evidence. Aggregate benchmark alone cannot assign a specific falsely admitted line. Controller carry-forward ruling remains pending; manifest FAIL preserved.

### Tests and self-review

`.venv/Scripts/python.exe -m pytest tests/test_universe_stage.py tests/test_exchange_repair.py tests/test_alpha_panel_market.py tests/test_alpha_panel_indexes.py tests/test_alpha_panel_identity_v3.py -q -n0`: 61 passed, offline, under 10 seconds combined. R5 regression injects an extreme 2023 return, proves it never enters source u before aggregation, checks no output period >=2023, and rejects later --end. Covered exchange SC/BZX, unsupported/stale/same-date evidence, codes, market/rank eligibility, cutoff/link/index transitions, retained SIC, snapshots, BDC future append, missing months and stale sample binding. Explicit-path diffs checked; owned code/tests clean after commits. No full suites or held-out evaluation.

### Commands / memory evidence

All real builds and SQL/fetch/build diagnostics use the guard, one at a time in this lane. Fixture tests need no guard. All source updates are local source contracts; no new raw source landing.
'''
commands=[('before audit',guard('../.superpowers/sdd/tier1-v4/t2_audit.py before',0.8,False)),('codes',guard('atx_db.alpha_panel.identity_v3 codes',0.8)),('market R5 final',guard('atx_db.alpha_panel.market_index build --end 2022-12-31')),('indexes',guard('atx_db.alpha_panel.indexes build')),('universe final',guard('atx_db.alpha_panel.universe build')),('French reconciliation',guard('../.superpowers/sdd/tier1-v4/t2_reconcile.py',0.9,False)),('verification',guard('../.superpowers/sdd/tier1-v4/t2_verify.py',0.8,False)),('hand evidence binding',guard('../.superpowers/sdd/tier1-v4/t2_handcheck.py',0.4,False))]
s+=tab(['job','exact command'],commands)
receipts=[]
for stamp in ('20261003T160034-3528','20261003T160212-17664','20261003T160357-6664','20261003T160448-16944','20261003T160607-11508','20261003T160722-15108','20261003T161245-14416','20261003T161717-13484','20261003T162105-14416','20261003T162800-12600','20261003T162956-15928'):
 p=Path('C:/Users/natha/AppData/Local/atx-memory-guard/receipts')/(stamp+'.json')
 if p.exists():
  z=json.loads(p.read_text(encoding='utf-8'));receipts.append([stamp,z['status'],z.get('job_limit_gb'),z.get('native_peak_job_memory_gb'),z.get('run_seconds'),z.get('returncode')])
s+=tab(['guard receipt','status','cap GiB','peak GiB','wall s','exit'],receipts)
s+='\nRaw/stage provenance: original Task 1 commits ee353557, 0c35357e, ff4d829a, 5b773a2f, 3afb9687; original Task 2 b2d3b4705272a01c61d8924283f1431610d9604a, 8791f4cdaf6613f11e157b5f79887a5919a0da3f, bf0ad7faa480aff09cac7540b070cfe6fd52c937. Frozen panel provenance SHA `c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4` unchanged. Baseline full manifests/input hashes in t2-before-audit.json; corrected hashes/code fingerprints in t2-after-verification.json and dataset docs. No return value/evaluation >=2023 inspected. Initial inherited market run published later aggregate-return product rows despite --end; R5 corrected/superseded it, and future factor-return products are now unavailable without authorization.\n'
s+='\nFiles: owned alpha_panel universe/security_master/identity_v3/market_index/indexes modules; universe/exchange regressions; universe/indexes/market_index/share_exchange_history docs and registry stubs; hand judgments; reports and reproducible diagnostics/measurement artifacts in this scratch directory. No new raw data; SEC/FINRA/Nasdaq web pages were primary reference checks, not lake source landings. Existing free FINRA semi-monthly names, SEC filings/covers, vendor price/shares and French monthly benchmark receipts/terms/cadence remain those of original Task 1/2 reports.\n'
p=art/'task-2-report.md';old=p.read_text(encoding='utf-8');p.write_text(old+s,encoding='utf-8')
x='''# Task 2x report: SC/BZX repair inside Task 2 fix round 1

Status: DONE_WITH_CONCERNS. Codes, market_index, indexes and universe rebuilt; no foreign panel code/stage rebuilt. Parent Task 2 French accuracy gate still FAIL, with no waiver. Agent/model selection: Codex, gpt-6.1-sol under controller R1; GC6 historical trailer retained.

Code commit `d306e7768f7d72293aa600718a396814c98caba9`; dependent Task 2 code commit `f0ab20f50a5d9fa4e4e8d0a68b23beeba90f51f4`. Final evidence/docs/report commit listed in final response (specific commits only).

## Canonical mapping and timing

Primary [Nasdaq report contract](https://www.nasdaqtrader.com/content/home/help/tsiqtxtkey.pdf) defines historical SC as Nasdaq Capital Market. [FINRA Regulatory Notice 19-33](https://www.finra.org/sites/default/files/2019-10/Regulatory-Notice-19-33.pdf) identifies H as Cboe BZX, and [SEC exchange directory](https://www.sec.gov/about/divisions-offices/division-trading-markets/national-securities-exchanges) identifies Cboe BZX as formerly Bats BZX. The existing warehouse uses XNAS for Nasdaq tiers and BATS for BZX; those MIC conventions are preserved. Source field remains FINRA marketClassCode, not a trading venue or unconditional fallback.

Shared EXCHANGE_BY_CLASS now includes SC -> XNAS, BZX -> BATS (and existing universe IEX repair centrally). Consumers replace only NULL panel exchange from source dissemination_date < session/rank date and <=45-day freshness. Unsupported/stale/source-absent values stay unknown. Existing finra_names.exchange and panel.exchange remain frozen; repair reads market_class directly. Codes preserve first-session stamp and T1-b qualification. Market/index/universe input hashes include source manifests and source-code mapping fingerprints.

Rank computations rerun full eligible company ME aggregation, Russell ranks/bands and S&P buffers; no names were added to previously computed ranks. No return evaluation after 2022.

## R5 inherited-builder issue and correction

Initial `market_index build --end 2022-12-31` at 16:03:57Z computed/published aggregate-return product rows through 2026 because --end only bounded checks. Reported to controller; R5 explicitly forbids portfolio-return products after 2022. Corrected load_universe filters rows before aggregation and rejects later end, then republished at 16:06:07Z. Current output has 5,122 rows and no periods >=2023. Original pre-repair artifact metadata is preserved in t2-before-audit.json; the initial interim run is identified by command/time/guard receipt and 9,022 output rows. Its interim output SHA was not captured before supersession; no interim return values were inspected. No post-2022 return values were inspected, compared or tuned. Downstream Task 6 factor-return products cannot extend beyond 2022 without new authorization; membership and value/presence coverage continue through 2026.

## Gates
'''
x+=tab(['criterion','PASS/FAIL','measurement','command'],[['market replication (3 gates)','PASS' if all(market['gate_status']['per_gate'].values()) else 'FAIL',json.dumps(market['gate_status']['measured']),guard('atx_db.alpha_panel.market_index build --end 2022-12-31')],['indexes R2000 recon count','PASS' if idx['gate_status']['pass'] else 'FAIL',json.dumps(idx['gate_status']),guard('atx_db.alpha_panel.indexes build')]]+summary)
x+='\nInherited combined gates: 8 PASS / 1 FAIL (French); technical verifications below are additional evidence.\n'
x+='\n## Before/after exchange source classes\n\nBaseline panel NULLs are intentionally unchanged. Before source-SC membership exclusion follows old NULL panel-exchange eligibility; original Task 2 report records zero SC R2000 representation. After publication counts are measured from repaired codes/universe flags, with dated source classes on each session. Counts include all observed lines, not just eligible common.\n'
rows=[]
for y,rec in a['per_year'].items():
 before={z[0]:z for z in b[y]}
 for z in rec['exchange_classes']:
  k=z[0]
  if k in ('SC','BZX','UNKNOWN','IEX'):
   prev=before.get(k,[k,0,0,0,0]);rows.append([y,k,prev[1],prev[2],prev[3],prev[4],z[3],z[4],z[5],z[6]])
x+=tab(['year','class','source cells','distinct lines','panel NULL before/after','fresh NULL candidates','published exchcd NULL after','all_common cells after','R2000 cells after','R3000 cells after'],rows)
x+='\n## SC representation at rank-date recomputed R2000 reconstitutions\n'+tab(['effective session','SC lines','SC companies'],a['r2000_sc_recons'])
x+='\n## Keys, PIT and alignment\n'+tab(['artifact','rows','distinct keys','bytes'],[[rel,*z['rows_keys'],z['bytes']] for rel,z in a.items() if isinstance(z,dict) and 'rows_keys' in z])
x+=f"\nCodes cutoff violations {a['codes_pit_bad']}; interval overlaps {a['codes_overlap']}; index/universe alignment mismatch per year "+json.dumps({y:z.get('index_alignment_bad') for y,z in a['per_year'].items()})+'.\n'
x+='\nUniverse row/key evidence: '+json.dumps({y:z['universe_keys'] for y,z in a['per_year'].items()})+'.\n'
x+='\n## Commands, test evidence and provenance\n'+tab(['job','exact command'],commands)+tab(['guard receipt','status','cap GiB','peak GiB','wall s','exit'],receipts)
x+='\nTests: 61 passed offline in <10 seconds via the exact pytest command in task-2-report.md Fix round 1. R5 source-filter/output/rejection tests, SC/BZX and unsupported/stale/same-day regressions, market and index eligibility, universe cutoff transitions, identity clocks and issuer future append cases all covered.\n'
x+='\nSources: no new raw lake landing, zero downloaded source-data bytes. Mapping verification used primary Nasdaq/FINRA/SEC contracts linked above; existing FINRA semi-monthly and SEC filing/covers/vendor shares/French landings and their receipt/terms/rate-limit provenance are inherited from Task 1/2 reports. Historical original commits, frozen panel SHA and all before/after input hashes/code fingerprints are recorded in task-2-report.md Fix round 1, t2-before-audit.json and t2-after-verification.json. Guard scratch files are file-backed spill, not foreign panel outputs.\n'
x+='\nResiduals: French FAIL remains unresolved; exact failed months and 60-month exclusive-class/source-coverage reconciliation are in task-2-report.md and t2-french-reconciliation.json. Same-classifier checks are internal consistency only. No claim official-list overlap passes (licensed-source GAP remains), no price/share/link/unknown-exchange coverage invented. Precision retains visible ADR 9/12. Documentation/stubs reflect current product bounds and all source inputs; shared registries untouched.\n'
(art/'task-2x-report.md').write_text(x,encoding='utf-8')
print('universe gates', {k:z['pass'] for k,z in g.items()}, 'French failed',g['french_nyse']['outside'])
print('daily means',{y:z['mean'] for y,z in g['daily_band']['per_year'].items()})
print('SC recons',a['r2000_sc_recons'])
print('otherwise eligible missing shares',[(str(z['session_date'])[:7],z.get('otherwise_eligible_missing_shares'),z['residual']) for z in r['monthly'] if z['residual']<0])
