"""Legacy terminal-candidate keys/metadata only; never read return columns."""
import json
import os
from pathlib import Path
from atx_db.alpha_panel import common as C

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'task12-candidate-preflight-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous candidate audit')
path = C.build_root() / 'delisting/events.parquet'
before = {'sha256': C.sha256_file(path), 'bytes': path.stat().st_size}
con = C.connect(memory='128MB', threads=1)
try:
    schema = con.execute('DESCRIBE SELECT * FROM read_parquet(?)', [str(path)]).fetchall()
    counts = con.execute("SELECT year(last_session),cause,continued,count(*) FROM read_parquet(?) WHERE last_session>=DATE '2020-01-01' GROUP BY ALL ORDER BY ALL", [str(path)]).fetchall()
    keys = con.execute("SELECT count(*),count(DISTINCT (security_id,last_session,available_at)),count(*) FILTER(WHERE security_id IS NULL OR last_session IS NULL OR available_at IS NULL) FROM read_parquet(?) WHERE cause='mna' AND NOT continued AND last_session>=DATE '2020-01-01'", [str(path)]).fetchone()
    years = con.execute("SELECT year(last_session),count(*),count(*) FILTER(WHERE ever_member) FROM read_parquet(?) WHERE cause='mna' AND NOT continued AND last_session>=DATE '2020-01-01' GROUP BY ALL ORDER BY ALL", [str(path)]).fetchall()
finally:
    con.close()
if {'sha256': C.sha256_file(path), 'bytes': path.stat().st_size} != before:
    raise RuntimeError('Legacy candidate bytes drifted')
result = {'status': 'LEGACY_CANDIDATE_METADATA_ONLY', 'path': str(path), 'identity': before,
          'schema': schema, 'counts_year_cause_continued': counts,
          'mna_noncontinued_2020_onward': {'rows': keys[0], 'distinct_keys': keys[1], 'null_key_rows': keys[2], 'year_rows_member_rows': years},
          'return_columns_read': False, 'returns_computed': False, 'http_attempts': 0,
          'qualification': 'Legacy candidate classification and clocks remain unqualified. Counts do not certify deal identity, completion, proceeds or exact DLRET.'}
out.write_text(json.dumps(result, indent=2, default=str) + '\n', encoding='utf-8')
print(json.dumps(result, default=str), flush=True)
