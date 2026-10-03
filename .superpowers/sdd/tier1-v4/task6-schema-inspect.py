"""Footer schemas only: no rows, values, return computations or acceptance evidence."""
import json
from pathlib import Path
import pyarrow.parquet as pq

root = Path('C:/atx/atx-db/data/alpha_panel/v1')
patterns = ['prices_history/year=*/prices.parquet', 'prices/year=*/prices.parquet',
            'universe/year=*/universe.parquet', 'compustat_std/funda.parquet',
            'compustat_std/fundq.parquet', 'ratios/fund_state.parquet',
            'market/liquidity/year=*/liquidity.parquet', 'borrow_proxy/year=*/borrow_proxy.parquet',
            'classification/issuer_industry.parquet']
out = {}
for pattern in patterns:
    files = sorted(root.glob(pattern))
    out[pattern] = {'files_count': len(files), 'schema': None, 'sample_path': None}
    if files:
        out[pattern]['sample_path'] = str(files[0])
        out[pattern]['schema'] = str(pq.ParquetFile(files[0]).schema_arrow)
Path('C:/atx/.superpowers/sdd/tier1-v4/task6-native-schemas.json').write_text(
    json.dumps(out, indent=2), encoding='utf-8')
print(json.dumps(out, indent=2))
