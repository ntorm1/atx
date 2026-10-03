"""Guarded authoritative publication evidence and Task14 owned documentation/report."""
import json,re
from pathlib import Path
from atx_db.alpha_panel import common as C
a=Path('../.superpowers/sdd/tier1-v4');root=C.stage_dir('fundamentals_notes')
man=C.read_json(root/'manifest.json');cov=C.read_json(root/'coverage.json')
effects=C.read_json(a/'t14-fix-r4-effects.json');den=C.read_json(a/'t14-fix-r4-denominator-summary.json')
con=C.connect(memory='150MB',threads=1)
files=[[name,con.execute(f"SELECT count(*) FROM read_parquet('{(root/name).as_posix()}')").fetchone()[0],entry['bytes']] for name,entry in man['files'].items()]
con.close()
coverage_inputs=C.read_json(a/'t14-fix-r4-coverage-inputs.json')
hashes=coverage_inputs['before']
evidence={'files':files,'published_bytes':sum(f[2] for f in files),'gate_status':cov['gate_status'],'segments':cov['segments'],'cells':cov['cells'],'coverage_inputs_sha256':hashes,'remeasured_at':man.get('remeasured_at'),'effects':effects,'independent_positive_denominator':den,'http_attempts':0,'new_raw_bytes':0,'network_allocation_used':False}
(a/'t14-fix-r4-evidence.json').write_text(json.dumps(evidence,indent=2),encoding='utf8')
doc=Path('docs/datasets/fundamentals_notes.md');text=doc.read_text(encoding='utf8')
text=text.replace('Corrected Task 14 Fix round 3 publication','Corrected Task 14 Fix round 4 publication')
for name,rows,size in files:
 text=re.sub(r'\| `'+re.escape(name)+r'` \| [\d,]+ \| [\d,]+ \|',f'| `{name}` | {rows:,} | {size:,} |',text)
text=re.sub(r'Parquet total [\d,]+ bytes',f"Parquet total {evidence['published_bytes']:,} bytes",text)
start=text.index('## Source structure and downstream quality')
end=text.find('\n## ',start+4)
if end<0:end=len(text)
quality=dict(effects['feature_quality_counts'])
section=f'''## Source structure and downstream quality (Fix round 4)

The reusable primary parser certifies an explicit named current-reportable list only when exact annual inline revenue contexts, currency unit definitions, and a sound matching presentation role corroborate the selected level. Conditional, historical, reorganization, operating-only and product-line statements remain unresolved. Exact names can select an economic level above graph leaves. A conflicting unrelated role cannot replace the primary-declared cut; overlapping selected members are refused. Equal numeric values cannot merge siblings.

The retained 100-filing pilot has 14 source cuts: nine prior accession-specific proofs plus five automatically extracted cuts across ADES and Evoqua. The original nine primary gzip payloads were reopened independently: 9/9 primary SHA matches and 9/9 exact HTML citation spans are retained in `t14-fix-r4-reopened-proof.json`. Those source judgments certify reportable identity, not reconciliation or foreign sales.

`segment_structure.parquet` key `(adsh,tag)` and clock `available_at` retain URLs, receipt/primary hashes, roles, context IDs, current claim offsets and revenue table spans. Signed bridge terms require the same primary revenue row, selected source context family and currency units. A separate intersegment-sales row cannot be subtracted from external-sales amounts. Explicit nonreportable All Other may bridge company revenue; named reportable All Other or Corporate remains in the economic count and HHI. Bridge amounts remain original NUM values and never alter economic values. The original strict diagnostic and post-source-bridge residual both remain inspectable.

Gold must filter `bseg_partition_quality`: source_verified_reportable ({quality.get('source_verified_reportable',0)} rows), tagged_unverified_reportable ({quality.get('tagged_unverified_reportable',0)}), inferred_singleton_unverified ({quality.get('inferred_singleton_unverified',0)}); {quality.get(None,0)} rows have no business block. Quality/source accession carry only with preceding visible business blocks. `bseg_n_tagged` retains the original tagged proxy; foreign geography remains independently unverified.

Current production source cuts reconcile {effects['source_cut_reconciled']} of {effects['source_cut_rows']}; remaining 2020-2025 gaps are {effects['remaining_2020_2025_revenue_misses']:,} revenue-bearing verified misses and {effects['remaining_2020_2025_missing_revenue']:,} missing-revenue filings. Semantic quality is independent of the 1% test. Unresolved cases are not labeled intrinsic absence.

The cached text pass independently extracts 698 current named disclosure spans across 497 accessions (447 multi/50 single), with 15 named multi accessions outside the numeric multi proxy. NUM/DIM values and fitted sums do not enter this classifier. It reopens the 2,057 prior positive candidates from the readable 19,388-accession cache; absent, truncated, ambiguous and unexamined disclosures remain unknown. The 35,180 other annual filings prevent exact all-filer certification. This is a nonprobability positive frame, not a census or a population estimate. One optional section partition remains corrupt. No new HTTP attempt or raw landing occurred in round 4.

GC3 coverage input universe SHA256 `{hashes.get('universe_manifest')}` at `{man.get('remeasured_at')}`. Task 2 final universe publication/one controller-coordinated refresh is pending; this records the current measurement without chasing intermediate builds.
'''
text=text[:start]+section+text[end:]
text=text.replace('t14_fix_r3_build.json','t14_fix_r4_build_final.json')
for y in range(2020,2026):
 r=cov['segments'][str(y)]
 row=f"| {y} | {r['ten_k']:,} | {r['multi_segment']:,} | {r['with_segment_revenue']:,} | {'PASS' if r['share_segment_revenue']>=.9 else 'FAIL'} {r['share_segment_revenue']:.4f} | {r['reconciled_1pct']:,}/{r['with_segment_revenue']:,} | FAIL {r['share_reconciled']:.4f} |"
 text=re.sub(r'\| '+str(y)+r' \| [\d,]+ \| [\d,]+ \| [\d,]+ \| (?:PASS|FAIL) [\d.]+ \| [\d,/]+ \| FAIL [\d.]+ \|',row,text)
start=text.index('## Measured month-end coverage');end=text.index('## Limits',start)
tables='## Measured month-end coverage\n\n'
cols=['cells','linked','annual_row','bseg_n','bseg_rev_hhi','foreign_sales_share','major_cust_share','dd1','oplease_liab','stkco']
for panel,block in cov['cells'].items():
 tables+=panel+': strict month-end 22:00 UTC cutoff, period within548days; latest visible period/vintage.\n\n| Year | '+' | '.join(cols)+' |\n|'+'---|'*(len(cols)+1)+'\n'
 for y,r in block['by_year'].items():tables+='| '+y+' | '+' | '.join(str(r.get(c)) for c in cols)+' |\n'
 tables+='\n'
text=text[:start]+tables+text[end:]
doc.write_text(text,encoding='utf8')
report=a/'task-14-report.md';old=report.read_text(encoding='utf8')
old=old.split('\n\n## Fix round 4 — reusable current named lists, exact revenue-row bridges and independent positive denominator')[0]
old=old.replace('Current status after Fix round 3','Current status after Fix round 4',1).replace('latest Fix round 3 section','latest Fix round 4 section',1)
gate='| Year | Numeric multi proxy | Revenue observed | Revenue proxy | Verified reconciled | Reconciliation |\n|---|---:|---:|---:|---:|---:|\n'
for y in range(2020,2026):
 r=cov['segments'][str(y)];gate+=f"|{y}|{r['multi_segment']}|{r['with_segment_revenue']}|{r['share_segment_revenue']:.2%}|{r['reconciled_1pct']}|{r['share_reconciled']:.2%}|\n"
receipts=[]
for name in ['t14_fix_r4_inspect','t14_fix_r4_pilot_final','t14_fix_r4_reopen','t14_fix_r4_denominator','t14_fix_r4_build_final','t14_fix_r4_coverage_final','t14_fix_r4_effects_final','t14_fix_r4_integrity_final']:
 r=C.read_json(a/'receipts'/f'{name}.json');receipts.append(f"- `{name}.json`: command `{' '.join(r['command'])}`, cap {r['job_limit_gb']} GiB, exit {r['returncode']}, {r['run_seconds']}s, peak {r['native_peak_job_memory_gb']} GiB, cap_hit={r['cap_hit']}.\n")
tests=(a/'t14-fix-r4-tests.txt').read_text(encoding='utf8').strip().splitlines()[-1]
body=f'''

## Fix round 4 — reusable current named lists, exact revenue-row bridges and independent positive denominator (2026-10-03)

**DONE_WITH_CONCERNS; binding gates 0 PASS / 2 FAIL.** Owner-requested GPT-6.1-sol fresh implementer; no child agents. Contracts, binding §2 item 12, foreign-dirty list and round3 findings read verbatim. Settled I1-I3/N1 remain unchanged. No source allocation was consumed: 0 new HTTP attempts (including retries/redirects), 0 responses, 0 compressed bytes. No whole 3,183/35,677 corpus fetch was launched. The bounded 300-filing/1,200-attempt allocation remains unused while source semantics and concept mapping need further validation.

Code commit `74320900` adds `fund_notes_semantic.py`, integrates it in production and the manifest code hashes, and covers source statement/list/table/role/context/unit selection offline. Five automatic economic cuts across unrelated ADES and Evoqua supplement the original nine curated proofs. Primary economic certification needs no arithmetic equality and does not certify geography. Exact source-backed bridges use the same annual revenue row/context family/unit; separate intersegment-sales rows are rejected. Source-declared reportable names remain economic members even when a generic corporate/other name filter would exclude them. Raw proxy counts remain separate. M1 documentation now distinguishes candidate exclusions from source-verified nonselected levels.

Original nine primary payloads independently reopened: 9/9 receipt/primary SHA matches and 9/9 readable citation sets, preserving compressed hash, URL, source span character offsets and exact raw-span SHA. Fresh source review confirms Martin Marietta East/West plus Magnesia; Helix four strategic units with aggregated Well Intervention; Watts Americas/Europe/APMEA; Riot current Bitcoin Mining/Engineering with historical hosting separate; Fathom three reportables plus corporate/elimination reconciliation; California Resources oil/gas plus carbon management; Republic Groups1/2/3 with subtotal separate; Franklin Covey four reportables; Lionsgate three reportables with Studio Business aggregation. `t14-fix-r4-reopened-proof.json` provides the full inspectable source evidence, rather than relying on the old curated map alone.

Production publishes {len(files)} parquet files/{evidence['published_bytes']:,} bytes. Source cut rows {effects['source_cut_rows']}, reconciled {effects['source_cut_reconciled']}, unreconciled {effects['source_cut_not_reconciled']}. Remaining gaps: {effects['remaining_2020_2025_revenue_misses']:,} revenue-bearing misses and {effects['remaining_2020_2025_missing_revenue']:,} missing revenue. The five automatic cuts establish partition quality; they still need issuer revenue-definition/bridge mapping before passing the company sum. ADES explicitly includes equity-method earnings in segment revenues; Evoqua reported segment revenue contains additional sales requiring supported bridge mapping. No amount or concept equivalence is fitted to close these cases.

Independent text classifier reopens 2,057 cached prior claim accessions; 698 exact named spans support 497 current positive accessions (447 multi,50 single), no count conflicts. Fifteen multi accessions are outside the numeric multi proxy (yearly 1/4/3/2/2/3). Examples include HP Personal Systems/Printing/Corporate Investments, Central Garden Pet/Garden, Applied Materials Semiconductor Systems/Applied Global Services, and Broadcom semiconductor solutions/infrastructure software. The named source list extractor uses no numerical axis, total or reconciliation result. Still 35,180 unclassified annual filings: this nonprobability positive frame cannot certify the entire denominator, imply missing-source singleton, or waive either binding threshold. Corrupt `part-00127.parquet` remains disclosed. The prior751/203/628/etc candidates are not themselves proof.

{gate}
Both gate families FAIL: reconciliation below95% every year; revenue below90% in2020/2021/2024 plus independently uncertified all-filer denominator. Command `-m atx_db.alpha_panel.fund_notes coverage` measured these gates and GC3 on all three panels. Exact coverage inputs/rows are in `t14-fix-r4-evidence.json`; universe SHA256 `{hashes.get('universe_manifest')}`, measurement `{man.get('remeasured_at')}`. Task2 final publication refresh is pending for controller coordination; no post2022 return statistic was computed.

All guarded commands below ran from `C:/atx/atx-db` with `PYTHONPATH=C:/atx/atx-db/src`, `OPENBLAS_NUM_THREADS=1`, wrapper `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/NAME.json -- COMMAND`. Each was one data job at a time.

{''.join(receipts)}
Fixture command `.venv/Scripts/python.exe -m pytest -o addopts='' --run-slow tests/test_alpha_panel_notes.py tests/test_alpha_panel_notes_fund.py tests/test_alpha_panel_notes_semantic.py -q`: `{tests}`. Offline tests cover temporal/reorg/conditional vs current disclosures, product/operating vs reportable lists, missing contexts/units/year identity/tables, conflicting declarations, invalid roles, named All Other and Corporate, signed bridge rows/units, reportable-name restoration, economic HHI and source clocks. Final integrity checks verify every output and module SHA, duplicate keys, candidate promotion, census/recon alignment, provenance/clock and foreign upgrade errors; all errors zero.

Self-review exposed and corrected an early parser attribute collision with HTMLParser.offset, a too-permissive descriptive-name fixture, and a same-table bridge attempt that mixed external-sales/intersegment-sales rows (first build temporarily lost two reconciliations). Those attempts are superseded by exact row/context selection and the final guarded build. Rebuild2 still preceded the final source-name restoration/code-hash publication; only `t14_fix_r4_build_final.json` is authoritative. Final covering tests are green. No foreign/shared registry or controller ledger/HANDOFF path was edited. Open I4/independent census remain explicitly unmet; unexamined cases are not declared intrinsic source gaps.
'''
report.write_text(old+body,encoding='utf8')
print(json.dumps({'files':len(files),'bytes':evidence['published_bytes'],'source_cuts':effects['source_cut_rows'],'reconciled_cuts':effects['source_cut_reconciled'],'remaining_misses':effects['remaining_2020_2025_revenue_misses'],'gates':cov['gate_status'],'universe_sha256':hashes.get('universe_manifest')}))
