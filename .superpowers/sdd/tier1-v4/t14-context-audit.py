"""Bounded diagnostics of retained disclosure contexts and independent numeric denominator."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import fund_notes as F

con = C.connect(memory='300MB', threads=1)
root = C.build_root()
items = (root / 'fundamentals_notes/items/year=*/items.parquet').as_posix()
con.execute(f"CREATE TEMP VIEW it AS SELECT * FROM read_parquet('{items}', hive_partitioning=false)")
extra = con.execute(f"""SELECT segments, count(*) FROM it WHERE axis_kind='business' AND item='revenue'
 AND NOT regexp_matches(segments, '{F.SEGMENTS_ONLY_RE}') GROUP BY 1 ORDER BY 2 DESC LIMIT 40""").fetchall()
tags = con.execute("""SELECT tag,count(*) FILTER (WHERE segments IS NULL),count(*) FROM it
 WHERE item='revenue' GROUP BY 1 ORDER BY 3 DESC""").fetchall()
Path('../.superpowers/sdd/tier1-v4/t14-context-preview.json').write_text(json.dumps({'extra_axis_top40':extra,'revenue_tags':tags},indent=2),encoding='utf-8')
# Full NUM landing retains every fact on a business/geographic axis, independently of fund_notes registry.
den = []
for y in range(2020, 2026):
    sources = con.execute(f"""SELECT DISTINCT source FROM read_parquet(
      '{(root / 'notes/parts/source=*/sub.parquet').as_posix()}', hive_partitioning=false)
      WHERE year(filed)={y} AND form IN ('10-K','10-KT')""").fetchall()
    paths = [root / 'notes/parts' / f'source={s[0]}' for s in sources]
    ns, ds, ss = ([str(p / f'{f}.parquet').replace('\\','/') for p in paths] for f in ('num_items','dim','sub'))
    con.execute(f"""CREATE OR REPLACE TEMP TABLE s AS SELECT adsh,period,filed,source FROM read_parquet({ss},hive_partitioning=false)
      WHERE year(filed)={y} AND form IN ('10-K','10-KT') QUALIFY row_number() OVER(PARTITION BY adsh ORDER BY source)=1""")
    con.execute('CREATE OR REPLACE TEMP TABLE allmem(adsh VARCHAR, member VARCHAR)')
    for npath, dpath in zip(ns,ds):
        con.execute(f"""CREATE OR REPLACE TEMP TABLE bd AS SELECT dimhash,
          regexp_extract(segments,'(^|;)BusinessSegments=([^;]*)',2) AS member FROM read_parquet('{dpath}')
          WHERE segments LIKE '%BusinessSegments=%'
          AND NOT regexp_matches(regexp_extract(segments,'(^|;)BusinessSegments=([^;]*)',2),'{F.RECONCILING_RE}')""")
        con.execute(f"""INSERT INTO allmem SELECT DISTINCT n.adsh,d.member FROM read_parquet('{npath}') n
          JOIN s USING(adsh,source) JOIN bd d ON d.dimhash=n.dimh WHERE n.coreg IS NULL AND n.ddate=s.period""")
    row = con.execute(f"""WITH mem AS (SELECT adsh,count(DISTINCT member) AS m FROM allmem GROUP BY 1),
      frame AS (SELECT adsh,multi_segment FROM read_parquet(
       '{(root / 'fundamentals_notes/segment_recon.parquet').as_posix()}'))
      SELECT count(*),count(*) FILTER(WHERE m>=2), count(*) FILTER(WHERE m>=2 AND NOT coalesce(f.multi_segment,false)),
      count(*) FILTER(WHERE f.adsh IS NULL) FROM s LEFT JOIN mem USING(adsh) LEFT JOIN frame f USING(adsh)""").fetchone()
    den.append([y,*row])
result = {'extra_axis_top40': extra, 'revenue_tags':tags,
 'denominator_columns':['year','all_original_10k','current_period_any_numeric_tag_multi','missing_from_registry_multi','no_registry_filing'],
 'numeric_denominator':den}
Path('../.superpowers/sdd/tier1-v4/t14-context-audit.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result))
