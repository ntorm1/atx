# factors

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

The default factors evaluation source is bounded strictly before 2023.
The separate factors_predictor_history stage uses the same frozen kernels solely
for trailing predictor inputs, with explicit construction_asof and purpose.
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

## Review fix round 1 contract (offline)

The required return grid is every selected calendar session for every formed eligible
line. An absent/guarded bar makes its entire period return NULL. Duplicate or
overlapping selected price line/session keys are rejected before construction;
there is no assumed delisting liquidation or implicit shorter window. Issuer and
market weights preserve missing eligible lines. BAB retains monthly formed members.
Monthly signal support also requires every session in every required month.

All cap consumers select the latest strictly visible audit per line/date before
checking qualification. A later failed/revoked record supersedes an older pass;
equal-clock records are rejected as ambiguous. Fiscal ME/training ME consolidate
all eligible issuer lines without multiplying audit vintages or dropping failed lines.

Generation identity includes complete selected files, price parts, RF adapter and
frequency mappings, release bindings, audit/share binding and membership binding,
as well as config/code. Risk accepts own factors only at equal producer release,
selected-source, configuration and code lineage; these identities are checked again
before publication. Exact old snapshots cannot certify a new dependent release.

The native spec also requires `member_equity`: explicit null for unavailable membership,
or `{stage, manifest, sha256, files}` binding immutable session_date/security_id cells.
Missing membership is a source/coverage FAIL. No caller Boolean supplies source evidence.

## Review fix round 2 issuer-cap contract

Every dated eligible class remains in its issuer aggregate, including classes with
unknown or invalid ME. Any NULL, nonpositive, NaN/infinite constituent ME, or invalid
aggregate ME, makes issuer ME NULL and cap qualification false. Period returns,
monthly signals, EG fiscal/training ME and BAB issuer/market history cannot compute
from surviving classes. SQL predicates explicitly map unknown validity to false;
NULL-skipping aggregate behavior cannot certify the remaining class. Invalid basis
has `quality_reason=invalid_eligible_constituent_me` in the period adapter.

R21 source-only construction uses `--construction-asof YYYY-MM-DD` and optional
`--max-new-months N`, with output directory `factors_predictor_history`. Immutable
month receipts resume only the same pinned as-of generation. RF requires a known
clock strictly before the pinned prior-session cutoff; factor availability includes
that RF clock. Unknown RF clocks leave return factors missing. This path provides
no performance assessment, replication or forward labels. Actual gates remain
unmeasured and FAIL; no production source-quality claim is made.
