from pathlib import Path
import json
# Prepare documentation from the published validation and verification artifacts, under the guard.
from atx_db.alpha_panel import common as C, universe as U
root=C.build_root();v=C.read_json(root/'validation/universe.json');manifest=C.read_json(root/'universe/manifest.json')
p=Path('docs/datasets/universe.md');old=p.read_text(encoding='utf-8');cols=old[old.index('## Columns'):old.index('## Rule')]
cols=cols.replace('universe-all-common-v1','universe-all-common-v2').replace('or issuer SIC 6770 as of the cutoff (units, warrants, shares)','or issuer SIC 6770 as of the cutoff with zero/missing panel sale_ttm (retained SIC with positive revenue is operating)').replace('cover page as of the cutoff, else profile snapshot','cover page strictly before cutoff; unknown without dated evidence').replace('or BDC (814- file number)','or latest visible BDC election/support event (814- file number), revoked by latest withdrawal')
def table(head,rows):return '\n| '+' | '.join(head)+' |\n|'+ '|'.join(['---']*len(head))+'|\n'+''.join('| '+' | '.join(str(x) for x in r)+' |\n' for r in rows)+'\n'
s='''# universe (`universe/`)

Rebuilt 2026-10-03, tier1-v4 Task 2 fix round 1 and Task 2x. Rule `universe-all-common-v2`, schema `atx.alpha-panel.universe/v1`; one joinable row per frozen panel key `(session_date, security_id)`. Delisted lines remain through their last panel session. Registry stub `universe.registry.json`; manifest `universe/manifest.json`; validation `validation/universe.json`.

Build command from `C:/atx/atx-db` with `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`:
`.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build`.

'''+cols+'''
## Rule and timing

The imported market_index common filter requires operating common/common_unverified lines, excludes ADR/FPI/REIT/LP/royalty trust and indexes, and requires NYSE/Amex/Nasdaq/Arca/BZX listing. Universe adds raw close >= USD 1, positive known shares_daily shrout, no preferred/warrant/unit/right ticker suffix, domestic filer regime, and no foreign issuer, blank check, fund/BDC or REIT flag.

NULL frozen-panel exchanges are repaired only by dated fresh FINRA listing evidence: dissemination_date < session d and age <=45 calendar days. The authoritative security_master.EXCHANGE_BY_CLASS now includes SC -> XNAS, BZX -> BATS and IEX -> IEXG. Codes, market_index, indexes and universe use this mapping; all company caps, ranks and membership intervals were recomputed, with no panel rebuild.

available_at = session d 22:00 UTC. Dated incorporation, SIC, filer regime, links, BDC status and index membership require available_at < lake_cutoff(d), the previous calendar session at 22:00 UTC. Incorporation is unknown when no dated cover is visible; an undated issuer_profile snapshot is never a historical fallback. BDC status is the latest qualifying event strictly before the cutoff, with withdrawal prevailing on a tied timestamp. SIC 6770 only supplements SPAC when sale_ttm is zero/missing, matching the panel rule. Code intervals additionally hold session d and are stamped one second before their first-session cutoff.

T1-b remains the accepted qualification: frozen-panel FINRA classifications can be visible at the prior cutoff, its CIK links can use d 22:00, and Nasdaq directory classifications are a current snapshot. No extra incorporation exception is introduced. Daily-stage consumption uses universe_as_of(lake_cutoff(s)); the strict inequality means the latest stored market row is generally s-2. Formation-date research joins use d close for holding from d+1.

## Measured gates
'''
g=v['gates'];s+=table(['gate','result','measurement'],[(k,'PASS' if z['pass'] else 'FAIL',json.dumps({a:b for a,b in z.items() if a not in ('criterion','monthly','file')},sort_keys=True)) for k,z in g.items() if k!='daily_no_price_screen'])
s+='\n## Published rows\n';s+=table(['year','rows','bytes'],[(y,n,(root/'universe'/f'year={y}'/'universe.parquet').stat().st_size) for y,n in manifest['receipt']['rows'].items()])
s+='\n## Mean daily line counts\n';names=['sessions','all_common','r3000p','r1000p','r2000p','sp500p','r3000p_in_all_common','is_adr','is_reit','is_spac','is_cef_etf','is_foreign_issuer'];s+=table(['year']+names,[[y]+[r[k] for k in names] for y,r in v['counts'].items()])
s+='\n## French NYSE comparison (value only, before 2023)\n';s+=table(['month','French firms','our lines','our companies','line deviation','company deviation'],[[r[k] for k in ('month','french','lines','companies','dev_lines','dev_companies')] for r in g['french_nyse']['monthly']])
s+='\n## Coverage: 30 core fields\n\nbeta_input and idio_vol_input measure return-input sufficiency, not actual beta or idiosyncratic-volatility values. Task 6 must publish actual-field coverage. All coverage after 2022 is value/presence checking; no return evaluation is used.\n'
for universe in U.COVERAGE_UNIVERSES:
 s+='\n### '+universe+'\n';ys=[str(y) for y in U.GATE_YEARS];s+=table(['field','source']+ys,[[n,src]+[v['coverage'][y][universe].get(n) for y in ys] for n,src,_ in U.FIELDS])
s+='''
## Known limits and residuals

The French accuracy threshold remains +/-5% and the daily band remains 3,200-4,800. Failed observations remain FAIL in the manifest; source/class diagnostics and month-by-month residuals are in `.superpowers/sdd/tier1-v4/t2-french-reconciliation.json` and task-2-report.md Fix round 1. Exclusive exclusion classes partition our observed NYSE lines; they do not identify the unknown CRSP counterpart of a residual. A licensed line-level comparator is unavailable. Missing historical incorporation is an explicit source-coverage limit.

Index flags are proxies with no licensed official-list overlap, no float adjustment, and incomplete IPO/spin-off additions. They include REITs and other classes deliberately outside all_common. Market-index aggregate returns stop at 2022 under R5; membership and universe/value coverage continue through the latest panel session.

ADR precision is reported per stratum and cannot be inferred from the pooled result. Hand verdicts must match unique sample identities, seed, rule and sample hash; classifier changes invalidate old verdicts until evidence is rechecked. No automatic inheritance of the prior 97/100 claim is permitted.
'''
s+='\n## Input manifest provenance\n'+table(['input','SHA-256'],list(manifest['input_manifests_sha256'].items()))
p.write_text(s,encoding='utf-8')
