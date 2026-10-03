"""Publish documentation from corrected artifacts, under the memory guard."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

root=C.stage_dir('fundamentals_notes')
cov=json.loads((root/'coverage.json').read_text(encoding='utf-8'))
man=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
con=C.connect(memory='300MB',threads=1)
files=[]
for file,entry in man['files'].items():
    rows=con.execute(f"SELECT count(*) FROM read_parquet('{(root/file).as_posix()}')").fetchone()[0]
    files.append([file,rows,entry['bytes']])
recon=(root/'segment_recon.parquet').as_posix()
residual=con.execute(f"""SELECT year(filed),recon_class,count(*) FROM read_parquet('{recon}')
 WHERE multi_segment AND n_revenue_members>=1 AND year(filed) BETWEEN 2020 AND 2025
 GROUP BY 1,2 ORDER BY 1,2""").fetchall()
diagnostics=con.execute(f"""SELECT year(filed),recon_rule,count(*) FROM read_parquet('{recon}')
 WHERE candidate_only AND year(filed) BETWEEN 2020 AND 2025 GROUP BY 1,2 ORDER BY 1,2""").fetchall()
projection=con.execute(f"""SELECT context_basis,count(*),count(DISTINCT adsh) FROM
 read_parquet('{(root/'segments.parquet').as_posix()}') GROUP BY 1""").fetchall()
span=con.execute(f"""SELECT min(filed),max(filed),min(available_at),max(available_at),min(period),max(period)
 FROM read_parquet('{(root/'segment_features.parquet').as_posix()}')""").fetchone()
amendments=con.execute(f"""SELECT count(*) FILTER(WHERE is_amendment),count(*) FILTER(WHERE carry_n>0),
 count(*) FILTER(WHERE bseg_basis='inferred_singleton' AND is_amendment) FROM
 read_parquet('{(root/'segment_features.parquet').as_posix()}')""").fetchone()
text_files=list(Path('data/raw/sec_text/docs').glob('part-*.parquet'))
text_coverage=[]
if text_files:
    text_paths=[p.as_posix() for p in text_files]
    text_coverage=con.execute(f"""SELECT year(c.filed),count(*),count(t.accession)
      FROM read_parquet('{(root/'segment_census.parquet').as_posix()}') c
      LEFT JOIN (SELECT DISTINCT accession FROM read_parquet({text_paths},union_by_name=true)) t
      ON replace(c.adsh,'-','')=replace(t.accession,'-','')
      WHERE c.form IN ('10-K','10-KT') AND year(c.filed) BETWEEN 2020 AND 2025 GROUP BY 1 ORDER BY 1""").fetchall()
con.close()
evidence={'files':files,'published_bytes':sum(r[2] for r in files),'span':list(map(str,span)),
          'residual':residual,'arithmetic_candidates':diagnostics,'contexts':projection,'amendments':amendments,
          'gate_status':cov['gate_status'],'cells':cov['cells'],'existing_text_docs':[len(text_files),text_coverage],
          'coverage_inputs_sha256':{name:C.sha256_file(path) for name,path in
             [('universe_manifest',C.build_root()/'universe/manifest.json'),
              ('identity_manifest',C.build_root()/'identity/manifest.json'),('calendar',C.calendar_path())] if path.exists()},
          'coverage_remeasured_at':man.get('remeasured_at')}
Path('../.superpowers/sdd/tier1-v4/t14-fix-r1-evidence.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')

doc=Path('docs/datasets/fundamentals_notes.md')
old=doc.read_text(encoding='utf-8')
column_start=old.index('## `segment_features` columns')
column_end=old.index('## Reconciliation and context provenance') if '## Reconciliation and context provenance' in old else old.index('## Segment revenue reconciliation')
columns=old[column_start:column_end]
columns=columns.replace('reportable business-segment members (`StatementBusinessSegmentsAxis`, not reconciling-like, not a roll-up / duplicate dropped by the reconciliation search) over every fact of the filing',
                        'current-period reportable-looking business members across every numeric tag in the raw notes landing; no arithmetic deletion')
columns=columns.replace('or 1 for an annual filing with consolidated revenue and no tagged member',
                        'or inferred 1 for an original annual filing with consolidated revenue and no tagged member; partial amendments inherit earlier blocks')
columns=columns.replace('reconciliation-search rule that fixed the member set (annual; NULL = strict)',
                        'disclosed-component rule of verified reconciliation; arithmetic candidates never replace members')
columns=columns.replace('first tier-1 revenue chain tag with a non-zero total','first explicitly recognized full-revenue concept with a non-zero total')
if '| bseg_basis |' not in columns:
    columns=columns.replace('| bseg_n |', '| bseg_basis | VARCHAR | provenance | `current_period_numeric_tags` or `inferred_singleton`; carried blocks retain source basis |\n| bseg_n |')
columns=columns.replace('(`mixed` when the search merged tags)','(source concept, never replaced by an arithmetic candidate)')
columns=columns.replace('with at least one feature.','with at least one feature or an amendment state placeholder that can inherit an earlier feature block.')
header='''# Dataset `fundamentals_notes`: segments and notes, point in time

Corrected Task 14 Fix round 1 publication (2026-10-03). Code `src/atx_db/alpha_panel/fund_notes.py`,
schema `atx.alpha-panel.fundamentals-notes/v1`, clock `notes-accepted-v1`.
Source: existing SEC DERA Financial Statement and Notes landing, 2019q1-2026_08; no new download.

**Current binding gates: 0 PASS / 2 FAIL.** Source-wide numeric revenue proxy misses >=90% in 2020, 2021,
2024, and the all-filer multi-segment disclosure denominator remains uncertified. Verified reconciliation is
81.00%-84.95%, below 95%. Earlier registry-proxy PASS and arithmetic-fitting figures are superseded.

## Outputs

| File | Rows | Bytes |
|---|---:|---:|
'''
header+=''.join(f'| `{name}` | {rows:,} | {size:,} |\n' for name,rows,size in files)
header+=f"\nParquet total {evidence['published_bytes']:,} bytes. Features filed {span[0]} through {span[1]}, available_at {span[2]} through {span[3]}, periods {span[4]} through {span[5]}.\n"
header+='''
Keys: segment_features `(cik,period,adsh)` (vintage `(cik,period)`); segment_recon and segment_census `adsh`;
notes_wide `adsh`; items `(adsh,tag,is_custom,segments,ddate,qtrs,uom,coreg)`;
segments `(adsh,metric,axis_kind,member,tag,ddate,qtrs,segments,uom)`.
`segment_census.parquet` covers every periodic SUB filing, with `cik,adsh,form,period,filed,available_at,n_members,multi_segment`.
Its count uses all retained numeric tags on the current-period business axis, independent of the feature registry.
Underlying long-item and notes-wide family definitions are in `docs/ALPHA_PANEL_NOTES.md`.

Additional diagnostic columns (source NUM/DIM and accession clock; same annual staleness as source filing):

| File | Column | Type/unit | Definition |
|---|---|---|---|
| segment_census | n_members | BIGINT/count | distinct current-period reportable-looking business members across all retained numeric tags |
| segment_census | multi_segment | BOOLEAN | n_members >=2; tag-observable proxy, not an independent untagged-disclosure classification |
| segments | context_basis | VARCHAR/provenance | plain or timing_transfer_partition |
| segments | context_provenance | VARCHAR[]/DIM strings | original source contexts behind the value |
| segment_recon | total_basis | VARCHAR/provenance | full_revenue_concept or component_or_extension_unverified |
| segment_recon | candidate_total | DOUBLE/filer currency | nonverified comparison total retained for diagnosis |
| segment_recon | candidate_only | BOOLEAN | candidate arithmetic fit cannot promote verification or alter economic members |
| segment_recon | recon_rule | VARCHAR | disclosed-component correction or candidate fitting rule |
| segment_recon | dropped_members | VARCHAR[]/names | candidate-only suggested member deletions; never economic count deletion |
| segment_recon | seg_sum_reconciled | DOUBLE/filer currency | candidate sum, verified only when candidate_only=false |
| segment_recon | reconciling_used | DOUBLE/filer currency | disclosed component used in arithmetic comparison |
| segment_recon | seg_members | STRUCT(member VARCHAR,value DOUBLE)[] | candidate/verified member set, separated by candidate_only |

## PIT and amendments

Every row carries UTC `available_at`: DST-aware SUB acceptance time, otherwise filed +46 hours.
Daily consumption requires available_at < previous decision session 22:00 UTC.
Month-end coverage requires available_at < that month-end session 22:00 UTC (strict exclusion at equality).
Partial amendments containing consolidated revenue but no segment block inherit the earlier same-period
count/HHI; no singleton is inferred for an amendment. Earlier accession rows remain unchanged.
Original-filing singleton defaults are marked `bseg_basis=inferred_singleton`; this is not certified disclosure.
Annual coverage requires period within 548 days and selects latest period then latest visible vintage.

```sql
CREATE OR REPLACE MACRO segment_features_asof(ts) AS TABLE
 SELECT * FROM read_parquet('<root>/fundamentals_notes/segment_features.parquet')
 WHERE available_at < CAST(ts AS TIMESTAMPTZ)
 QUALIFY row_number() OVER(PARTITION BY cik,period ORDER BY available_at DESC,adsh DESC)=1;
```

'''
reconciliation='''## Reconciliation and context provenance

Original 10-K/10-KT filing frame starts from all SUB filings, including filings with no registry facts.
Multi-segment denominator uses current-period business-axis members across every numeric tag retained by notes_fetch.
Source-wide tagging is broader than the feature registry, but still cannot identify an untagged textual disclosure.

Verified reconciliation uses source member values and known full-revenue concepts. Industry/component/extension
revenue tags may supply segment measures, but cannot borrow an unrelated consolidated total to claim verification.
`total_basis` describes the concept class, `candidate_total` preserves the old candidate comparison,
`consolidated_revenue` is NULL when equivalence is unverified. Bank gross interest income, insurer premiums,
and lessor income are components, not automatically the company's revenue total; missing net-interest or
investment-income components are not fabricated. Named full-revenue tags are enumerated in manifest verification.

`recon_class_strict` preserves strict arithmetic classes. A correction using a disclosed consolidation component
can be `reconciled_search`. Numerically deleting equal-valued segments, arbitrary subset roll-ups, keyword-member
exclusions and mixed-tag fitting are `candidate_only=true`: diagnostics (`recon_rule,dropped_members,
seg_sum_reconciled,reconciling_used,seg_members`) never alter verified class, count, or HHI.
No numerical coincidence establishes an economic hierarchy.

Safe extra-axis projection supports exactly both standard ASC 606 timing members (TransferredAtPointInTime,
TransferredOverTime), exactly two non-null facts, no other extra axis, same fact key/currency/duration,
and no already-tagged plain segment total. Both mutually exclusive timing values are summed. `segments` rows
carry `context_basis=timing_transfer_partition` and original DIM strings in `context_provenance`; plain rows carry
`context_basis=plain`. Incomplete timing partitions and unproven geographic/product/entity hierarchies remain
in items and are excluded from automatic aggregation. No fitted sum or arbitrary partition completeness assumption is used.

## Gates

Both binding gates remain FAIL in manifest. Revenue's source-tagged proxy threshold result is separate
(`observable_threshold_pass`); certified PASS additionally requires an independent all-filer multi-segment
disclosure denominator, which is unavailable in the retained landing (`denominator_certified=false`).
Sum denominator is the multi-segment filings with observed segment revenue; threshold is >=95% within 1%.
Exact command from atx-db with PYTHONPATH=src and OPENBLAS_NUM_THREADS=1:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_fix_r1_build.json -- .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build
```

| Year | All 10-Ks | Multi-segment numeric | Revenue | Revenue proxy >=90% | Reconciled | Sum >=95% |
|---|---:|---:|---:|---|---:|---|
'''
for year in range(2020,2026):
    s=cov['segments'][str(year)]
    reconciliation+=f"| {year} | {s['ten_k']:,} | {s['multi_segment']:,} | {s['with_segment_revenue']:,} | {'PASS' if s['share_segment_revenue']>=.9 else 'FAIL'} {s['share_segment_revenue']:.4f} | {s['reconciled_1pct']:,}/{s['with_segment_revenue']:,} | FAIL {s['share_reconciled']:.4f} |\n"
reconciliation+='\nProxy observations above never constitute certified all-filer PASS. All diagnostics are value/coverage measurements; no return statistic is computed.\n\n## Measured month-end coverage\n'
for panel, entry in cov['cells'].items():
    if not isinstance(entry,dict) or 'by_year' not in entry:
        continue
    cols=['cells','linked','annual_row','bseg_n','bseg_rev_hhi','foreign_sales_share','major_cust_share','dd1','oplease_liab','stkco']
    reconciliation+=f"\n{panel}: {entry['basis']}\n\n| Year | "+' | '.join(cols)+' |\n|---|'+ '|'.join('---' for _ in cols)+'|\n'
    for year, row in entry['by_year'].items():
        reconciliation+='| '+year+' | '+' | '.join(str(row[c]) for c in cols)+' |\n'
reconciliation+='''
## Limits

The independent all-tag census diagnoses registry omissions but cannot certify untagged multi-segment filers.
All-tag current-period fact coverage and arithmetic reconciliation are not hand-verified filing fidelity.
Industry composites lacking consolidated component facts need expanded landing/disclosure validation.
Other extra-axis hierarchies need source structure rather than numeric fitting.
Major-customer percentage disclosures are largely absent from retained source; this task does not re-land them.
Region-only geography cannot identify domestic/foreign split. IFRS axes are unmapped. Amounts remain filer currency.
As-filed negative amounts are retained; operating lease rates outside [0,1] are NULL.
Daily users must continue using previous-session cutoff even though monthly coverage now uses month-end cutoff.
'''
doc.write_text(header+columns+reconciliation,encoding='utf-8')
registry=Path('docs/datasets/fundamentals_notes.registry.json')
reg=json.loads(registry.read_text(encoding='utf-8'))
reg['guard_gb']=.9
reg['verification']={'denominator_certified':False,'arithmetic_candidates':'diagnostic only','monthly_coverage_cutoff':'month-end session 22:00 UTC','extra_context_projection':'exhaustive timing partition with original-context provenance'}
reg['outputs']=[x for x in reg['outputs'] if x['view']!='fundamentals_notes_segment_census']
reg['outputs'].append({'view':'fundamentals_notes_segment_census','glob':'fundamentals_notes/segment_census.parquet',
 'clock':'available_at','vintage':'event','keys':['adsh'],'doc':'all periodic SUB filings and current-period numeric business-member counts independent of registry tags'})
for output in reg['outputs']:
    if output['view']=='fundamentals_notes_items':
        output['keys']=['adsh','tag','is_custom','segments','ddate','qtrs','uom','coreg']
    if output['view']=='fundamentals_notes_segment_recon':
        output['doc']='all original 10-K/10-KT filings: verified reconciliation class plus separate candidate-only diagnostics'
    if output['view']=='fundamentals_notes_segments':
        output['doc']='segment metrics with context_basis and original DIM context_provenance; plain or exhaustive timing-transfer projection'
reg['coverage']='fundamentals_notes/coverage.json: corrected month-end panels, all-tag census proxy denominator, uncertified revenue gate, verified reconciliation gate'
registry.write_text(json.dumps(reg,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'files':files,'contexts':projection,'gate_status':cov['gate_status']}))
