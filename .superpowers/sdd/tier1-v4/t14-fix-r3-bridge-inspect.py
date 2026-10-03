import json
from atx_db.alpha_panel import common as C,fund_notes_structure as S
con=C.connect(memory='150MB',threads=1)
path=(C.stage_dir('fundamentals_notes')/'items/year=*/items.parquet').as_posix()
keys=','.join(repr(k) for k in S.CORROBORATED)
rows=con.execute(f"""SELECT adsh,tag,segments,value,ddate,uom FROM read_parquet('{path}',hive_partitioning=false)
 WHERE adsh IN ({keys}) AND qtrs=4 AND item='revenue' AND member IS NULL
 AND segments IS NOT NULL ORDER BY 1,2,3""").fetchall()
con.close()
for row in rows: print(json.dumps(row,default=str))
