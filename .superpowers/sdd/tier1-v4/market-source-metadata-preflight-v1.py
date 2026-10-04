"""Bounded metadata leads for the shared market reader; no payload admission."""
import hashlib
import json
import os
from pathlib import Path
from atx_db.alpha_panel import common as C

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
out = base / 'market-source-metadata-preflight-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous metadata evidence')
root = C.build_root().resolve()
if root != Path('C:/atx/atx-db/data/alpha_panel/v1'):
    raise RuntimeError('Unexpected configured root; inspect separately')
metadata_names = (
    'prices/manifest.json', 'corporate_actions/manifest.json',
    'identity/link_table_manifest.json', 'security_master/manifest.json',
    'market/market_shares_manifest.json', 'market/market_liquidity_manifest.json',
    'borrow_proxy/manifest.json', 'short_interest/manifest.json',
    'thirteenf/manifest.json', 'ftd/manifest.json', 'regsho_threshold/manifest.json',
    'fundamentals_release_v1/current.json', 'fundamentals/manifest.json',
)
metadata = {}
pins = {}
read_bytes = 0
for name in metadata_names:
    path = root / name
    if not path.exists():
        metadata[name] = {'exists': False}
        continue
    if path.is_symlink() or path.is_junction() or not path.is_file():
        raise RuntimeError('Unexpected metadata member type')
    size = path.stat().st_size
    if size > 1024 * 1024:
        metadata[name] = {'exists': True, 'bytes': size, 'status': 'NOT_OPENED_OVERSIZE'}
        continue
    raw = path.read_bytes()
    read_bytes += len(raw)
    if read_bytes > 8 * 1024 * 1024:
        raise RuntimeError('Total metadata budget exceeded')
    pin = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
    pins[str(path)] = pin
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError):
        metadata[name] = {'exists': True, **pin, 'status': 'UNPARSEABLE_METADATA'}
        continue
    metadata[name] = {'exists': True, **pin, 'declared': value,
                      'qualification': 'Untrusted source/publication declarations only; referenced bytes and semantics not verified.'}
payload_paths = [C.TICKERHISTORY.resolve(), root / 'calendar.parquet',
                 root / 'security_master/finra_names.parquet', root / 'identity/link_table.parquet',
                 root / 'corporate_actions/vendor_events.parquet']
payloads = {}
for path in payload_paths:
    if not path.exists():
        payloads[str(path)] = {'exists': False}
        continue
    if not path.is_file() or path.is_symlink() or path.is_junction():
        raise RuntimeError('Unexpected payload path type')
    stat = path.stat()
    payloads[str(path)] = {'exists': True, 'bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns,
                           'payload_or_footer_opened': False, 'identity_authenticated': False}
for path, pin in pins.items():
    raw = Path(path).read_bytes()
    if {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()} != pin:
        raise RuntimeError('Metadata changed during inspection')
result = {'status': 'MARKET_SOURCE_METADATA_LEADS_ONLY', 'configured_root': str(root),
          'metadata_bytes_read_per_pass': read_bytes, 'metadata': metadata, 'payload_path_stats': payloads,
          'source_payload_bytes_read': 0, 'gate_promotion': False,
          'qualification': 'Only the thirteen exact named small metadata paths were inspected and byte-checked twice. Five exact payload paths were statted without opening payload/footer or hashing source contents. Presence, declared complete/PASS, manifest/source hashes and mtime are leads, not actual source/clock/economic qualification. Missing named paths do not prove absence elsewhere. No directory scan, build, return evaluation, source fetch or publication occurred.'}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'metadata_bytes_read_per_pass': read_bytes,
                  'metadata_present': [name for name, row in metadata.items() if row['exists']],
                  'metadata_missing': [name for name, row in metadata.items() if not row['exists']],
                  'payload_path_stats': payloads, 'gate_promotion': False}), flush=True)
