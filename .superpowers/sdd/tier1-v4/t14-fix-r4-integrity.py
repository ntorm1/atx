"""Read-only corrected publication integrity checks; requires memory guard."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

out=C.stage_dir('fundamentals_notes')
man=C.read_json(out/'manifest.json')
checks={name:C.sha256_file(out/name)==entry['sha256'] and (out/name).stat().st_size==entry['bytes']
        for name,entry in man['files'].items()}
con=C.connect(memory='300MB',threads=1)
feature=(out/'segment_features.parquet').as_posix()
recon=(out/'segment_recon.parquet').as_posix()
census=(out/'segment_census.parquet').as_posix()
duplicate_keys=con.execute(f"""SELECT count(*) FROM (SELECT cik,period,adsh,count(*) n
 FROM read_parquet('{feature}') GROUP BY 1,2,3 HAVING n>1)""").fetchone()[0]
invalid_candidates=con.execute(f"""SELECT count(*) FROM read_parquet('{recon}')
 WHERE candidate_only AND recon_class='reconciled_search'""").fetchone()[0]
invalid_amendments=con.execute(f"""SELECT count(*) FROM read_parquet('{feature}')
 a WHERE is_amendment AND bseg_basis='inferred_singleton' AND NOT EXISTS (
  SELECT 1 FROM read_parquet('{feature}') p WHERE p.cik=a.cik AND p.period=a.period
  AND (p.available_at<a.available_at OR (p.available_at=a.available_at AND p.adsh<a.adsh))
  AND p.bseg_basis='inferred_singleton' AND p.bseg_n=a.bseg_n
  AND p.bseg_rev_hhi IS NOT DISTINCT FROM a.bseg_rev_hhi)""").fetchone()[0]
census_match=con.execute(f"""SELECT count(*) FROM read_parquet('{recon}') r
 JOIN read_parquet('{census}') c USING(adsh) WHERE r.n_members<>c.n_members OR r.multi_segment<>c.multi_segment""").fetchone()[0]
code=C.sha256_file(Path('src/atx_db/alpha_panel/fund_notes.py'))==man['code']['fund_notes.py']['sha256']
code_all={name:C.sha256_file(Path('src/atx_db/alpha_panel')/name)==entry['sha256'] for name,entry in man['code'].items() if name.endswith('.py') and isinstance(entry,dict)}
structure=(out/'segment_structure.parquet').as_posix()
quality_errors=con.execute(f"""SELECT count(*) FROM read_parquet('{feature}') f
 LEFT JOIN read_parquet('{structure}') s ON s.adsh=f.bseg_partition_source_adsh
 WHERE f.bseg_partition_quality='source_verified_reportable' AND
 (s.adsh IS NULL OR s.cik<>f.cik OR s.period<>f.period OR s.available_at>f.available_at)""").fetchone()[0]
foreign_upgrades=con.execute(f"SELECT count(*) FROM read_parquet('{feature}') WHERE foreign_sales_quality LIKE 'source_verified%'").fetchone()[0]
result={'file_hash_matches':checks,'code_hash_match':code,'all_module_hash_matches':code_all,'quality_provenance_errors':quality_errors,'unsupported_foreign_quality_upgrades':foreign_upgrades,'duplicate_feature_keys':duplicate_keys,
        'candidate_promotions':invalid_candidates,'amendment_singleton_inferences':invalid_amendments,
        'census_recon_mismatches':census_match}
con.close()
Path('../.superpowers/sdd/tier1-v4/t14-fix-r4-integrity.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result))
assert all(checks.values()) and code and all(code_all.values()) and not any([duplicate_keys,invalid_candidates,invalid_amendments,census_match,quality_errors,foreign_upgrades])

