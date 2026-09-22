# Custom price and liquidity feature research

CF1 creates production warehouse research surfaces over the corrected daily price
warehouse. These eight prespecified candidates complement the fundamentals,
ratios, growth and per-share pipeline. They are not claimed novel, significant or
profitable. The first full-price-universe build, `custom-features-build1`,
completed on 2026-09-20; forward-return evaluation is still pending.

The read-only post-build measurement found **31,934,514 daily rows across 34,224
warehouse security IDs**, covering decision dates 2012-03-27 through 2026-09-17.
Of these, 16,338,033 rows meet the prespecified price/liquidity research cohort.
All rows pass the modeled decision-clock check, with zero invalid input/decision/
entry date ordering. These are warehouse IDs and research eligibility counts,
not certified issuer counts or historical US-common coverage.

| Feature | Finite observations |
| --- | ---: |
| Five-session reversal | 29,593,359 |
| Momentum with recent month skipped | 26,521,953 |
| Volatility-scaled momentum | 28,472,775 |
| Dollar-volume shock | 28,569,192 |
| Close-location pressure | 30,101,514 |
| Range compression | 28,583,137 |
| Liquidity-conditioned reversal | 28,392,687 |
| Compression accumulation | 28,432,813 |

No feature has a nonnull nonfinite value. Missing values remain missing when
their input-history requirements are unmet. The build used 1 GB DuckDB memory,
one thread and 16 sequential partitions; measured process-tree peak was
1.767 GiB under a 3 GiB guard. It retains the corrected source SHA-256
`0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`.
The explicit run timestamp is the modeled observation cutoff, 2026-09-20
22:00 UTC; the inspection's actual UTC timestamp is recorded separately.
Evidence: `.superpowers/sdd/tier1-parity/custom-features-build1-memory.json`,
`custom-features-build1.log` and `custom-features-build1-inspection.json` with its
separate memory receipt. At inspection there were zero forward labels and zero
evaluation records, so statistical significance and production eligibility
remain unmeasured/uncertified.

`custom_features_daily` is wide: one row per observed security/input session,
carrying eight feature columns, coverage counts, a build reference and explicit
decision/entry clocks. Definition metadata, immutable run manifests, daily decile
results and evaluation summaries live in `custom_feature_definitions`,
`custom_feature_runs`, `custom_feature_deciles` and `custom_feature_evaluations`.
Null features, excluded dates and unsuccessful tests are retained.

## Fixed formulas and timing

Let `t` be the previous market session relative to decision date `T`. `A` is the
corrected adjusted close, `P` raw close, `DV=P*raw volume`, `R=(high-low)/P`, and
`CLV=(2P-high-low)/(high-low)`. All windows below are inclusive market-calendar
ranges. Missing security sessions do not shorten a horizon or get filled.

| Feature | Fixed positive-direction value | Minimum valid observations |
|---|---|---|
| Five-session reversal | `1-A[t]/A[t-5]` | All six adjusted closes |
| Momentum with recent month skipped | `A[t-21]/A[t-126]-1` | Exact endpoints; 100 of 106 closes |
| Volatility-scaled momentum | `(A[t]/A[t-63]-1)/(sd(log adjacent returns)*sqrt(63))` | Exact endpoints; 50 of 63 adjacent returns; positive sample SD |
| Dollar-volume shock | `mean(DV,5)/mean(DV,63)-1` | 5 short and 50 long; positive long mean |
| Close-location pressure | `sum(CLV*DV,21)/sum(DV where CLV valid,21)` | 17 valid sessions; positive denominator |
| Range compression | `1-mean(R,5)/mean(R,63)` | 5 short and 50 long; positive long mean |
| Liquidity-conditioned reversal | `reversal/(1+ln(1+mean(DV,63)/1000000))` | Reversal and 50 dollar-volume observations |
| Compression accumulation | `range_compression*close_location_pressure` | Both component features valid |

Adjusted prices must be finite and positive. Raw close must be positive, volume
nonnegative, and derived values finite. Range/pressure require valid high-low
bounds; pressure also requires a strictly positive range. Unknown split-only
factors remain unknown. The cumulative total-return factor is never presented as
a split factor. Source economic adjustment anomalies remain an input limitation.

Decision time is **T at 22:00 UTC**. Each feature uses only observations through
the previous market session, and the hypothetical entry is **the next market
session's close**. Each observation's modeled clock is the later of its stored
availability and noon UTC on the following calendar day. The feature records the
maximum clock over its 127-session input window and excludes any row not known by
decision time. The operational source-load timestamp is retained through the
source/run provenance, not misrepresented as historical delivery evidence.

The noon floor conservatively covers the vendor's stated full-history delivery
at 05:00 US Central on T+1. This is a modeled backfill clock, not verified
historical availability. These source fields and delivery semantics come from the
[TickerHistory3 dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).

## Cohort, labels and inference

The named cohort is
`bar_observed_price5_adv63_1m_v1_not_certified_us_common`: raw price at least $5,
63-session mean raw dollar volume at least $1 million, and at least 50 valid
dollar-volume observations. These rules use only feature inputs known by the
decision. Historical US-common listing membership is **not certified** by these
bar observations; current directory membership is never backdated.

Daily equal-weight deciles are fixed from eligible feature values before returns
are joined, with stable security-ID tie breaks, at least 200 names and 20 names
per decile. Constant cross-sections are explicitly excluded. Missing labels do
not change ranks. Every decile reports formation count, labeled and missing
counts, invalid labels, observed/other terminal counts and endpoint. The labels
come from the production survivorship forward panel, on the **entry** session,
using its corrected-adjusted default mode. The operator must not pass a panel
built in the optional raw-close compatibility mode. Its latest eligible revision
is chosen before validity checks; a later invalid revision cannot resurrect an
older favorable label.

The primary horizon is **21 market sessions**; 5 and 63 are secondary sensitivity
checks and cannot supply primary significance. Train ends in 2020, validation is
2021–2023, and holdout starts in 2024. Labels crossing these boundaries are purged;
the first 63 observed market sessions of validation and holdout are embargoed.
Unmatured labels remain excluded-date records. All eight directions and formulas
are fixed; no holdout direction selection or parameter search is performed.
These horizons align with the existing production panel contract. This correction
from the initial 20/60-session proposal was made before any outcomes were tested;
it does not select a horizon on observed performance. Missing required label
horizons cause an explicit prerequisite error before evaluation.

The reported daily series is the **Q10 minus Q1 horizon-return spread**, not daily
portfolio PnL or an annualized trading Sharpe. Python receives only SQL-aggregated
spread/date/feature/horizon rows. Mean standard errors use Bartlett/Newey–West
weights at lag `horizon-1`, using actual calendar-session differences across gaps.
Two-sided p-values and 95% intervals use a stated asymptotic normal approximation;
fewer than `max(30,2*horizon)` spread dates or degenerate variance yield no
significance result. The complete eight-test primary family receives Holm
adjustment within each split; missing tests remain in that family. Methodological
references are [Newey and West (1987)](https://users.ssc.wisc.edu/~behansen/718/NeweyWest1987.pdf)
and [Harvey, Liu and Zhu](https://people.duke.edu/~charvey/Research/Published_Papers/P118_and_the_cross.PDF).
The feature formulas and thresholds are our research design, not endorsements
from these papers.

Simple 10/25/50 basis-point **per-side** costs subtract `4*cost` from the spread:
long and short legs, each with entry and exit. This is sensitivity analysis and
does not establish realized turnover, execution, borrow availability or capacity.
Statistical/economic screening requires a positive primary spread, Holm p≤0.05,
positive 25bp-per-side result, at least 252 spread dates, at least two annual
buckets with at least 60 dates each and positive means in every available bucket,
and at least 99% label coverage. Only holdout evidence supports candidacy;
train/validation screens are diagnostics. Partial years below 60 dates prevent
this conservative screen from passing.

**Production eligibility stays false in CF1**, separately from the statistical
screen, until historical listing membership, adjustment/identity quality,
historical source vintages and complete terminal/label coverage are certified.
No external certificate or manual override is fabricated. A run with no qualified
signals is a valid publishable research result.

## Operation and resource limits

Apply migration 0313 with the backed-up governed migration workflow. The CLI
requires an existing current warehouse and never initializes migrations. Callers
must first publish the corrected source and build the production adjusted forward
panel. Source SHA256 is operator-supplied artifact evidence; CF1 does not perform
a second bulk ingestion or claim to independently verify that digest.

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/cf1-build-memory.json -- .venv/Scripts/python.exe scripts/build_custom_features.py build --db-path data/warehouse.duckdb --as-of-date 2026-09-20 --run-at 2026-09-20T22:00:00+00:00 --run-id custom-features-build1 --source-sha256 0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae --memory-limit 1GB --partitions 16
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/cf1-evaluate-memory.json -- .venv/Scripts/python.exe scripts/build_custom_features.py evaluate --db-path data/warehouse.duckdb --as-of-date 2026-09-20 --run-at 2026-09-20T22:00:00+00:00 --run-id custom-features-evaluation1 --build-run-id custom-features-build1 --memory-limit 1GB
```

Run commands sequentially in the controller's single heavy-workload slot. The
guard may refuse launch if physical or commit headroom is insufficient. DuckDB
defaults to 1GB and one thread; 16 partitions are sequential, not worker count.
Source calculations and decile ranks stay in DuckDB with disk spill. The daily
table avoids a full-panel ART primary-key index and instead uses a single atomic,
source/version-scoped writer with deduplicated source keys. Builds use distinct
explicit run IDs and timestamps; a failed publication rolls back to the previous
snapshot. Evaluation aggregates are retained by immutable run ID.

With approximately 32 million source rows, wide daily output is of the same
order, minus final two sessions and duplicate keys. Decile output is at most
about 8 features × 3 horizons × 10 deciles × observed market dates (roughly
0.9 million small aggregate rows for 14 years), and 72 summary rows. Full-universe
runtime, memory/disk demand, actual counts and significance remain unmeasured
until the guarded operator run. CF1 does not claim full-scale completion from
the compact focused fixtures.
