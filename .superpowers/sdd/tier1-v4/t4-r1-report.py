import json, shutil
from pathlib import Path
from atx_db.alpha_panel import common as C, ratios as R
root=C.build_root(); out=root/'ratios'; docs=Path('docs/datasets'); artifacts=Path('../.superpowers/sdd/tier1-v4')
v=C.read_json(out/'validation.json'); m=C.read_json(out/'manifest.json')
extra=C.read_json(artifacts/'t4-r1-extra-sourcecheck.json')
hashv=v.get('universe_manifest_sha256'); src=C.sha256_file(root/'compustat_std'/'manifest.json')
status={'is_latest_universe':False,'gc3':'PENDING controller final publication and output-integrity verification','overlap_coverage':'provisional/invalid for final acceptance: Task2 partial publication overlapped validation','measured_manifest_sha256':hashv,'manifest_hash_alone_certifies_output_integrity':False}
coverage={k:x for k,x in v.items() if k.startswith('coverage')}
C.write_json_atomic(docs/'ratios.coverage.json',{'input_sha256':src,'universe_sha256':hashv,'snapshot_status':status,'coverage':coverage,'historical_primitive_coverage_evidence':'t4-evidence.json; not refreshed in R1'})
C.write_json_atomic(docs/'ratios.gates.json',dict(v,snapshot_status=status,supplementary_sourcecheck=extra))
mp=C.read_json(docs/'ratios.requirements.json'); mp['coverage_snapshot_status']=status;mp['input_manifest_sha256']=src
mp['producer_needs']['annual_duration_certification']=['Task5/compustat_std: source-backed period_start/end or verified full-year/comparable-period flag, with source available_at; adjacent fiscal-year end dates alone are not duration certification']
mp['policy']['coverage']='Missing history, absent annual records, ordinary missing primitives and stale/missing states remain eligible. Only explicit sector applicability and true zero denominators exclude; history insufficiency is ratio_history_missing.'
mp['policy']['identity_classification']='Identity available_at and latest dated FINRA observation strictly before common cutoff: prior calendar session22UTC daily; month-end22UTC monthly. No all-time modal FINRA filter.'
mp['policy']['fiscal_pairing']='Quarter lag must match 4*(fyearq0-fyearq)+fqtr0-fqtr and same fyr as well as elapsed-day tolerance. Annual lag requires fyear progression/same fyr and preceding end gap350..380; end-date screen is not source duration certification, missing/transition paths NULL.'
mp['sourcecheck_scope']={'main_primary':33,'main_extensions':3,'supplementary_primary':2,'unverified_primary':31,'unverified_composites':4,'both_absent_is_certification':False}
C.write_json_atomic(docs/'ratios.requirements.json',mp)
stub=C.read_json(docs/'ratios.registry.json');stub['asof_macro_sql']=R.macros_sql();C.write_json_atomic(docs/'ratios.registry.json',stub)
receipts={}
for name in ('build','validate','handcheck','extra-sourcecheck','final-build','final-handcheck','attribution-2','source-cases','final-attribution'):
 path=artifacts/'logs'/f't4-r1-{name}.receipt.json';receipt=C.read_json(path);receipts[name]=receipt
 shutil.copyfile(path,artifacts/'receipts'/path.name)
summary={'code_matches_manifest':C.sha256_file(Path(R.__file__))==m['code']['ratios.py']['sha256'],'input_matches_manifest':src==m['input_manifests_sha256']['compustat_std'],'input_sha256':src,'coverage_snapshot_status':status,'gates':m.get('gate_status'),'coverage_roles':{k:x.get('by_role') for k,x in coverage.items()},'handcheck':{k:x for k,x in v.get('handcheck',{}).items() if k!='checks'},'supplementary_counts':extra['counts'],'one_sided_evidence':'t4-r1-hand-attribution.json: keyed source primitive absence vs retained state; narrow extraction absence does not establish upstream corruption','receipt_summary':{k:{a:x.get(a) for a in ('status','returncode','cap_hit','native_peak_job_memory_gb','run_seconds')} for k,x in receipts.items()}}
C.write_json_atomic(artifacts/'t4-r1-evidence.json',summary)
text="""

## Fix round 1 ? review-T4-r0

R1 addresses I1/I2/I3 locally; I4 remains open. 26 focused offline tests PASS. Missing retained history/FY0 is now in the coverage denominator and separately exposed in `ratio_history_missing`; only genuine zero denominators and explicit sector applicability exclude cells. Empty structural-mask mappings produce valid SQL. Observed missing-input wording no longer calls absence structural.

Identity links and latest dated FINRA classification require `available_at < cutoff`, using strict prior-calendar-session22UTC daily and month-end22UTC monthly. Same-session newly known links cannot select earlier fundamentals. No all-time modal FINRA classification participates in eligibility. Missing dated FINRA evidence remains unknown, governed by the visible common-link contract; this is not independent historical class certification.

Quarter pairing requires fiscal-quarter progression, fiscal-year consistency and unchanged fiscal calendar as well as elapsed-day tolerance. Annual pairing requires fiscal-year progression, unchanged calendar, and preceding end dates350?380days apart. Tests retain raw annual rows while excluding a335day comparative and a fiscal-calendar transition from annual history pairing. Rejected annual history makes annual/fallback ratios NULL; missing history remains eligible. The standardized producer does not expose period start/duration: this end-date screen is **not source-backed full-period duration certification**. Task5 must publish dated period start/end or a verified full-period/comparability flag. Missing certification remains an explicit consumer qualification, not an equivalence claim.

Ohlson still uses nominal assets/OCF proxy; complete formula/composite source-model certification remains unverified. No hard gate is waived. Main independent arithmetic covers33primary+3extensions; two additional existing-primitive formulas (`aftret_equity`, `profit_lct`) are checked separately by t4-r1-extra-sourcecheck.py.31primary and4composites remain unverified. Both-absent cells never certify formulas. No broader source discovery was performed.

The guarded R1 rebuild/validate/handcheck/extra-sourcecheck commands use `run_memory_guarded.py --job-gb`1.0/.8/.6/.6 respectively, receipt/stdout/stderr paths `logs/t4-r1-{build,validate,handcheck,extra-sourcecheck}.*`, with child commands `-m atx_db.alpha_panel.ratios build --buckets32`, `validate`, `handcheck --n20`, and `../.superpowers/sdd/tier1-v4/t4-r1-extra-sourcecheck.py`. Working directory C:/atx/atx-db; PYTHONPATH=C:/atx/atx-db/src; OPENBLAS_NUM_THREADS=1. Offline command from C:/atx: `atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' atx-db/tests/test_alpha_panel_ratios.py -q`;26passed in19.44s.

GC3 remains pending. R1 validation overlapped Task2 partial output publication: its all-common/R3000 measurements are **provisional/invalid for final acceptance**, regardless of recorded manifest hash. The controller must refresh against completed final output-integrity hashes. Earlier33a15585 measurements remain historical; neither snapshot is relabeled latest.

Exact latest evidence follows (t4-r1-evidence.json); previous sections describe the pre-review snapshot:

"""
text+='\nThe four previously mismatching issuer-date cash cases were local independent-extraction defects: add actually reported MarketableSecuritiesCurrent or use explicitly reported CashCashEquivalentsAndShortTermInvestments. Retained t4-r1-source-cases.json records exact facts (no source-value fitting); the final module fixes these chains and fixtures cover priority/no double counting. Final build preserves source-hash lineage; main sourcecheck was rerun. Coverage/range/correlation formulas were unchanged by this sourcecheck-only correction, so their prior R1 measurements remain applicable with the stated universe invalidity.\n\n'
text+='Task5 source/history gaps and Task13 indicated dividends remain open. Primary coverage is24/66 member; extensions2/3; composites0/4. Correlations remain below.99 and unsupported/range failures remain open. Provisional all-common/R3000 counts19/66 and11/66 are not valid final acceptance evidence. Preliminary attribution execution from C:/atx failed on a relative artifact path (exit1); successful attribution-2 used C:/atx/atx-db.\n\n'
text+='\nRemaining one-sided ROE case: CIK1817358, accession0001817358-22-000039, FY2022-01-29, session2022-03-31. Filing prior TXDITC138.703m is present; eligible prior annual-state TXDITC is NULL. This is an unavailable retained/comparable annual-history path, not a demonstrated arithmetic defect or proof that the issuer primitive is absent. Task5 must reconcile source-history/period certification; no current-only average or tax plug is introduced.297 narrow-chain source absences remain uncertified. Final main arithmetic:999matches,143both-absent,297source-absent,1ratio-absent,0numerical mismatches; supplementary80cells:59matches,6both-absent,15source-absent.\n'
text+='```json\n'
text+=json.dumps(summary,indent=2,default=str)+'\n```\n'
for path in (artifacts/'task-4-report.md',docs/'ratios.md'):
 old=path.read_text(encoding='utf-8').split('\n\n## Fix round 1')[0]
 path.write_text(old+text,encoding='utf-8')
print(json.dumps(summary,indent=2,default=str))
