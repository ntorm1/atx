"""Guarded post-publication source-cut outcomes and lower-bound disclosure/source disagreement."""
import json,collections
from pathlib import Path
from atx_db.alpha_panel import common as C
a=Path('../.superpowers/sdd/tier1-v4');root=C.stage_dir('fundamentals_notes');con=C.connect(memory='150MB',threads=1)
recon=(root/'segment_recon.parquet').as_posix();feat=(root/'segment_features.parquet').as_posix();proof=(root/'segment_structure.parquet').as_posix()
rows=con.execute(f"""SELECT r.adsh,r.recon_class,r.n_members,r.n_revenue_members,r.seg_sum,r.consolidated_revenue,
 r.reconciling_items,r.rel_diff_after_source_bridge,f.bseg_n_tagged,f.bseg_n,f.bseg_rev_hhi,s.proof_json
 FROM read_parquet('{proof}') s JOIN read_parquet('{recon}') r USING(adsh)
 JOIN read_parquet('{feat}') f USING(adsh) ORDER BY 1""").fetchall()
summary={'source_cut_rows':len(rows),'source_cut_reconciled':sum(r[1]=='reconciled_search' for r in rows),
 'source_cut_not_reconciled':sum(r[1]!='reconciled_search' for r in rows),
 'remaining_2020_2025_revenue_misses':con.execute(f"""SELECT count(*) FROM read_parquet('{recon}') WHERE year(filed) BETWEEN 2020 AND 2025 AND multi_segment AND n_revenue_members>=1 AND recon_class NOT IN ('direct','with_reconciling','reconciled_search')""").fetchone()[0],
 'remaining_2020_2025_missing_revenue':con.execute(f"SELECT count(*) FROM read_parquet('{recon}') WHERE year(filed) BETWEEN 2020 AND 2025 AND multi_segment AND n_revenue_members=0").fetchone()[0]}
quality=con.execute(f"SELECT bseg_partition_quality,count(*) FROM read_parquet('{feat}') GROUP BY 1").fetchall()
summary['feature_quality_counts']=quality
claims=[json.loads(s) for s in (a/'t14-fix-r3-disclosure-census.jsonl').read_text().splitlines()]
con.register('claim_keys',__import__('pyarrow').table({'adsh':[c['adsh'] for c in claims],'classification':[c['classification'] for c in claims]}))
summary['uncorroborated_claim_numeric_disagreements']=con.execute(f"""SELECT k.classification,count(*) FILTER(WHERE r.multi_segment),count(*) FILTER(WHERE NOT r.multi_segment)
 FROM claim_keys k JOIN read_parquet('{recon}') r USING(adsh) GROUP BY 1""").fetchall()
con.close()
summary['cuts']=[dict(zip(['adsh','recon_class','n_members','n_revenue_members','seg_sum','consolidated_revenue','source_bridge','rel_diff_after_bridge','raw_tagged_count','economic_count','hhi','proof'],r[:-1]+(json.loads(r[-1]),))) for r in rows]
(a/'t14-fix-r4-effects.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in summary.items() if k!='cuts'}))

