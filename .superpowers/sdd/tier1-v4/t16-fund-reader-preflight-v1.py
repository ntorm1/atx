"""Root guarded metadata-only inventory before independent fund byte binding."""
import json
import os
from pathlib import Path
os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fund_batch_release as R
from atx_db.alpha_panel import fundamentals as F, fund_extract as X

workspace = Path(__file__).resolve().parent
output = workspace / 'task16-fund-reader-preflight-v1.json'
if output.exists():
    raise RuntimeError('Preserve previous audit')
root = C.build_root().resolve()
if root != Path('C:/atx/atx-db/data/alpha_panel/v1').resolve():
    raise RuntimeError('Unexpected root')
products = {}
missing = []
for name, rule in R.reader_contract(root).items():
    selected = R._actual(rule)
    if name == 'fx':
        selected = [root / 'reference/fx_daily.parquet']
    if rule.get('optional') and not any(path.exists() for path in selected):
        products[name] = None
        continue
    if not selected:
        missing.append(name + ':empty inventory')
    rows = []
    for path in selected:
        path = path.resolve()
        if not path.is_file():
            missing.append(str(path))
            rows.append({'path': str(path), 'exists': False})
        else:
            st = path.stat()
            rows.append({'path': str(path), 'exists': True, 'bytes': st.st_size,
                         'mtime_ns': st.st_mtime_ns})
    products[name] = rows
paths = {'root': root, 'prep_root': F.work_dir(), 'extract_root': X.out_dir(),
         'fsds_root': C.FSDS_DIR, 'archive': X.ARCHIVE}
path_metadata = {}
for name, path in paths.items():
    path = path.resolve()
    path_metadata[name] = {'path': str(path), 'exists': path.exists()}
    if not path.exists():
        missing.append(str(path))
    elif path.is_file():
        path_metadata[name].update(bytes=path.stat().st_size, mtime_ns=path.stat().st_mtime_ns)
result = {'status': 'METADATA_ONLY_NOT_ADMITTED', 'paths': path_metadata,
          'products': products, 'missing': missing,
          'file_count': sum(len(rows or []) for rows in products.values()),
          'logical_bytes': sum(row.get('bytes', 0) for rows in products.values() if rows for row in rows),
          'qualification': 'No payload hashes, schema/key proof, original source qualification or gates.'}
C.write_json_atomic(output, result)
print(json.dumps({k: v for k, v in result.items() if k != 'products'}), flush=True)
raise SystemExit(2 if missing else 0)
