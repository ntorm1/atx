"""Verify typed exhibit discovery on the frozen official filing-detail index."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

from atx_db.press_release import _archive_urls, _ex99_documents

base = Path(__file__).parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=base / 'cvx-q4-2025-official-index-discovery-result.json')
out = parser.parse_args().output
if out.exists():
    raise SystemExit('Refusing existing discovery evidence')
payload = (base / 'cvx-q4-2025-official-filing-index.html').read_bytes()
digest = hashlib.sha256(payload).hexdigest()
assert digest == 'b38b40dc498c75c28a272a2ff1e714da611e75e8462aa695209f6d63f93897fa'
url, directory = _archive_urls('0000093410', '0000093410-26-000019')
documents = _ex99_documents(payload.decode('utf-8-sig'), directory)
result = {
    'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(),
    'index_url': url, 'decoded_utf8_sha256': digest, 'documents': documents,
    'production_receipt': False, 'warehouse_mutations': False,
}
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2)
print(json.dumps(result))
assert url == directory + '/0000093410-26-000019-index.html'
assert documents == ('a12312025ex9918-k.htm',)
