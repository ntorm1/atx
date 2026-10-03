"""Read-only Task 14 evidence verification; run under the memory guard."""
import hashlib
import json
from pathlib import Path

from atx_db.alpha_panel import common as C

root = Path('data/alpha_panel/v1/fundamentals_notes')
manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
checks = []
for name, entry in manifest['files'].items():
    path = root / name
    digest = hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
    checks.append({'file': name, 'bytes': path.stat().st_size,
                   'match': digest == entry['sha256'] and path.stat().st_size == entry['bytes']})
con = C.connect(memory='300MB', threads=1)
sql = """SELECT revenue_tier, recon_class, count(*) FROM read_parquet(
 'data/alpha_panel/v1/fundamentals_notes/segment_recon.parquet')
 WHERE year(filed) BETWEEN 2020 AND 2025 AND multi_segment AND n_revenue_members >= 1
 GROUP BY 1,2 ORDER BY 1,2"""
residual = con.execute(sql).fetchall()
con.close()
code = Path('src/atx_db/alpha_panel/fund_notes.py').read_bytes()
result = {'files': checks, 'all_hashes_match': all(x['match'] for x in checks),
          'published_bytes': sum(x['bytes'] for x in checks),
          'code_hash_match': hashlib.sha256(code).hexdigest() == manifest['code']['fund_notes.py']['sha256'],
          'residual_sql': sql, 'reconciliation_by_tier_class': residual}
Path('../.superpowers/sdd/tier1-v4/t14-recovery-audit.json').write_text(
    json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result))
