from atx_db.alpha_panel import common as C
from pathlib import Path
import json
root=C.build_root(); con=C.connect(memory='150MB',threads=1);out={}
for stage,glob in [('panel','year=*/*.parquet'),('characteristics','year=*/characteristics.parquet'),('fundamentals','events.parquet')]:
 files=list((root/stage).glob(glob))
 out[stage]={'files':len(files),'schema':con.execute(f"DESCRIBE SELECT * FROM read_parquet('{files[0].as_posix()}')").fetchall() if files else []}
print(json.dumps(out,default=str))
