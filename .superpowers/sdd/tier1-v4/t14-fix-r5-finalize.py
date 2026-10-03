"""Guarded Task14 owned report/documentation from final measured source evidence."""
import json,re,subprocess,collections
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes as F
a=Path('../.superpowers/sdd/tier1-v4');e=C.read_json(a/'t14-fix-r5-evidence.json');den=C.read_json(a/'t14-fix-r5-denominator-summary.json');pilot=C.read_json(a/'t14-fix-r5-pilot-summary.json');retained=C.read_json(a/'t14-fix-r5-retained-validation.json')
collisions=C.read_json(a/'t14-fix-r5-item-collisions.json');collision=collisions['summary']
collision['segment_revenue_keys_affected']=sum(r['item']=='revenue' for r in collisions['published_rows'])
collision['all_published_collision_rows_nondimensional']=all(r['dimn']==0 and r['segments'] is None for r in collisions['published_rows'])
collision['affected_current_notes_field_names']=sorted({r['item'] for r in collisions['key_groups_and_downstream_notes'] if r.get('notes_wide_ambiguous_source_choice')})
collision['affected_feature_field_names']=sorted(set(collision['affected_current_notes_field_names'])&set(F.FEATURE_COLS))
collision['current_notes_key_groups_by_field']=dict(collections.Counter(r['item'] for r in collisions['key_groups_and_downstream_notes'] if 'current_notes_wide_value' in r))
collision['ambiguous_notes_source_choices_by_field']=dict(collections.Counter(r['item'] for r in collisions['key_groups_and_downstream_notes'] if r.get('notes_wide_ambiguous_source_choice')))
collision['published_feature_columns_affected']=len(collision['affected_feature_field_names'])
assert collision['segment_revenue_keys_affected']==0 and collision['current_business_rows']==0 and collision['all_published_collision_rows_nondimensional'] and not collision['affected_feature_field_names']
(a/'t14-fix-r5-item-collisions.json').write_text(json.dumps(collisions,indent=2),encoding='utf8')
assert pilot['processed']==100 and pilot['outcomes']['verified_structure']==14
assert retained['automatic_cuts']==5 and retained['curated_hash_and_role_matches']==9
assert e['duplicate_keys']['items_all_years']==36 and collision['current_notes_ambiguous_source_choices']==6
gc3=C.read_json(a/'t14-fix-r5-final-coverage.json') if (a/'t14-fix-r5-final-coverage.json').exists() else None
cov=gc3 or e;old=C.read_json(a/'t14-fix-r4-evidence.json');prior={r['adsh']:r for r in old['effects']['cuts']}
table='| Year | Numeric multi proxy | Revenue observed | Revenue proxy | Verified reconciled | Reconciliation |\n|---|---:|---:|---:|---:|---:|\n'
for y in range(2020,2026):
 r=e['segments'][str(y)];table+=f"|{y}|{r['multi_segment']}|{r['with_segment_revenue']}|{r['share_segment_revenue']:.2%}|{r['reconciled_1pct']}|{r['share_reconciled']:.2%}|\n"
measure='| Accession | Prior source segment measure sum | Selected customer/external revenue sum | Company revenue | Source concept |\n|---|---:|---:|---:|---|\n'
for r in e['cuts']:
 if not r['proof'].get('selected_external_revenue'):continue
 measure+=f"|{r['adsh']}|{prior[r['adsh']]['seg_sum']:,.0f}|{r['seg_sum']:,.0f}|{r['consolidated_revenue']:,.0f}|{r['revenue_tag']}|\n"
receipts=[]
names=[p.stem for p in sorted((a/'receipts').glob('t14_fix_r5*.json')) if not p.stem.startswith('t14_fix_r5_finalize')]
for name in names:
 r=C.read_json(a/'receipts'/f'{name}.json')
 receipts.append(f"- `{name}.json`: command `{' '.join(r['command'])}`, cap {r['job_limit_gb']} GiB, exit {r['returncode']}, {r['run_seconds']}s, peak {r['native_peak_job_memory_gb']} GiB, cap_hit={r['cap_hit']}.\n")
tests=[]
for label,filename in [('notes','test_alpha_panel_notes.py'),('fund','test_alpha_panel_notes_fund.py'),('semantic','test_alpha_panel_notes_semantic.py')]:
 result=(a/f't14-fix-r5-tests-{label}.txt').read_text(encoding='utf8').strip().splitlines()[-1]
 tests.append(f"- `.venv/Scripts/python.exe -m pytest -o addopts='' --run-slow tests/{filename} -q`: `{result}`.\n")
gc3_text=(f"The one final controller-released GC3 refresh completed at `{gc3['remeasured_at']}` against universe manifest SHA256 `{gc3['universe_manifest_sha256']}` after R9 release. All nine manifest outputs, including six required 2020-2025 files, matched manifest bytes and SHA before the refresh; full before/after file and manifest hashes remained identical. Final panels member_equity, univ_all_common and univ_r3000p are in `t14-fix-r5-final-coverage.json`. Earlier overlap-era universe measurements are superseded. This is consumer coverage, not Task2 universe parity certification; controller reports its French 10/60 gate still FAIL." if gc3 else "GC3 final acceptance remains **PENDING** under R9. Final source/local build used `--defer-universe-coverage`, so no universe file was read or accepted while the producer rewrote it. Current member_equity coverage is measured independently. The controller must release final producer completion and required-output integrity before one final refresh.")
collision_text=f"""**Whole-stage public item-key integrity FAIL.** `t14_fix_r5_final_audit.json` exits 1 and `t14-fix-r5-evidence.json` preserves {collision['duplicate_key_groups']} duplicate advertised long-item key groups/{collision['published_duplicate_rows']} rows; no narrowed passing audit replaces this result. Of these, {collision['disagreeing_value_key_groups']} groups have different values. Existing extraction deduplicates with `x.version` but projects neither namespace nor version to the advertised `(adsh,tag,is_custom,segments,ddate,qtrs,uom,coreg)` key. The retained NUM rows contain same-local-name US-GAAP and IFRS concepts (2018-2024); exact landing file SHA, taxonomy/version, values and published rows are in `t14-fix-r5-item-collisions.json`. Both extraction and `wide_sql` are unchanged in the `74320900..c7ff21df` diff. Collision families are assets (7 groups), long_lived_assets (5), tax_dtl (21), goodwill (3). {collision['current_notes_key_groups']} current-period tax/goodwill groups win notes tag precedence ({json.dumps(collision['current_notes_key_groups_by_field'],sort_keys=True)}); all {collision['current_notes_ambiguous_source_choices']} distinct-value source ties affect `notes_wide.tax_dtl`. The three goodwill keys include one current-period group; all duplicated values agree. Wide ordering also lacks namespace/version. Exact accession, preferred tag, alternative values and published wide value are retained per key. All collision rows are nondimensional, with zero revenue tags and zero current business-axis rows. The computed intersection of affected notes fields with `FEATURE_COLS` is empty; census reads raw current business-axis facts and reconciliation/HHI read revenue. Thus the collision defect affects advertised long-item uniqueness and some tax_dtl wide values, with zero impact on segment features, count, HHI or business gates. Controller owns unchanged-code whole-branch/Task17 namespace triage; this round does not waive or hide it."""
body=f'''

## Fix round 5 — bounded declaration syntax and source-declared external measures (2026-10-03)

**DONE_WITH_CONCERNS; binding gates 0 PASS / 2 FAIL.** Controller-dispatched GPT-6.1-sol, xhigh reasoning; no child agents. IMPLEMENTER/resume/brief/global/foreign-dirty contracts, binding §2 item 12 and round4 review findings read fully; graph tools unavailable. I5 corrected through production `semantic_cut`; I4 has five concrete source-backed reconciliations but remains unmet. Prior I1-I3/N1 and M1 remain settled. No waiver, denominator relaxation or arithmetic fitting was introduced.

`c7ff21df` requires a syntactically introduced declared list and exact full-item alias identity. Sentence boundaries terminate lists; bounded consecutive paragraphs can supply pure declared items only. Every contributing block's qualifiers and years are checked; forward statements referring to the declared segments qualify the list. Descriptive and unmapped items cannot be completed from nearby known DIM names. Both exact reviewer counterexamples, following historical/year/reorganization qualifiers, multiblock lists, item-number syntax, conjunctions inside names, and unrelated subsequent product/market prose have production regressions. An initial overbroad forward qualifier treated ADES market expectations as a reportable-list qualification; reopened original source showed the unrelated scope, which was corrected with an attribution rule and regression.

All retained 100 primary cases were reprocessed with final code: {json.dumps(pilot['outcomes'],sort_keys=True)}. All five automatic cuts and nine curated cuts were revalidated from actual retained source. The nine original primary payloads retain 9/9 receipt/SHA/role/member equalities and nonempty exact citation sets in `t14-fix-r5-reopened-proof.json`; no old curated map alone was accepted.

Bounded source measure correction requires exact `Revenues from customers` row or `Sales to external customers` section, a complete current annual member set, a sound matching role, defined same-currency unit, and the same row/section's no-DIM annual company-total fact. Exact NUM tag/period/qtrs/currency/coreg/DIM contexts and unique same-concept company total then corroborate the selected source. This measure selection precedes and never depends on the member sum. ADES separately reports customer revenue and equity-method earnings; Evoqua separately reports gross/intersegment/external sales. No earnings or intersegment component is silently recast or numerically bridged. Gross facts remain unchanged in items/segments and `revenue_tag_strict`/`rel_diff_strict`; source-selected external facts drive the published economic HHI and selected revenue tag. Signed bridges retain their original exact revenue-row/context-family/period/unit guards; switching concept never carries a gross-measure bridge.

`bseg_partition_quality` certifies reportable identity only. New `bseg_rev_measure_basis` and recon `revenue_measure_basis` distinguish source_declared_external_revenue, source_reported_segment_measure, tagged_unverified_segment_measure and inferred_consolidated_singleton. Business measure basis carries with the same preceding visible business block; foreign geography remains unverified. Source offsets, rows, annual member and company-total context IDs, unit refs, receipt hashes and original values remain inspectable in proof JSON.

Amounts below are original filer USD values (not fitted); exact source row/context/role evidence is in `t14-fix-r5-evidence.json` and `t14-fix-r5-components-inspect.json`:

{measure}
Publication: {len(e['files'])} parquet files/{e['published_bytes']:,} bytes; {e['source_cut_rows']} cuts, {e['source_cut_reconciled']} reconciled, {e['source_cut_not_reconciled']} still failing. New verified reconciliations by filing year are {json.dumps(e['yearly_gate_effect'],sort_keys=True)}. Remaining revenue-bearing misses {e['remaining_2020_2025_revenue_misses']:,}; missing-revenue filings {e['remaining_2020_2025_missing_revenue']:,}. The bounded fixes cannot close the >=95% gate.

{table}
Revenue proxy still misses >=90% in 2020/2021/2024 and its independent all-filer denominator remains uncertified; every year still misses >=95% reconciliation. Both binding gate families explicitly FAIL in manifest. Final audit independently recomputed these gates using production `coverage`/`gate_status` without universe reads.

Independent denominator revalidation reopened all 698 original spans across all 497 old positive accessions with exact section/SHA/span equalities. Final syntax retains 423 old positive accessions, rejects 74 (115 spans), and independently finds 60 additional pure named disclosures: {den['proof_spans']} spans across {den['positive_accessions']} current positives ({sum(r[1] for r in den['by_year_multi_single_numeric_missing_multi'])} multi/{sum(r[2] for r in den['by_year_multi_single_numeric_missing_multi'])} single), no count conflicts, {sum(r[3] for r in den['by_year_multi_single_numeric_missing_multi'])} multi outside numeric proxy. NUM/DIM names, values and reconciliations never enter this independent parser. `t14-fix-r5-prior-positive-revalidation.jsonl` retains every old declaration, candidate syntax partition and acceptance/refusal for review. The 2,057 prior claim accessions were reopened; {den['unknown_unexamined_filings']:,} of 35,677 annual filings remain unclassified. This nonprobability positive subset is not a census or a population estimate. Optional `part-00127.parquet` corruption remains disclosed.

Residual work is specific: the other retained pilot cases still require current reportable-level/role/alias mapping, incomplete-fact or full-revenue concept validation, or valid links (statuses quantified above). Missing eligible retained NUM concepts are parser/landing/semantic questions, not evidence of intrinsic absence. Three previously curated source cuts still need valid company-sum semantics/bridges. Outside this bounded cache, 1,235 missing-revenue filings and the remaining revenue-bearing misses need filing-specific retained-source verification and, only where missing, primary/linkbase or expanded fact landing. The 35,194 unclassified all-filer denominator cases need independent disclosure classification with current reporting-period semantics; absent regex claims cannot prove single-segment reporting. No blanket intrinsic-gap assertion or speculative regex expansion was made.

{gc3_text}

All real-data commands ran sequentially from `C:/atx/atx-db` with `PYTHONPATH=C:/atx/atx-db/src`, `OPENBLAS_NUM_THREADS=1`, wrapper `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/NAME.json -- COMMAND`:

{''.join(receipts)}
Offline fixture files were run separately to demonstrate GC4's <60s per file:

{''.join(tests)}
Final publication hashes: {len(e['file_hash_matches'])}/{len(e['file_hash_matches'])} output bytes/SHA matches and {len(e['module_hash_matches'])}/{len(e['module_hash_matches'])} module SHA matches. Six derived public tables have zero duplicate keys; candidate promotions, census/recon divergence, quality origin/clock errors, amendment singleton errors and unsupported foreign quality upgrades are zero. These checks do **not** certify whole-stage integrity.

{collision_text}

Source clocks/cutoff/amendment carry remain covered offline. No return statistic was computed. No foreign/shared registry or controller ledger/HANDOFF path was changed.

Sources/network: **0 HTTP attempts, 0 response bytes, 0 compressed landing bytes** this round. Retained SEC primary/linkbase receipts and cached section partitions were reused. The 300-filing/1,200-attempt/1Hz/2GiB compressed/4GiB response allocation remains an unused ceiling; no bulk source plan was launched. Code and raw/source disk use remain bounded; no >1GiB landing occurred. The first build (`t14_fix_r5_build_final.json`, 162.1s) predates the explicit company-total/HHI basis strengthening and is superseded by `t14_fix_r5_build_final_r2.json`. Earlier 13-cut pilot evidence caused by the overly broad forward expectation rule is superseded by final 14-cut source revalidation. An initial retained-validation SQL alias ambiguity failed read-only, was corrected, and its successful final receipt is listed above. Evidence/documentation commit is the containing report commit (`git log -1 -- .superpowers/sdd/tier1-v4/task-14-report.md`). Controller adjudication after scoped round5 review is required for residual I4 gates; this implementer grants no waiver.
'''
report=a/'task-14-report.md';text=report.read_text(encoding='utf8').split('\n\n## Fix round 5 —')[0]
text=text.replace('Current status after Fix round 4','Current status after Fix round 5',1).replace('latest Fix round 4 section','latest Fix round 5 section',1)
text=text.replace('**81.00%-84.96%**','**81.05%-84.96%**',1)
warning=' Whole-stage integrity also FAILS: 36 pre-existing public item-key collisions and six ambiguous tax/goodwill notes-wide source choices; exact provenance and zero segment/gate impact are documented in Fix round 5.'
text=text.replace(warning,'').replace(warning.replace('tax/goodwill','tax_dtl'),'')
text=text.replace('No fitting was restored to inflate verification.','No fitting was restored to inflate verification.'+warning.replace('tax/goodwill','tax_dtl'),1)
report.write_text(text+body,encoding='utf8')
doc=Path('docs/datasets/fundamentals_notes.md');text=doc.read_text(encoding='utf8').replace('Fix round 4 publication','Fix round 5 publication',1)
text=text.replace('81.00%-84.96%','81.05%-84.96%',1)
for name,rows,size in e['files']:text=re.sub(r'\| `'+re.escape(name)+r'` \| [\d,]+ \| [\d,]+ \|',f'| `{name}` | {rows:,} | {size:,} |',text)
text=re.sub(r'Parquet total [\d,]+ bytes',f"Parquet total {e['published_bytes']:,} bytes",text)
text=text.replace('only for the nine exact filings','for the 14 exact retained filings')
warning='The advertised item key is not currently unique: 36 existing US-GAAP/IFRS namespace/version collisions remain an explicit integrity failure; six current tax/goodwill notes-wide source choices are ambiguous. See the detailed source diagnosis below.\n'
text=text.replace(warning,'').replace(warning.replace('tax/goodwill','tax_dtl'),'')
text=text.replace('Underlying long-item and notes-wide family definitions',warning.replace('tax/goodwill','tax_dtl')+'Underlying long-item and notes-wide family definitions',1)
start=text.index('## Source structure and downstream quality');end=text.index('## Gates',start)
section=f'''## Source structure and downstream quality (Fix round 5)

Reportable identity requires a syntactically bounded declared list, exact complete-item aliases, qualifiers/years over all contributing blocks, exact current annual inline contexts and currency definitions, and a sound matching presentation role. Adjacent product prose cannot complete unmapped names. Historical/conditional/forward segment qualifiers refuse certification. Genuine pure multiline lists and conjunctions within complete names remain supported.

The retained 100-case pilot supplies 14 source cuts (nine curated plus five automatic ADES/Evoqua). All original retained bytes were reprocessed, including nine receipt/SHA/role/member equalities and exact citation sets in `t14-fix-r5-reopened-proof.json`. Reportable-count identity alone does not establish the revenue measure's meaning or company-sum reconciliation.

`bseg_rev_measure_basis` (VARCHAR, source/PIT clock and annual staleness as business block) separately states source_declared_external_revenue, source_reported_segment_measure, tagged_unverified_segment_measure or inferred_consolidated_singleton. It carries together with the preceding visible business block. `revenue_measure_basis` is the corresponding recon diagnostic; `revenue_tag_strict` (VARCHAR) preserves the original selected concept. Source-declared external measure selection requires the exact customer/external section, annual member facts and same-section annual company-total context, defined same currency, sound role, exact NUM contexts and unique own total. Gross and customer/external facts remain distinct; no contract component, equity-method earnings or intersegment amount is fitted into company revenue. Source offsets, table rows, member and company context IDs and unit refs travel in proof JSON. HHI uses original source-selected values; partition quality remains reportable identity only.

Signed bridges require the same revenue row, selected context family, annual period and unit. A separate intersegment-sales row cannot be subtracted from external sales. Source-declared reportable Corporate/All Other remains economic; explicitly nonreportable rows may bridge only with source proof. Switching revenue concept cannot carry the original gross-measure bridge.

Current cuts reconcile {e['source_cut_reconciled']}/{e['source_cut_rows']}; remaining score-window misses are {e['remaining_2020_2025_revenue_misses']:,} revenue-bearing and {e['remaining_2020_2025_missing_revenue']:,} missing revenue. Unknown cases remain parser/landing/source-semantic questions, not intrinsic absences. All output/module hashes and six derived public keys match; quality/PIT provenance checks pass. Whole-stage integrity remains failed:

{collision_text}

Independent text evidence has {den['proof_spans']} named spans/{den['positive_accessions']} positive accessions. All old 698 spans/497 accessions were reopened; 423 remain positive, 74 are refused, and 60 additional pure named cases are accepted. This candidate-restricted positive frame leaves {den['unknown_unexamined_filings']:,} annual filings unclassified, so the all-filer denominator is uncertified. One optional cached section partition remains corrupt. No new HTTP attempt or landing occurred.

{gc3_text}

'''
text=text[:start]+section+text[end:]
text=text.replace('t14_fix_r4_build_final.json','t14_fix_r5_build_final_r2.json').replace('build --years 2025\n','build --years 2025 --defer-universe-coverage\n')
for y in range(2020,2026):
 r=e['segments'][str(y)];row=f"| {y} | {r['ten_k']:,} | {r['multi_segment']:,} | {r['with_segment_revenue']:,} | {'PASS' if r['share_segment_revenue']>=.9 else 'FAIL'} {r['share_segment_revenue']:.4f} | {r['reconciled_1pct']:,}/{r['with_segment_revenue']:,} | FAIL {r['share_reconciled']:.4f} |"
 text=re.sub(r'\| '+str(y)+r' \| [\d,]+ \| [\d,]+ \| [\d,]+ \| (?:PASS|FAIL) [\d.]+ \| [\d,/]+ \| FAIL [\d.]+ \|',row,text)
start=text.index('## Measured month-end coverage');end=text.index('## Limits',start)
tables='## Measured month-end coverage\n\n'
cols=['cells','linked','annual_row','bseg_n','bseg_rev_hhi','foreign_sales_share','major_cust_share','dd1','oplease_liab','stkco']
for panel,block in cov['cells'].items():
 if not isinstance(block,dict) or 'by_year' not in block:continue
 tables+=panel+': strict month-end 22:00 UTC cutoff, period within548days; latest visible period/vintage.\n\n| Year | '+' | '.join(cols)+' |\n|'+'---|'*(len(cols)+1)+'\n'
 for y,r in block['by_year'].items():tables+='| '+y+' | '+' | '.join(str(r.get(c)) for c in cols)+' |\n'
 tables+='\n'
if not gc3:tables+='univ_all_common / univ_r3000p: final producer/integrity release and coordinated refresh pending (R9).\n\n'
doc.write_text(text[:start]+tables+text[end:],encoding='utf8')
stub=Path('docs/datasets/fundamentals_notes.registry.json')
registry_text=subprocess.run(['git','show','c7ff21df:atx-db/docs/datasets/fundamentals_notes.registry.json'],capture_output=True,text=True,check=True).stdout
registry=json.loads(registry_text)
for output in registry['outputs']:
 old_doc=output['doc']
 if output['view']=='fundamentals_notes_items':output['doc']='Long registry-tag facts as filed; advertised keys are NOT currently unique: 36 legacy US-GAAP/IFRS local-name/version collisions (72 rows). Extraction dedups by taxonomy version but public items omit it; namespace triage required before registration.'
 if output['view']=='fundamentals_notes_wide':output['doc']='One row per periodic filing, current-period notes in filer currency. Legacy namespace ambiguity affects 6 distinct-value tax_dtl source choices; goodwill duplicate values agree. See Task14 Fix5 collision evidence. Segment features/gates unaffected.'
 if old_doc!=output['doc']:registry_text=registry_text.replace(json.dumps(old_doc),json.dumps(output['doc']))
registry['verification']['public_item_key_unique']=False
registry['verification']['public_item_key_collision_groups']=36
registry['verification']['whole_stage_integrity']='FAIL: long-item keys collide across omitted US-GAAP/IFRS namespace/version; 6 current tax_dtl wide source choices are ambiguous; goodwill duplicates agree in value. Derived public table keys are unique, segment features/revenue gates unaffected. Controller/Task17 triage required.'
extra='\n'.join('                         '+json.dumps(k)+': '+json.dumps(registry['verification'][k])+',' for k in ('public_item_key_unique','public_item_key_collision_groups','whole_stage_integrity'))
registry_text=registry_text.replace('"verification":  {','"verification":  {\n'+extra)
assert json.loads(registry_text)==registry
stub.write_text(registry_text,encoding='utf8')
print(json.dumps({'report_appended':True,'parquet_bytes':e['published_bytes'],'source_cut_reconciled':e['source_cut_reconciled'],'binding_gates':'0 PASS / 2 FAIL','gc3_final_pending':not bool(gc3)}))
