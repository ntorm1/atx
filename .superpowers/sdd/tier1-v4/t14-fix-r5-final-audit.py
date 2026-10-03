"""Guarded immutable final source/local publication, keys, hashes, quality and gate evidence."""
import collections,json
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes as F
a=Path('../.superpowers/sdd/tier1-v4');root=C.stage_dir(F.STAGE);man=C.read_json(root/'manifest.json')
con=C.connect(memory='250MB',threads=1)
checks={name:C.sha256_file(root/name)==entry['sha256'] and (root/name).stat().st_size==entry['bytes'] for name,entry in man['files'].items()}
code={name:C.sha256_file(Path('src/atx_db/alpha_panel')/name)==entry['sha256'] for name,entry in man['code'].items() if name.endswith('.py') and isinstance(entry,dict)}
files=[[name,con.execute(f"SELECT count(*) FROM read_parquet('{(root/name).as_posix()}')").fetchone()[0],entry['bytes']] for name,entry in man['files'].items()]
keys={'segment_features.parquet':'cik,period,adsh','segment_recon.parquet':'adsh','segment_census.parquet':'adsh','notes_wide.parquet':'adsh','segment_structure.parquet':'adsh,tag','segments.parquet':'adsh,metric,axis_kind,member,tag,ddate,qtrs,segments,uom'}
duplicates={name:con.execute(f"SELECT count(*) FROM (SELECT {columns},count(*) n FROM read_parquet('{(root/name).as_posix()}') GROUP BY ALL HAVING n>1)").fetchone()[0] for name,columns in keys.items()}
duplicates['items_all_years']=sum(con.execute(f"SELECT count(*) FROM (SELECT adsh,tag,is_custom,segments,ddate,qtrs,uom,coreg,count(*) n FROM read_parquet('{(root/name).as_posix()}') GROUP BY ALL HAVING n>1)").fetchone()[0] for name in man['files'] if name.startswith('items/'))
feat=(root/'segment_features.parquet').as_posix();rec=(root/'segment_recon.parquet').as_posix();census=(root/'segment_census.parquet').as_posix();structure=(root/'segment_structure.parquet').as_posix()
quality_errors=con.execute(f"""SELECT count(*) FROM read_parquet('{feat}') f LEFT JOIN read_parquet('{structure}') s ON s.adsh=f.bseg_partition_source_adsh WHERE f.bseg_partition_quality='source_verified_reportable' AND (s.adsh IS NULL OR s.cik<>f.cik OR s.period<>f.period OR s.available_at>f.available_at)""").fetchone()[0]
candidate_promotions=con.execute(f"SELECT count(*) FROM read_parquet('{rec}') WHERE candidate_only AND recon_class='reconciled_search'").fetchone()[0]
census_errors=con.execute(f"SELECT count(*) FROM read_parquet('{rec}') r JOIN read_parquet('{census}') c USING(adsh) WHERE r.n_members<>c.n_members OR r.multi_segment<>c.multi_segment").fetchone()[0]
foreign=con.execute(f"SELECT count(*) FROM read_parquet('{feat}') WHERE foreign_sales_quality LIKE 'source_verified%'").fetchone()[0]
amendments=con.execute(f"""SELECT count(*) FROM read_parquet('{feat}') a WHERE is_amendment AND bseg_basis='inferred_singleton' AND NOT EXISTS (SELECT 1 FROM read_parquet('{feat}') p WHERE p.cik=a.cik AND p.period=a.period AND (p.available_at<a.available_at OR (p.available_at=a.available_at AND p.adsh<a.adsh)) AND p.bseg_basis='inferred_singleton' AND p.bseg_n=a.bseg_n AND p.bseg_rev_hhi IS NOT DISTINCT FROM a.bseg_rev_hhi)""").fetchone()[0]
rows=con.execute(f"""SELECT r.adsh,r.recon_class,r.n_members,r.n_revenue_members,r.seg_sum,r.consolidated_revenue,r.reconciling_items,r.rel_diff_after_source_bridge,r.rel_diff_strict,r.revenue_tag,r.revenue_tag_strict,r.revenue_measure_basis,f.bseg_n_tagged,f.bseg_n,f.bseg_rev_hhi,f.bseg_rev_measure_basis,s.proof_json FROM read_parquet('{structure}') s JOIN read_parquet('{rec}') r USING(adsh) JOIN read_parquet('{feat}') f USING(adsh) ORDER BY 1""").fetchall()
cols=['adsh','recon_class','n_members','n_revenue_members','seg_sum','consolidated_revenue','source_bridge','rel_diff_after_bridge','rel_diff_strict','revenue_tag','revenue_tag_strict','revenue_measure_basis','raw_tagged_count','economic_count','hhi','hhi_measure_basis','proof']
cuts=[dict(zip(cols,r[:-1]+(json.loads(r[-1]),))) for r in rows]
misses=con.execute(f"SELECT count(*) FROM read_parquet('{rec}') WHERE year(filed) BETWEEN 2020 AND 2025 AND multi_segment AND n_revenue_members>=1 AND recon_class NOT IN ('direct','with_reconciling','reconciled_search')").fetchone()[0]
missing=con.execute(f"SELECT count(*) FROM read_parquet('{rec}') WHERE year(filed) BETWEEN 2020 AND 2025 AND multi_segment AND n_revenue_members=0").fetchone()[0]
quality=con.execute(f"SELECT bseg_partition_quality,count(*) FROM read_parquet('{feat}') GROUP BY 1").fetchall()
measure_quality=con.execute(f"SELECT bseg_rev_measure_basis,count(*) FROM read_parquet('{feat}') GROUP BY 1").fetchall()
cov=C.read_json(root/'coverage.json');assert 'GC3 pending' in cov['cells']['universe']
# Recompute binding source/local gates without reading any universe consumer cells.
measured=F.coverage(con,root);gates=F.gate_status(measured);assert gates==cov['gate_status']
initial=C.read_json(a/'t14-fix-r4-evidence.json');baseline=initial['segments']
delta={str(y):{'reconciled_before':baseline[str(y)]['reconciled_1pct'],'reconciled_after':measured['segments'][str(y)]['reconciled_1pct'],'new_reconciled':measured['segments'][str(y)]['reconciled_1pct']-baseline[str(y)]['reconciled_1pct']} for y in range(2020,2026)}
selection_diagnostics=[]
for c in cuts:
 p=c['proof'];ext=p.get('external_revenue_selection',{})
 if not ext:continue
 path=(root/'items/year=*/items.parquet').as_posix();tag=ext['concept'].removeprefix('us-gaap_')
 totals=con.execute(f"SELECT DISTINCT value FROM read_parquet('{path}',hive_partitioning=false) WHERE adsh=? AND tag=? AND ddate=? AND qtrs=4 AND uom=? AND coreg IS NULL AND segments IS NULL",[c['adsh'],tag,p['period'],ext['currency']]).fetchall()
 selection_diagnostics.append({'adsh':c['adsh'],'source_selection':ext,'production_applied':bool(p.get('selected_external_revenue')),'same_concept_company_num_totals':totals,'original_company_total':c['consolidated_revenue'],'source_recon_class':c['recon_class']})
con.close()
result={'files':files,'published_bytes':sum(r[2] for r in files),'file_hash_matches':checks,'module_hash_matches':code,'duplicate_keys':duplicates,'quality_provenance_errors':quality_errors,'candidate_promotions':candidate_promotions,'census_recon_mismatches':census_errors,'unsupported_foreign_upgrades':foreign,'amendment_singleton_errors':amendments,
 'source_cut_rows':len(cuts),'source_cut_reconciled':sum(c['recon_class']=='reconciled_search' for c in cuts),'source_cut_not_reconciled':sum(c['recon_class']!='reconciled_search' for c in cuts),
 'remaining_2020_2025_revenue_misses':misses,'remaining_2020_2025_missing_revenue':missing,'feature_quality_counts':quality,'hhi_measure_basis_counts':measure_quality,'cuts':cuts,'external_selection_diagnostics':selection_diagnostics,'yearly_gate_effect':delta,
 'segments':measured['segments'],'gate_status':gates,'cells':cov['cells'],'gc3_final_pending':True,'http_attempts':0,'new_raw_bytes':0}
(a/'t14-fix-r5-evidence.json').write_text(json.dumps(result,indent=2,default=str),encoding='utf8')
print(json.dumps({k:v for k,v in result.items() if k not in ('cuts','external_selection_diagnostics','cells','segments')}))
assert all(checks.values()) and all(code.values()) and not any(duplicates.values()) and not any([quality_errors,candidate_promotions,census_errors,foreign,amendments])
