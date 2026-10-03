"""Write owned static documentation/configuration only; no lake reads."""
import json
from pathlib import Path
from atx_db.alpha_panel.factors import CONFIG,config_hash

root=Path('C:/atx/atx-db/docs/datasets')
(root/'factors.config.json').write_text(json.dumps(CONFIG,indent=2,sort_keys=True)+'\n',encoding='utf-8')
(root/'factors.md').write_text('''# factors

Offline implementation checkpoint; real source qualification/build/acceptance remain pending.
Code: `factors.py`, `_factor_native.py`, `_factor_sources.py`, `_factor_io.py`.
Schema `atx.alpha-panel.factors/v1`; keys `(freq, session_date)`.
Frozen configuration: `factors.config.json`, identical to code CONFIG. No real return
comparisons occurred before this freeze. Configuration changes require a new recorded
rule/version before comparisons; runtime manifest hashes bind exact final inputs.

## Outputs and clocks

Immutable `factors/generation=<identity>/month=YYYY-MM.parquet`, each with one
monthly row and daily session rows. `manifest.json` is a publish-last pointer to
one complete generation. Consumers must resolve its exact files, never glob all
generations. Progress records complete unit hashes; interrupted units resume only
at identical source/config/code identities. Exclusive publisher lock protects the
pointer; a stranded lock requires owner inspection, not automatic deletion.

| Columns | Type/unit | Definition/source/clock |
|---|---|---|
| freq, session_date | STRING D/M, DATE | Return frequency and period-end session |
| available_at | UTC TIMESTAMP | Period-end 22 UTC; inherited price-vintage uncertainty remains unqualified |
| MktRF | nullable DOUBLE/decimal | Certified dated issuer-cap VW total return less explicit period RF |
| SMB/HML/RMW/CMA | nullable DOUBLE/decimal | French independent NYSE 2x3 size/value/profitability/investment spreads |
| UMD/STREV/LTREV | nullable DOUBLE/decimal | Monthly formed 2x3 momentum/reversal spreads; fixed calendar lookbacks |
| q_ME/q_IA/q_ROE | nullable DOUBLE/decimal | Independent 2x3x3; annual June ME/IA, monthly quarterly ROE |
| q_EG | nullable DOUBLE/decimal | Monthly independent 2x3 one-year expected investment-growth forecast spread |
| BAB | nullable DOUBLE/decimal | Monthly ranked beta-distance weights; beta-scaled excess long/short legs |
| n_universe/n_qualified | BIGINT/issuers | Dated available issuer grid and independently qualified issuer count |
| missing_reason | nullable STRING | Required source/formation/portfolio leg missing; no zero substitution |

Formation precedes return dates; accounting uses strict source clocks before formation
22 UTC. Standardized vintages retain available_at in physical identities and select
latest strictly visible issuer/period snapshots. June book equity uses prior calendar
fiscal-year statements and prior December ME. Labels carry through June-to-June;
monthly momentum/reversal/ROE/EG/BAB formation carries through the next month.
Daily and monthly stock returns are aggregated separately. Long-short daily spreads
are never compounded into monthly factors. Missing formed members/legs remain null.
Raw vendor delisting/vintage limitations remain source qualification gaps.

All own aggregate return inputs are bounded strictly before 2023 before aggregation.
The benchmark validator requires all 120 months 2013-01..2022-12, reports missing
months as FAIL, and never shrinks the window. Thresholds are frozen in configuration.

## Explicit native source interface

CLI: `python -m atx_db.alpha_panel.factors --native-spec PATH --destination PATH
--phase build|validate`, under external memory guard <=1 GiB. Native spec must bind
config_sha256 and explicit release objects for fundamentals_v11, compustat_std_v11,
ratios_v11, universe and prices_history, each `{manifest, sha256}`. No old defaults.
Every output SHA/size is verified before use and again before publication.

`sources` must contain prices, universe, funda, fundq, ratios_state, calendar,
cap_quality, liquidity, borrow, industry, rf and eg_training. Each is an explicit
`{stage, manifest, sha256, files:[manifest-relative paths]}` object. Prices can use
`parts` for separately bound prices_history and prices. Funda/fundq/ratios/universe
must match final release identities. RF can use adapter `french_rf` and explicit
frequency_files D/M, or an explicitly normalized `(freq,session_date,rf)` product.
Native observed dates extend the calendar without inferring historical membership.

cap_quality/eg_training may be explicitly null: typed missing relations are used;
no current membership or vendor-share certification is manufactured. Native EG
training derives dated annual outcomes/predictors when no external training stage
is supplied. Forecast fitting requires independently visible outcome and predictor
clocks at each historical fit cutoff, four calendar months of outcome seasoning,
ME-weighted monthly WLS, 1/99 predictor winsorization, and 30 of the prior 120 monthly
fits. Missing REVT remains null: inspected standardized sources have SALE, whose
name is not assumed to prove equivalence. Missing required primitives fail closed.

cap_quality requires an independently reviewed cap_basis_audit manifest, exact
market_shares/universe/prices_history input hashes, hash-verified primary evidence,
per-row evidence/source hashes and strict clocks. A Boolean assertion cannot certify
a row. CIK77159 is excluded until the audit explicitly resolves its share-basis issue;
neither market_shares nor the foreign panel is presumed correct.

Validation spec additionally binds own_factors and benchmarks: each benchmark has
exact manifest SHA, output file, date_column, canonical-to-source column mapping,
and decimal/percent units. Mappings are explicit source contracts, never fitted.
Validation writes a separate immutable-output-independent receipt; it does not
rewrite the producer manifest. Initial producer gates report unmeasured FAIL.

## Formula references and limits

[French FF5 definitions](https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/f-f_5_factors_2x3.html),
[original q5 forecast setup](https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/q5_2020jan_stylerf.pdf),
[q book-equity/ROE definitions](https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/securityanalysis_internetappendix_2019august.pdf),
[q growth primitives](https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/q5_2018nov.pdf),
[original BAB](https://doi.org/10.1016/j.jfineco.2013.10.005).

No new raw data landed. Current inspected historical prices have no dated universe,
CIK or certified share/availability fields, and universe currently starts in 2018.
These are explicit historical coverage failures, not permission to backfill latest
membership. Preferred-stock redemption/liquidation and quarterly deferred-tax
components are not present in the inspected schema; available standardized values
use documented book-equity fallbacks, leaving source equivalence unqualified.
Actual memory peak, build rows/bytes, all benchmark gates and source certification
remain unmeasured until final producers and an authorized exclusive data slot.
''',encoding='utf-8')
(root/'risk_inputs.md').write_text('''# risk_inputs

Offline implementation checkpoint; no real coverage acceptance claimed.
Schema `atx.alpha-panel.risk-inputs/v1`; keys `(session_date,security_id)`.
Immutable per-session Parquet units under one manifest-selected generation.
Code/source/config hashes govern resume; publisher locking and publish-last pointer
prevent incomplete builds replacing an earlier generation. Memory is bounded by
one <=20,000-line cross-section and one <=252-observation line history at a time.

| Columns | Type/unit | Definition/source |
|---|---|---|
| session_date/security_id/cik | DATE/BIGINT/BIGINT | Dated universe grid and issuer link |
| available_at | UTC TIMESTAMP | Session 22 UTC; consume with strict as-of cutoff |
| univ_all_common/univ_r3000p | BOOLEAN | Exact final dated universe denominator |
| beta_252 | nullable DOUBLE | Intercept CAPM OLS on <=252 aligned sessions, min200 complete |
| beta_ff5_MktRF/SMB/HML/RMW/CMA | nullable DOUBLE | Intercept FF5 OLS, min200 complete and full rank |
| ivol_capm_252/ivol_ff5_252 | nullable DOUBLE/decimal daily | Residual standard error with regression degrees of freedom |
| ff49 | nullable BIGINT | Strictly visible classification.ff49_no |
| size/value/momentum/volatility/liquidity/growth/leverage/quality | nullable DOUBLE/z score | Frozen formulas; sample normalization on certified all_common |
| adv_21/adv_63 | nullable DOUBLE/USD | Strictly visible native liquidity |
| spread_cs_21 | nullable DOUBLE/price fraction | Native Corwin-Schultz spread |
| si_to_io/si_shares/ftd_quantity/on_threshold_list | nullable DOUBLE/DOUBLE/DOUBLE/BOOLEAN | Explicit public borrow-proxy pack; no lending fee/utilization invention |
| cap_qualified/risk_missing_reason | BOOLEAN/nullable STRING | Independent economic-basis qualification and unavailable factor history |

Accounting/classification/liquidity/borrow use clocks strictly before the preceding
session 22 UTC. Accounting max age400 days, liquidity10, FTD60 and threshold10.
Daily exposure return windows contain no future observations; guarded vendor returns
are missing. Own factors are exact manifest/config-bound products, never benchmark
history substitutes. No post2022 risk regressions or return-derived momentum/volatility
are constructed under GC2; latest nonreturn primitives can publish. Missing factor
history stays missing. Coverage includes those missing cells on full universe/year
denominators, including every year 2020-2025, so latest completeness gates can FAIL.

CLI: `python -m atx_db.alpha_panel.risk_inputs --native-spec PATH --destination PATH
--start 2020-01-01 --end YYYY-MM-DD`, under memory guard <=1 GiB. Source/release
interface is the same as factors, plus exact own_factors `{manifest,sha256}`.
Each source is reverified before publish-last completion. No legacy prepared Boolean
certification publication API remains. CIK77159 quality exclusion shares the audit
dependency documented in factors.md. Stale/missing accounting, invalid ranks, missing
primitive inputs and insufficient histories remain nullable, never forward filled.

Registry integration must resolve exact manifest files rather than glob generations.
Dataset build rows/bytes/memory and coverage are unmeasured. Final v11/universe
snapshots, independent economic-basis audit and an exclusive guarded build are pending.
''',encoding='utf-8')
for stage,keys in [('factors',['freq','session_date']),('risk_inputs',['session_date','security_id'])]:
    stub={'stage':stage,'path':stage+'/{manifest.generation}/*.parquet','manifest_path':stage+'/manifest.json',
        'files_from_manifest':True,'keys':keys,'schema_id':'atx.alpha-panel.'+stage.replace('_','-')+'/v1',
        'registration_status':'ready_for_Task17_manifest_generation_resolver',
        'asof_macro_sql':f'CREATE OR REPLACE MACRO {stage}_asof(ts) AS TABLE SELECT * FROM {stage} WHERE available_at < CAST(ts AS TIMESTAMPTZ);'}
    (root/(stage+'.registry.json')).write_text(json.dumps(stub,indent=2)+'\n',encoding='utf-8')
print('Frozen config SHA256:',config_hash())
