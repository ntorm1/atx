from pathlib import Path
import json
from atx_db.alpha_panel import common as C
root=C.build_root();con=C.connect(memory='160MB',threads=1,db_file='t2_docmeta.duckdb')
def tab(head,rows):return '\n| '+' | '.join(head)+' |\n|'+ '|'.join(['---']*len(head))+'|\n'+''.join('| '+' | '.join(str(x) for x in r)+' |\n' for r in rows)+'\n'
# Keep the reviewed field/timing definitions; replace stale numbers and broken exchange caveats.
p=Path('docs/datasets/market_index.md');old=p.read_text(encoding='utf-8');body=old[old.index('## PIT rule'):old.index('## Validation') if '## Validation' in old else old.index('## Measured')]
m=C.read_json(root/'market/market_index_manifest.json');checks=C.read_json(root/'validation/factors_market.json')
s='''# market_index (`market/index_returns.parquet`)

Rebuilt 2026-10-03 in Task 2x with NULL exchange repair from dated FINRA listing classes through one shared mapping (SC -> XNAS, BZX -> BATS). Frozen panel is never rebuilt. Rule `crsp-index-v1`, schema `atx.alpha-panel.market-index/v1`; manifest `market/market_index_manifest.json`; validation `validation/factors_market.json`.

R5 strict holdout: source rows are filtered to session_date <= --end <= 2022-12-31 before daily/monthly aggregation. No index-return products after 2022 are published. Future factor-return products remain unavailable until separately authorized; code, index membership and universe coverage still span the latest session. The initial inherited-builder run ignored --end for product rows; that artifact was superseded by this corrected build. No held-out comparison/evaluation was performed.

Inputs: frozen panel, shares_daily, security_master/finra_names, reference, prices and prices_history (validation history only). Exchange fallback preserves FINRA dissemination_date < d and age <=45 days; unsupported/stale classes remain unknown. T1-b's panel/link/directory qualifications remain inherited.

Command: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.market_index build --end 2022-12-31`.

## Product coverage
'''
f=(root/'market/index_returns.parquet').as_posix();s+=tab(['freq','universe','weighting','rows','first','last'],con.execute(f"SELECT freq,universe,weighting,count(*),min(period_end),max(period_end) FROM read_parquet('{f}') GROUP BY 1,2,3 ORDER BY 1,2,3").fetchall())
s+=body+'\n## Measured\n'+tab(['gate','value','threshold','status'],[['Mkt-RF monthly',checks['mkt_rf_member_common']['monthly_rho'],0.99,'PASS' if checks['pass']['mkt_rf_monthly_rho'] else 'FAIL'],['SMB monthly',checks['smb']['monthly_rho'],0.90,'PASS' if checks['pass']['smb_monthly_rho'] else 'FAIL'],['Market 2013-2022',checks['mkt_all_common_2013']['monthly_rho'],0.99,'PASS' if checks['pass']['mkt_all_common_monthly_rho_2013'] else 'FAIL']])
s+='\n## Eligible source rows by year (through holdout)\n'+tab(['year','all_common cells','member cells','known ME','member known ME'],m['receipt']['universe'])
s+='\n## Limits\n\nNo exact delisting returns; pre-2018 validation history uses first-panel-session eligibility and vendor shares with survivorship; French is a reposted CRSP202608 vintage. Mapping fixes known SC/BZX omissions but cannot create missing FINRA evidence. NYSE breakpoints retain issuer-primary XNYS logic.\n'
s+='\n## Input provenance\n'+tab(['input','SHA-256'],list(m['input_manifests_sha256'].items()));p.write_text(s,encoding='utf-8')
p=Path('docs/datasets/indexes.md');old=p.read_text(encoding='utf-8');body=old[old.index('## Rules'):old.index('## Measured')];m=C.read_json(root/'indexes/manifest.json')
s='''# indexes (`indexes/`)

Rebuilt 2026-10-03 in Task 2x, recomputing eligible company caps, ranks, Russell bands, S&P buffers and membership intervals from the frozen panel with dated fresh FINRA exchange repair. SC -> XNAS, BZX -> BATS; no additive membership patch and no panel rebuild. Rule `russell-proxy-v1` / `sp500-proxy-v1`; manifest `indexes/manifest.json`; registry stub `indexes.registry.json`.

Command: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build`.

FINRA fallback replaces only NULL panel exchange when dissemination_date < rank session and age <=45 days. Unknown/stale classes remain unknown. T1-b's panel/link/directory qualification persists. Rank-date availability remains 22:00 UTC and membership starts the next session after reconstitution. These are value/membership products and span 2018-2026 despite R5's truncation of aggregate-return products.

'''+body
f=(root/'indexes/recon_summary.parquet').as_posix();s+='\n## R2000 reconstitutions\n'+tab(['rank','effective','lines','companies'],con.execute(f"SELECT rank_date,effective_from,lines,companies FROM read_parquet('{f}') WHERE index_id='R2000P' ORDER BY 1").fetchall())
s+='\nInherited R2000 count gate [1,900,2,100]: '+json.dumps(m['gate_status'],sort_keys=True)+'\n'
s+='\n## Outputs\n'+tab(['file','rows','bytes'],[(str(f.relative_to(root)),con.execute(f"SELECT count(*) FROM read_parquet('{f.as_posix()}',hive_partitioning=false)").fetchone()[0],f.stat().st_size) for f in sorted((root/'indexes').rglob('*.parquet'))])
s+='\n## Limits\n\nOfficial-list overlap remains an unmeasured licensed-source GAP. No float adjustment, committee/sector discretion, complete IPO or spin-off additions; CIK company identity inherits panel links. R3000 includes REITs and some classes deliberately excluded by broad all_common. SC representation and row-key/alignment checks are recorded in Task 2x report.\n'
s+='\n## Input provenance\n'+tab(['input','SHA-256'],list(m['input_manifests_sha256'].items()));p.write_text(s,encoding='utf-8')
p=Path('docs/datasets/share_exchange_history.md');s=p.read_text(encoding='utf-8').replace('397,050 rows, 2,291,842 bytes',f"399,328 rows, {(root/'security_master/share_exchange_history.parquet').stat().st_size:,} bytes")
s=s.replace('Rebuilt 2026-10-03 in tier1-v4 Task 1 (fix round 1: clock restamped,\nrows otherwise identical to the 2026-09-30 build).','Rebuilt 2026-10-03 in Task 2x: dated fresh FINRA repair for NULL frozen-panel exchanges; T1 first-session clock preserved.')
s=s.replace('NULL when the panel `exchange` is NULL (unknown, not OTC).','NULL when both panel exchange and dated fresh FINRA mapping are unknown (not OTC). SC -> XNAS and BZX -> BATS use the shared security_master mapping, dissemination_date < d and age <=45 days. Unsupported and stale classes remain unknown.')
s=s.replace('- Input: the v2 panel\'s point-in-time security flags (`panel/`, manifest SHA-256 in the manifest).','- Inputs: frozen v2 panel flags and security_master/finra_names; both input manifest SHA-256s are recorded. The panel code and data are never rebuilt.')
s+='\n## Task 2x rebuild and provenance\n\nCommand: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.identity_v3 codes`.\n'+tab(['input','SHA-256'],list(C.read_json(root/'security_master/share_exchange_history_manifest.json')['input_manifests_sha256'].items()))
p.write_text(s,encoding='utf-8');con.close()
