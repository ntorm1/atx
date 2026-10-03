"""Append measured round2 contract after successful publication; guard required."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C
a=Path('../.superpowers/sdd/tier1-v4')
read=lambda n:json.loads((a/n).read_text(encoding='utf-8'))
cov=C.read_json(C.stage_dir('fundamentals_notes')/'coverage.json')
ev=read('t14-fix-r2-evidence.json'); pilot=read('t14-fix-r2-pilot-summary.json')
pres=read('t14-fix-r2-preservation-after.json'); causes=read('t14-fix-r2-causal-summary.json')
acq=read('t14-fix-r2-acquisition-estimate.json')
p=a/'task-14-report.md'; old=p.read_text(encoding='utf-8')
old=old.replace('Current status after Fix round 1:', 'Current status after Fix round 2:').replace('superseded by the Fix round 1 section','superseded by the latest Fix round 2 section')
ratios=[cov['segments'][str(y)]['share_reconciled'] for y in range(2020,2026)]
first_end=old.index('\n\nStatus:')
old=old[:first_end].replace('81.00%-84.95%',f'{min(ratios):.2%}-{max(ratios):.2%}')+old[first_end:]
new='''
## Fix round 2 (authoritative current publication)

Review `review-T14-r1-findings.md` read verbatim. N1 addressed; retained I4 and independent denominator certification remain unmet. Current binding status **0 PASS / 2 FAIL**, with lower verified figures preserved. No numerical hierarchy inference, proxy waiver, threshold relaxation, foreign edits, progress edits or subagents.

Implementation commits **1b1856c2** and **1fae76ef** (accession-scope correction) preserves the census for every republished existing partition when item extraction is restricted. Offline two-year build then one-year rebuild fixture preserves omitted census, count, HHI and gate denominator. Targeted regression red: 1 failed in 3.02s; green: 1 passed in 2.28s. Final covering command `.venv/Scripts/python.exe -m pytest -o addopts='' --run-slow tests/test_alpha_panel_notes.py tests/test_alpha_panel_notes_fund.py -q`: **26 passed in 34.72s**, exit 0, final completed session 93481. `t14-fix-r2-tests.txt` records actual final tool output.

Accession-scoped primary-evidenced ReportableLegalEntities and ReportableGeographicalComponents exceptions supplement generally admissible OperatingSegments. Primary disclosures corroborate Forian's three reportable segments, Avantor's identical operating/reportable geographic segments, and Smith Douglas's Southeast/Central reportable segments. Members remain unchanged; this is container identity, not an arithmetic-fitted hierarchy. Fixture tests assert direct count/HHI for both identities. Detailed source URLs, response SHA256 and checked raw receipts appear in `t14-fix-r2-pilot-evidence.json`.

### Causal audit and unresolved implementation/source split

Baseline all **3,183** keys audited: **1,945** verified reconciliation misses and **1,238** missing-revenue filings. Compact reproducible index `t14-fix-r2-case-keys.jsonl`; scripts retain detailed raw NUM/DIM evidence locally. Diagnostic classes below describe required evidence, not certified economic causality:

| Diagnostic evidence class | Cases |
|---|---:|
'''
new+=''.join(f'| {k} | {v} |\n' for k,v in causes['cause_counts'].items())
new+='''
The 23 alternate recognized-concept cases include 10 numerical fits, none accepted merely for fitting. Cowen `0001466538-20-000026` explicitly reconciles Economic Income segment revenue to US GAAP with funds/consolidation adjustments; this disproves automatic equivalence. Three of the six context-review cases support the container correction. The other three contain only a ConsolidatedEntities business aggregate; no artificial segment split is inferred.

Martin Marietta `0000950170-23-004361` independently discloses East/West nested under Total Building Materials, plus Magnesia Specialties. This proves at least one source-supported hierarchy exists, but a general filing-role-aware presentation/definition parser and full production structural landing remain **unimplemented**. It is not labeled intrinsic data failure. Exact role relationships must be preserved before member/count correction; a subtotal keyword or subset sum is insufficient. The compact key index identifies this case separately. The 818 cases with no eligible annual revenue fact in retained known concepts need expanded source or definition-equivalence evidence; it is not proven that every one lacks revenue in the filing. Other 3,178 keys beyond the three container cases, one hierarchy case and one non-equivalence case remain **unknown local versus new-source causality**; diagnostic categories do not certify that split. No pilot extrapolation is claimed.

### Authorized primary-disclosure pilot and minimal next acquisition

'''
new+=f"Deterministic selection: all 29 local-review cases plus balanced discrepancy/SIC strata, seed `t14-r2-pilot-1`; **{pilot['processed']}/100 processed**, **{pilot['successful_primary']} successful primary filings**, **{pilot['requests_total']} total HTTP attempts** including retries/redirects, **{pilot['response_bytes']:,} response bytes**, **{pilot['disk_used_bytes']:,} on-disk bytes**. Enforced <=1 attempt/s, <=600 attempts, <=750 MiB response landing, sec_http approved User-Agent/shared limiter, <=0.8 GiB Job Object. Raw receipts `data/raw/sec_notes_t14_pilot/receipts.jsonl`; no raw data committed. Pilot selection, summary and raw SHA checks are persisted. Existing Business cache was consulted first; its corrupt/oversized parts are reported in pilot cache_errors rather than silently treated as complete.\n\n"
new+=f"Primary path: SEC Archives accession primary document for Item 8 segment notes, accession index.json, relevant definition/presentation/calculation linkbases. All 3,183 residual primary URLs are identified; expanded residual acquisition needs 3,183 primary +3,183 index +3,183..9,549 structure GETs before retries, estimated **{acq['document_estimates_median_bytes']['gate_cases']:,} uncompressed primary bytes** using historical median. Production untagged denominator requires independent classification of **35,677** original annual filings; 35,676 primary URLs known, one index resolution needed, median-size estimate **{acq['document_estimates_median_bytes']['all_original_10k']:,} bytes** before compression. Existing 28 historical Notes ZIPs total **12,205,444,547 bytes**, but TAG/PRE/roles were not retained. Minimal next step is bounded role-aware extraction/validation from this pilot, then approval of a scoped full structural/source landing; full corpus was not launched. Preserve C:>=40GB before any >1GB landing. Estimates are cost estimates, not evidence that missing data will resolve every gate.\n\n"
new+='### Latest measured gates and restricted-build integrity\n\n| Year | Multi | Revenue proxy count/share | Verified reconciliation count/share |\n|---|---:|---|---|\n'
for y in range(2020,2026):
 s=cov['segments'][str(y)]
 new+=f"| {y} | {s['multi_segment']} | {s['with_segment_revenue']}/{s['multi_segment']} = {s['share_segment_revenue']:.4%} | {s['reconciled_1pct']}/{s['with_segment_revenue']} = {s['share_reconciled']:.4%} |\n"
new+=f"\nCertified all-filer revenue gate remains FAIL with denominator_certified=false; numeric proxy observations are not certified PASSs. Restricted-year real rebuild recomputed every census row; all-year census digest and omitted 2020-2022 feature/reconciliation digests match baseline: {pres['matches']}; omitted gate counts preserved={pres['omitted_gate_counts_preserved']}. Updated parquet total **{ev['published_bytes']:,} bytes**; full row/size table and GC3 year panels in dataset docs and `t14-fix-r2-evidence.json`. GC3 refreshed at {ev['coverage_remeasured_at']}; actual current input SHA256 {ev['coverage_inputs_sha256']}. No returns computed.\n\n"
new+='''### Exact guarded commands and receipts

From C:/atx/atx-db, PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1. Each command below used `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/RECEIPT -- COMMAND`. One data job at a time in this lane.

| Receipt | Cap | Command after -- | Exit | Seconds | Peak GiB |
|---|---:|---|---:|---:|---:|
'''
jobs=[('t14_fix_r2_causal_audit_r2.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-causal-audit.py'),('t14_fix_r2_acquisition_audit_r2.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-acquisition-audit.py'),('t14_fix_r2_pilot_r3.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-pilot.py'),('t14_fix_r2_preservation_before.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-preservation.py before'),('t14_fix_r2_build.json','.venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build --years 2025'),('t14_fix_r2_preservation_after.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-preservation.py after'),('t14_fix_r2_coverage.json','.venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes coverage'),('t14_fix_r2_evidence.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-evidence.py'),('t14_fix_r2_integrity.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-integrity.py'),('t14_fix_r2_pilot_evidence.json','.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-pilot-evidence.py')]
for filename,cmd in jobs:
 r=read('receipts/'+filename)
 new+=f"| {filename} | {r['job_limit_gb']} | `{cmd}` | {r['returncode']} | {r['run_seconds']} | {r['native_peak_job_memory_gb']} |\n"
new+='\nAll successful receipts cap_hit=false. Initial causal SQL alias failed before publication; initial acquisition diagnostic exceeded its 0.6 GiB guard and was replaced by a filtered bounded scan at 0.8 GiB. Initial pilot cache scan hit DuckDB allocation limit; second hit a corrupt cached parquet. Both stopped before HTTP, so consumed no SEC attempts. Third reads bounded partitions and records unavailable cache parts. Failed receipts retained. Final output integrity verifies all parquet hashes, code hash, duplicate keys, candidate separation, amendment ancestry and census/recon agreement.\n\nRound2 commits: implementation **1b1856c2** and **1fae76ef**; evidence/docs commit listed in controller final and git log; final report commit identifies itself via git log. Prior Task14 SHAs remain 7ec056a3,146a7766,95a03266,f77afbc3,1fd12bc8,91fbb1e4,6bcce194.\n'
p.write_text(old+new,encoding='utf-8')
registry=Path('docs/datasets/fundamentals_notes.registry.json'); reg=json.loads(registry.read_text())
reg['verification']['restricted_year_scope']='census recomputed for every republished existing item year'
reg['verification']['disclosed_reportable_container_exceptions']={'0001140361-23-014888':'ReportableLegalEntities','0001722482-24-000012':'ReportableGeographicalComponents','0001982518-25-000007':'ReportableGeographicalComponents'}
reg['verification']['primary_pilot']='100 deterministic residual filings; no all-filer denominator certification'
registry.write_text(json.dumps(reg,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'report_updated':True,'gate_status':cov['gate_status']}))
