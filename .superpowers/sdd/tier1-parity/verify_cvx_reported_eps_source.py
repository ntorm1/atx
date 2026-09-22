"""Read-only extraction check against a frozen, decoded official SEC exhibit."""

import datetime as dt
import hashlib
import json
from pathlib import Path

from atx_db.press_release import _reported_fiscal_quarter, extract_reported_gaap_diluted_eps

base = Path(__file__).parent
source = base / 'cvx-q4-2025-official-exhibit.html'
out = base / 'cvx-q4-2025-official-extraction-result.json'
if out.exists():
    raise SystemExit('Refusing to overwrite extraction evidence')
payload = source.read_bytes()
digest = hashlib.sha256(payload).hexdigest()
assert digest == '4b2ba83859eeb59a5ed94dca29a4a22f7c1d5409166b34c0ae4a2c4690494b00'
document = payload.decode('utf-8-sig')
fact, reason = extract_reported_gaap_diluted_eps(
    document, period_end=None, filed_on=dt.date(2026, 1, 30)
)
fiscal = _reported_fiscal_quarter(document, fact['period_end'] if fact else None)
result = {
    'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(),
    'source_url': 'https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/a12312025ex9918-k.htm',
    'local_decoded_utf8_sha256': digest,
    'original_http_body_hash_measured': False,
    'production_receipt': False,
    'warehouse_mutations': False,
    'filing_event_report_date': '2026-01-30',
    'fact': fact, 'rejection_reason': reason, 'fiscal': fiscal,
}
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps(result, default=str), flush=True)
assert reason is None and fact is not None
assert fact['period_end'] == dt.date(2025, 12, 31)
assert fact['value'] == 1.39 and fact['prior_year_quarter_value'] == 1.84
assert fiscal == (2025, 'Q4')
