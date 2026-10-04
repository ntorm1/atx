"""Guarded exact extraction layout recovery audit; no files copied or changed."""
import json
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C, fund_extract as X

root = C.build_root()
workspace = Path(__file__).resolve().parent
out = workspace / 'task16-extract-layout-audit-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous audit')
required = {'plan.json'} | {f'batch-{i:04d}{suffix}' for i in range(85) for suffix in ('.parquet', '.json', '.stats.parquet')}
results = {}
for stage in ('fundamentals_v10', 'fundamentals_v11'):
    directory = root / stage / '_work/cf'
    members = {p.name for p in directory.iterdir() if p.is_file()}
    plan = C.read_json(directory / 'plan.json')
    stats = {}
    for name in sorted(required & members):
        if not name.endswith('.stats.parquet'):
            continue
        path = directory / name
        before = path.stat()
        parquet = pq.ParquetFile(path)
        sha = C.sha256_file(path)
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError('Stats changed while auditing')
        stats[name] = {'path': str(path), 'sha256': sha, 'bytes': before.st_size,
                       'mtime_ns': before.st_mtime_ns, 'rows': parquet.metadata.num_rows,
                       'native_schema_matches': parquet.schema_arrow == X.STATS_SCHEMA}
    results[stage] = {'directory': str(directory), 'files': len(members),
                      'missing': sorted(required - members), 'extra': sorted(members - required),
                      'plan': plan, 'plan_sha256': C.sha256_file(directory / 'plan.json'),
                      'stats': stats, 'stats_bytes': sum(row['bytes'] for row in stats.values())}
result = {'status': 'AUDIT_ONLY_NO_RECOVERY', 'stages': results,
          'qualification': 'Retained stats are not source-admitted. Any recovery still requires independent archive replay and full native validation.'}
C.write_json_atomic(out, result)
print(json.dumps({stage: {k: v for k, v in row.items() if k not in ('plan', 'stats')} for stage, row in results.items()}), flush=True)
