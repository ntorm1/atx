# Equity platform improvement program

Started 2026-09-19. Objective: an institutional-quality low/medium-frequency equity
long/short research and trading platform. This is an ongoing objective, not a claim
that ATX currently matches any named firm. No live trading is enabled by this work.

## Working method

Research primary sources, review current code, implement a bounded change, measure
the behavior, and use the result to select the next change. Delegate disjoint work
and keep a durable record here. Prefer focused correctness and integration checks
over a large test-first process, as requested by the owner.

The owner's subsequent priority is to make `atx-impl` the complete working equity
long/short book: signals, combination, optimization, replay, and eventual production
operation. Use that implementation's measured needs to guide `atx-engine`, its
backbone. Prioritize net out-of-sample Sharpe, capacity at a declared AUM, and low
realized turnover. The next research cycle should deliver a reproducible book
baseline and comparable signal/portfolio recipes; it must carry source-quality,
availability, trading-cost, and shorting limitations into its acceptance evidence.
These objectives are evaluation criteria, not achieved performance claims.

Checkpoint 11 (2026-09-20) adds an explicit research execution-price constraint.
An exactly unheld instrument without a finite positive current close receives
`weight=0` before optimization. The original solve union, preferences, decision
eligibility and lagged risk history remain intact. Held missing marks still fail;
no volume filter, price repair or permanent security exclusion is introduced.
The stage opts into `constrained-preference-weekly-observed-close-v3`; the public
helper retains strict requested-mark failure by default. The same-close snapshot
is a research abstraction with unverified order-submission availability, not a
validated closing-auction execution model.

Both changed production translation units compile. All 21 affected allocation
and stage checks pass, including three new cases, in 16.57 seconds. The first
build found a test-only string-versus-JSON comparison error; four explicit string
extractions corrected it, without changing production behavior. Independent
review found no integration blocker. Failure diagnostics now preserve callback
times, security IDs, phases, typed invalid marks and whether a raw candidate was
actually captured. Canonical required-zero reasons, source observation identities
and distinct solver/representation scopes are recorded for every proposal.

The original-window native attempt retains all 189 dates and 38 scheduled
preferences. It returns four certified proposals at decision periods 0/5/10/15.
At execution period 6, two additional equalities prevent entries without current
prices; subsequent exact Hold counts are 24, 59 and 108. An independent checker
verifies all four proposals, actual dollar trades/cash/borrow and 18 complete
intervals. The first 381-position allocation still matches the frozen startup
oracle. The run then fails on a missing held close at period 19, May 1, 2013,
canonical instrument 1063, vendor security 146189 (archive ticker PCS).
That exact held-gap failure is independently reproduced. No fee for an incomplete
borrow interval or completed performance report is manufactured.

Native elapsed time was 32.634 seconds, peak working set 163,254,272 bytes and
peak private commit 152,481,792 bytes. All measured pins remain unchanged. A new
verified archive retains the exact producer executable and 38 selected source/
build/helper files (39 members plus a manifest); dependencies, compiler, full
repository and research data are outside that archive. It preserves executable
and selected source bytes, not a portable complete rebuild environment. Earlier
receipts retain their original hash evidence and are not retroactively promoted
to source archives.

The original source audit identifies the PCS gaps as absent source rows after
April 30, not parser loss or OHLC quarantine. This boundary is consistent with
the [issuer's MetroPCS/T-Mobile completion announcement](https://www.t-mobile.com/news/press/t-mobile-and-metropcs-combination-complete-wireless-revolution),
which describes a reverse split and cash distribution. Handling that event needs
both successor inventory and a cash entitlement. Exact
vendor identity mapping, price/return-factor basis and information timing still
need evidence before applying an event to the native book. Next work is that
security-transition accounting seam, preserving this failed attempt. No Sharpe,
capacity or investment qualification is established.
See the [execution-price design](../reviews/2026-09-20-iteration11-execution-availability-design.md),
[checkpoint receipt](../reviews/2026-09-20-execution-availability-validation.json), and
[next event design](../reviews/2026-09-20-iteration12-security-transition-design.md).

Checkpoint 10 (2026-09-20) connects continuous allocation to explicit TargetWeight,
HoldCurrent and Close instructions. Holds preserve TRI units and marked dollars
exactly; closes charge for the actual closing trade. A versioned machine-precision
rule selects instructions before checking unused entry marks, records every
continuous weight and instruction, and recertifies the complete represented book
under the original economic limits. It is not an economic no-trade band or share
rounding rule. Independent reviews found no blocker; all 52 affected checks pass,
including seven new replay, allocation and reporting cases.

The fresh attempt kept all original dates, instruments, preferences and charges.
Its first continuous solution is identical to checkpoint 9. Representation removes
592 dust positions totaling `3.04435e-25` weight and produces 381 nonzero positions;
the ordinary correction budget is `7.80388e-11`. Actual dollar-sizing roundoff is
recorded separately. The native run still fails on an uncertified new-entry
candidate requiring the missing April 12 GNW price, canonical instrument 604,
security ID 150340. GNW has zero units after the first allocation. Independent
accounting verifies that allocation and six observable intervals, reaching the
second proposal boundary without a missing held mark. The second raw proposal
was not retained, so its weight and optimizer result are not independently
reproduced. No completed report, Sharpe or capacity estimate exists.

Native elapsed time was 18.724 seconds, peak working set 163,217,408 bytes and
peak private commit 152,469,504 bytes. All measured input/source/executable pins
remain unchanged. Prior failed attempts and receipts remain intact. Next work is
to review execution availability for new entries and retain rejected-proposal
diagnostics; held missing marks and corporate events remain unresolved.
See the [representation design](../reviews/2026-09-20-equity-execution-representation-design.md),
[independent review](../reviews/2026-09-20-allocation-representation-review.md), and
[checkpoint receipt](../reviews/2026-09-20-execution-representation-validation.json).

Checkpoint 9 (2026-09-20) repairs the engine's active-set QP polishing: refinement
targets the unregularized KKT equations, preserves useful ADMM auxiliary values,
and accepts a coherent primal/dual pair after feasibility and residual checks.
It retains the existing four polish solves, 1,200 ADMM iterations and original
book controls. There are 181 latest passing selected cases, including three new
repair cases. The unchanged dense-PCG reference battery timed out at 120 seconds
and remains incomplete coverage; two obsolete historical digest checks were
replaced by independent KKT and direct inert-feature equivalence checks.

The fresh native attempt kept all 189 evaluation dates and 38 scheduled decisions.
Its first allocation across a 973-name solve agrees with an independent separable
KKT solution to `4.34e-19` over all 1,661 canonical weights; the independently
recomputed objective differs by `5.78e-20`. It trades $20m at $100m starting NAV,
charges $10,000 and leaves approximately $99,990,000 NAV. These are startup
proposal/accounting checks, not return or capacity estimates. The replay then
fails on the original missing April 12, 2013 GNW close (`security_id=150340`,
evaluation period 6), before a second allocation. The triggering weight is numerical
dust (`1.05147e-27`): 592 native nonzero values coincide with exact zeros in the
independent startup solution and total only `3.04435e-25` absolute weight. The
strict replay treats these as holdings. This attempt is preserved without any
threshold or mark repair. Next priority is a declared zero/no-trade representation
rule with full portfolio recertification, before the terminal-cash accounting seam.
Later source gaps must never decide which holdings to remove. No complete report is published.
Native elapsed time was 19.525 seconds, peak working set 163,115,008 bytes and
peak private commit 152,309,760 bytes. All pinned inputs and sources were unchanged.
See the [numerical review](../reviews/2026-09-20-qp-polish-review.md),
[validation receipt](../reviews/2026-09-20-qp-polish-validation.json), and
[next accounting slice](../reviews/2026-09-20-iteration10-terminal-cash-slice.md).

Checkpoint 8 (2026-09-20) added `equity-book`: weekly preferences are allocated
against marked holdings and cash with causal diagonal risk, full-L1 turnover,
gross/name/net limits and post-fee checks. The focused validation has 177 distinct
passing cases, including 24 new cases; two initial analytical failures exposed
auxiliary-row error accumulation and were corrected by tightening internal row
tolerance. The original economic limits and final independent checks remain.

The first native constrained trial preserved all 189 original training dates and
38 scheduled preferences. It failed before its first allocation because the QP
did not meet the tighter row tolerance. It used 164,126,720 bytes peak working set
and 152,473,600 bytes peak private commit; no complete replay or performance result
exists from that attempt. Checkpoint 9 addressed the polish residual system,
dual certificate and feasible-candidate acceptance before repeating the same
window. The original missing-bar and terminal-event problems remain unresolved.
See the [constrained book guide](../../atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md)
and [checkpoint receipt](../reviews/2026-09-20-constrained-book-validation.json).

Active development now lives in `C:/atx/.worktrees/equity-platform` on
`feat/equity-platform-20260920`. An external process stashed the shared tree during
integration. The coordinator recovered 96 scoped tracked files from immutable
stash `8f36c2682a811ecc123d8a9588c57ac3177c7c3a` and copied 70 untracked engine/impl
files into this isolated checkout, excluding database edits. Existing checkpoints
and data artifacts were preserved. FetchContent build directories are local to
the worktree. Cross-worktree cached precompiled headers failed to load; rebuilding
with `CCACHE_DISABLE=1` resolved that build issue without changing source logic.

Graph MCP discovery tools were unavailable in the initial session; code discovery
used targeted file reads. Three agents own disjoint data, risk, and execution
files. For this first repair cycle, they edit the shared tree without builds;
the coordinator alone configures/builds. This is an explicit deviation from the
worktree-pool guidance: automatic pool configuration references modules absent
from this checkout, and only one build owner is used while restoring that path.
The unrelated untracked warehouse sprint plan is preserved.

## Iteration 1: restore measurable correctness

Baseline findings from source inspection:

| Area | Observed failure | Intended acceptance |
| --- | --- | --- |
| Build | Root configuration unconditionally requires four absent modules | Separate equity preset builds the engine and CLI |
| Test discovery | Six risk suites at the tests root are omitted by group globs | Risk target includes the existing cone/QP suites |
| Data | Reporting delay is metadata; readers select by observation date | All aligned/catalog/factor reads honor explicit calendar-day availability |
| Risk | Tracking/robust descriptors bypass the solver; multi-horizon classification also omits sector/capacity | Enforce supported constraints and reject unsupported capacity requests |
| Execution | Rebalance appends target deltas while old partial orders remain | Pending intent agrees with the latest successful target without resetting unchanged latency |

The bounded development checkpoint is accepted for the measured scope below.
The platform goal remains active. Real-data validation and the full dense-oracle
battery are not accepted by this checkpoint.

### Review feedback and preliminary measurements

- Execution: 54 focused tests passed, including nine new partial-fill, cancellation,
  reversal, and latency regressions. Evidence: `build-equity/iteration1-execution.xml`.
- First risk pass: 103 tests passed. The restored
  `RiskQpAugment.MatchesDenseOracleAcrossBattery` was stopped after over five minutes
  to keep the development loop bounded; CTest records that interruption as one
  failure. It is **incomplete**, not accepted as passing. Total initial risk gate
  elapsed time: 340.44 seconds. Evidence: `build-equity/iteration1-risk.xml`.
- Independent review exposed interactions missing from the initial risk fix:
  turnover penalties and capacity limits could disappear on augmented solver
  routes, and a post-solve partial trade could violate a tracking limit. The
  implementation now preserves the exact L1 turnover objective and calibrated
  gross cap, and rejects unsupported partial execution with augmented constraints.
- The equity CLI and shared-memory worker build successfully. The wrapper's
  `check` command resolves `equity-dev` correctly. The environment uses a classic
  vcpkg installation with Arrow 24.0.0 and GoogleTest 1.17.0, not a freshly resolved
  copy of the pinned manifest. Validation therefore describes this local toolchain.
- Builds and tests that use the same executable must run sequentially on Windows.
  An attempted relink during the long risk test failed because Windows locks the
  executing file; no source/compiler error was reported by that attempt.

Component records: [data availability](../reviews/2026-09-19-data-availability-audit.md),
[risk constraints](../reviews/2026-09-19-risk-constraint-audit.md),
[execution intent](../reviews/2026-09-19-execution-intent-audit.md), and
[next financing/accounting iteration](../reviews/2026-09-19-financing-accounting-audit.md).

### Final measurement

Debug clang-cl build completed for `atx-engine-risk-tests`,
`atx-engine-data-tests`, `atx-engine-core-tests`, `atx-impl`, and `atx-shm-worker`.
The CLI help command also exits successfully. Independent cross-agent review
completed for build configuration, data availability, execution replacement, and
the final risk/cost interactions.

The selected CTest gate ran 246 tests: **241 passed, 0 failed, 5 skipped** in
**14.31 seconds**. All **37 new regressions passed**: 13 availability, 15 risk,
and 9 execution. The five skips are the `DataRealPanel` tests requiring an
unavailable security-master/Databento smoke fixture (`ATX_DATA_DIR`). The slow
`RiskQpAugment.MatchesDenseOracleAcrossBattery` was explicitly excluded from this
final gate after its separately recorded interruption; it remains incomplete.

Evidence is in `build-equity/iteration1-final-build.log`,
`build-equity/iteration1-final.log`, and `build-equity/iteration1-final.xml`.
The persisted [validation receipt](../reviews/2026-09-19-validation.json) records
suite counts and evidence hashes. A final comment-only correction to the QP
auxiliary-count explanation does not change executable behavior.

The measured synthetic contracts include:

- February 28, 2024 observations with a two-day lag remain unavailable until
  March 1, including catalog, alignment, and factor-reference access.
- The two-name risk fixture honors tracking error `0.1`, robust-alpha weights
  `(+1/6, -1/6)`, cost-aware weights `(+0.25, -0.25)`, and gross capacity `0.05`
  within the stated numerical tolerances.
- A repeatedly rebalanced, partially filled target reaches `(+500, -500)` shares
  without exceeding its $100,000 gross budget in the flat-price fixture, while
  unchanged orders retain their original latency.

To reproduce the selected gate after building, run from the repository root:

```powershell
$filter = '^(RiskConstraintDispatch|RiskOptimizer|RiskMultiHorizon|MultiHorizonIntegration|RiskCone|RiskElasticity|RiskGarleanuPedersen|RiskKktLdl|RiskQpAugment|RiskQpSolver|DataAvailability|DataDataset|DataAlign|DataCatalog|DataAdaptFactor|DataAdaptPanel|DataAdaptFeature|DataAdaptSignal|CatalogReport|DataBoundaryPin|DataE2EByoCapstone|DataRealPanel|RealPanelRegime|BacktestLoop|ExecSim|BacktestIntegration|BacktestBorrow)\.'
& .\scripts\atx-build.ps1 -Ctest -Preset equity-dev -Args @('--tests-regex', $filter, '--exclude-regex', '^RiskQpAugment\.MatchesDenseOracleAcrossBattery$', '--timeout', '120', '--output-junit', (Join-Path $PWD 'build-equity/iteration1-final.xml'))
```

These are software correctness results on synthetic/local fixtures. No current
out-of-sample investment return, profitability, or trading capacity was measured.

## Checkpoint 2: elapsed financing and consistent post-fill marks

The [financing audit](../reviews/2026-09-19-financing-accounting-audit.md)
identified two concrete accounting errors: a full daily borrow debit on every
data slice, and portfolio marks that lagged execution's permanent market impact.
Three agents implemented and independently reviewed the bounded corrections.

The loop now finances the preceding holdings and marks for the actual UTC
interval before processing the next slice. Opening at EOF incurs no invented
holding period; covering pays for the interval that was held. The first slice
anchors time, repeated timestamps add zero, and weekend gaps count calendar
days. ACT/360 and ACT/365 are supported. The elapsed API rejects a 252-session
basis without a trading calendar, invalid rates, invalid active-short marks,
duration errors, and unrepresentable charges or resulting cash balances. Failed
checked debits do not mutate cash. Existing explicitly daily helpers retain
their legacy contract.

After applying a fill batch, the loop refreshes portfolio marks from the
execution simulator's market book before sizing or sampling. This applies to
both next-slice and opt-in same-slice execution. It makes valuation internally
consistent with the existing impact model; it does not calibrate that model.

The build wrapper also needed a correction discovered by this iteration's new
test file: Windows PowerShell treated CMake's benign stderr `GLOB mismatch`
notice as a terminating error when output was captured. Native diagnostics are
now preserved while the native exit code determines success; application lookup
and the rest of the script retain strict error handling.

Debug builds of the CLI, core/data test targets, and shared-memory worker passed.
CLI help exits successfully. The focused CTest gate passed **120/120 tests**, with
**zero failures or skips**, in **12.82 seconds**. All **29 financing/accounting
cases** passed: 13 elapsed helper, 12 loop borrowing, and 4 post-fill accounting
cases. The remaining selected tests cover portfolio bookkeeping, execution,
permanent/temporary impact, cost integration, and data-to-engine integration.

The analytical fixtures establish $0.578703704 for a one-hour $100,000 short at
5% ACT/360, $41.666666667 over 72 hours, and no charge for a position first opened
at EOF. Separate minute debits agree within the explicitly bounded nano-dollar
rounding allowance. The impact fixture now reports $90,000 cash plus 100 shares
marked at $101, or $100,100 equity, under the existing simulator convention.

The wrapper survived the captured CMake regeneration diagnostic and returned
exit code 1 for an intentionally nonexistent target. The
[accounting validation receipt](../reviews/2026-09-19-accounting-validation.json)
records source/evidence SHA256 hashes and suite counts. Logs and JUnit output are
under `build-equity/iteration2-*`. Prior fixture-dependent and slow-oracle gaps
from checkpoint 1 remain open; this focused gate does not replace those checks.

Reproduce after building:

```powershell
$filter = '^(Borrow|BorrowElapsed|BacktestBorrow|BacktestAccounting|BacktestLoop|ExecSim|BacktestIntegration|Portfolio|PortfolioDeathTest|PermanentImpactPersist|CostIntegration|TempPerm|Phase3cIntegration_BridgeE2E|DataE2EByoCapstone|DatabentoPipelineE2E)\.'
& .\scripts\atx-build.ps1 -Ctest -Preset equity-dev -Args @('--tests-regex', $filter, '--timeout', '120', '--output-junit', (Join-Path $PWD 'build-equity/iteration2-final.xml'))
```

## Checkpoint 3: supplied equity history and coherent price adjustment

The user supplied `C:/Users/natha/Downloads/tbltickerhistory3_10y.zip` for daily
OHLCV, reported shares, and cumulative-return factors. The full streaming profile
found **31,598,499 rows**, 71 columns, and 3,576 dates from 2012-03-26 through
2026-06-15. The original 3.54 GB ZIP was left unchanged. Its SHA256, source-quality
findings, factor-direction evidence, and vendor references are recorded in the
[input audit](../reviews/2026-09-19-tbltickerhistory-input-audit.md).

The existing `load_orats_history` adapter matches this format despite its legacy
name. It now strictly validates representable Gregorian dates, canonical positive
64-bit security IDs, input date order, and duplicate date/ID keys. Duplicate keys
fail before sector filtering instead of overwriting observations. CLI summaries
now expose malformed-row counts. A failed load may leave earlier partitions;
imports therefore use fresh output directories.

All four research OHLC fields now use the same pointwise `raw_price *
cumulReturnFactor` transformation. Invalid operands or products become missing.
Raw close and volume remain available, and liquidity/market-cap screens use raw
prices, traded volume, and reported shares. Shares are not adjusted by this
total-return factor. Existing adjusted panel caches must be rebuilt: O/H/L now
use the corrected basis, and close can change in the last rounding bits.

A bounded-memory [preparation tool](../tools/prepare_tickerhistory.py) creates a
derived ZIP without changing accepted row bytes. It quarantines all rows of
conflicting positive keys, invalid IDs, invalid OHLC/factors/volume, and OHLC
ordering violations. Its versioned manifest records overlapping and exclusive
reason counts, output hashes, source CRC, and unchanged source SHA256. Reported
zero shares and zero volume remain explicitly flagged observations.

For the May 1 through June 15 interval, preparation retained **372,014 of 375,746
rows** and rejected **3,732**: 1,916 invalid IDs, 32 rows from 16 conflicting keys,
one invalid OHLC row, and 1,783 other OHLC-order failures. The native loader kept
all accepted rows with **zero malformed rows**, writing 31 partitions covering
12,737 IDs in about 6.37 seconds. A fixture containing two original conflicting
rows correctly failed with ID 6459818 and date 2026-05-04 in the diagnostic.

The native panel uses a $5 million minimum 21-session average dollar volume,
$1 minimum raw price, and top 200 names by dollar volume per eligible session.
Compaction retained **213 instruments across 31 dates**, with **2,200 eligible
date/instrument cells** after warmup. The 12-field artifact contains 6,593 finite
OHLC cells, no OHLC-order violations, and no infinite field values. The ten missing
bar cells remain missing. A second panel build produced the same complete-file
SHA256, `4cf238d5ea396ead5cda5c5742f55ea93420346f82b56ada5d991c117eaaab6e`.

An independent Python comparison reconstructed the declared date/security order
from the accepted ZIP and verified all **46,151 finite values** across adjusted
OHLC, raw close, volume, and raw-price-times-reported-shares: every value matched
bit for bit, as did all 70 missing field cells. All 6,603 universe mask cells
matched the source-based liquidity calculation, and the APNL checksum passed.
This comparison covers seven fields and membership; it does not validate the
economic meaning or historical availability of the source observations.

Debug CLI/data builds passed. The selected CTest gate ran 42 cases: **40 passed,
zero failed, two skipped**, in **16.24 seconds**. All 13 new loader/adjustment
cases passed. The skips require an absent AAPL security-master oracle and a
deferred survivorship-documentation check; they are not validated contracts.
The preparation tool's three fixture tests also passed. See the
[preparation receipt](../reviews/2026-09-19-tbltickerhistory-preparation.json) and
[native validation receipt](../reviews/2026-09-19-tbltickerhistory-validation.json).

Native reproduction after preparation, using fresh output paths:

```powershell
& .\build-equity\bin\atx-impl.exe load --zip C:/atx/data/tickerhistory_20260501_20260615_20260919/accepted.zip --out C:/atx/data/tickerhistory_native_new/segments --min-date 2026-05-01
& .\build-equity\bin\atx-impl.exe panel --segs C:/atx/data/tickerhistory_native_new/segments --panel-out C:/atx/data/tickerhistory_native_new/panel.bin --start 2026-05-01 --end 2026-06-16 --min-adv-usd 5000000 --top-n-by-adv 200 --min-price 1 --compact-universe true
```

These measurements validate source preparation and native data transformation.
The archive has no per-observation publication or revision timestamps, shares can
lag corporate actions, and APNL v1 does not persist date/security axes. Same-date
universe screening still needs an explicit decision/execution availability rule.
Price and liquidity screens do not establish common-stock eligibility; the source
also contains ETFs and other instrument types that need reference classification.
The short selected interval is not an investment backtest. The separate Databento
smoke fixture remains absent. No investment returns, borrow availability, or
trading capacity were measured.

## Checkpoint 4: identified artifacts across the native pipeline

Historical attachment now retains actual ordered session labels, canonical
positive 64-bit security IDs, selected source paths, and the original column
mapping. Non-prefix compaction applies the same selection to data and identities.
Attachment rejects duplicate present date/security cells, invalid source time
axes, and invalid historical IDs. Optional field augmentation preserves identity
and updates the numeric digest after augmentation.

The APNL v1 payload remains compatible with existing numeric tooling. A required
`.manifest.json` adds exact axes, a versioned recipe, parent hashes, and payload
SHA256. IDs, timestamps, and dimensions use integer strings. The artifact ID binds
the whole manifest body using a domain-separated SHA256. JSON parsing rejects
duplicate keys, excessive nesting, and oversized inputs; the numeric payload's
size and layout are checked before deserialization. Publication uses fresh paths,
an exclusive reservation, and a manifest published last. This provides detectable
incomplete output and content binding, not authentication or guaranteed power-loss
durability. Serialization is versioned and deterministic, without a JCS claim.

Native ingestion can now verify `--preparation-manifest` against the accepted ZIP,
hash the ZIP before and after loading, and publish `_ingestion.manifest.json` after
hashing the resulting segments. Panel construction verifies selected segment
hashes before and after assembly, the loader receipt, and any preparation binding.
Unknown upstream provenance remains explicit when loading older segments.
Incomplete ingestion cannot silently become an unknown-but-accepted source.

Combination, optimization, metabook construction, and reporting now require
identified inputs by default. Joins compare identities and exact parent artifacts,
not just dimensions. Books carry their selected session dates and the original
security mapping; reports verify the bound schedule and combination fit boundary.
Discovery and sweep outputs record their source artifact IDs. Existing synthetic
plain-panel fixtures explicitly select `--allow-unidentified-panels true`; known
and unknown identities cannot be mixed. Legacy incremental CLI construction now
fails explicitly pending an append contract that preserves verified identity.

Peer review also found unchecked floating-point-to-integer sector conversion.
Sector resolution now checks finite, nonnegative, integral, representable values
before conversion, uses valid SIC as a fallback, and otherwise records unknown.
This follows the C++ [floating-integral conversion contract](https://eel.is/c++draft/conv.fpint);
it does not establish a dated instrument classification.

Debug builds of `atx-engine-data-tests`, `atx-impl-tests`, and `atx-impl` passed.
The focused CTest gate selected **94 cases: 93 passed, zero failed, one skipped**
in **32.48 seconds**. All **35 new cases** passed: six axis/compaction, eight
artifact IO, eight pipeline binding, ten ingestion provenance, and three sector
conversion cases. The skip is the existing deferred survivorship-documentation
check. The remaining selected cases cover existing data and pipeline behavior;
the full suite, broad search batteries, and sanitizers were not run.

Using the same prepared subset of the supplied ZIP, native ingestion again kept
all **372,014 rows**, with zero malformed rows, across **31 dates and 12,737 IDs**.
Two fresh panel builds produced the same **31 x 213 x 12** payload and artifact ID:
`ecf0c10bbc12463d67befda20eea878021da350ba83aac67d4235cf5b27e214b`.
The payload SHA256 remains
`4cf238d5ea396ead5cda5c5742f55ea93420346f82b56ada5d991c117eaaab6e`,
identical to checkpoint 3's independently source-validated numeric artifact.
An independent Python verifier reconstructed original source order and confirmed
all date/ID axes, compaction indices, manifest/component hashes, and 31 segment
hashes against the actual outputs.

A predeclared two-expression diagnostic then completed native equal-weight
combination, weekly portfolio construction, and reporting. Its seven book rows
bind exactly to source periods `[0, 5, 10, 15, 20, 25, 30]`, with verified research,
combination, schedule, weights, and fit-boundary parents. Outputs are under
`data/tickerhistory_identified_20260919`; the
[validation receipt](../reviews/2026-09-19-panel-identity-validation.json) pins
the measured sources, executable, artifacts, and build/test evidence.

This is software identity and integrity evidence. Session labels are explicitly
not publication times; historical vintages, common-stock eligibility, borrow
availability, and capacity remain unverified. The report still samples a single
forward return at each book period instead of replaying an entire multi-day
holding interval, so this diagnostic's reported returns/Sharpe/capacity are not
accepted economic measurements. Full signal-source/configuration lineage and
artifact-aware incremental append also remain work. Hashing the executable and
segments increases observed Debug panel time to about 9.54 seconds on this subset;
concurrent runs and an unoptimized build make this unsuitable as a benchmark.

## Checkpoint 5: daily holdings replay and actual-dollar diagnostics

Implemented the daily cash and TRI-unit replay recommended by the
[holding-period audit](../reviews/2026-09-19-report-holding-period-audit.md).
`book::replay_scheduled_targets` holds units between effective rebalances, marks
every observed interval, computes trades from drifting holdings and pretrade NAV,
debits fees on actual absolute dollar trades, and accrues annual short financing
over exact calendar time. The last observation is valuation only. Eligibility
gates desired positions; it never erases an existing position's P&L. Missing held
marks, malformed schedules and nonfinite or insolvent accounting fail explicitly.

Identified `atx-impl report` now uses that engine kernel. Its new
`--replay-execution-delay`, `--replay-trade-bps`, `--replay-annual-borrow-bps`, and
`--replay-day-basis` options distinguish timing, actual trade charges and annual
financing from legacy planning/per-period fields. `run` requires an explicit
replay fee choice when planning costs are nonzero. Outputs include daily ledger,
actual trades, final units, full/post-fit summaries and a manifest binding inputs,
schedule, fit metadata, recipe, executable and output hashes. Fresh publication
preserves incomplete or existing results rather than overwriting them.

Trade liquidity now uses actual traded dollars and 21 strictly prior observations
of raw-price dollar volume, without clipping or an adjusted-price fallback.
Unknown liquidity stays unknown; strategy capacity is unavailable until impact
and execution are replayed chronologically. Post-fit attribution begins with the
first effective decision whose origin is after the fit boundary; that label does
not certify independent strategy selection. Legacy attribution APIs and meta
target formation retain their prior behavior and limitations.

Two integrity defects surfaced during review/integration. Identified panel reads
now hash, validate and decode one owned byte snapshot, closing a replace/reopen
race. Absolute DSL/library paths in the bound weights sidecar made otherwise
identical pipelines produce different report identities. Stable local alpha
labels plus SHA256 of the consumed DSL text restore relocation invariance while
preserving weights and fit-boundary parent bindings.

Debug builds of `atx-engine-book-tests`, `atx-impl-tests` and `atx-impl` passed.
The accepted focused gate covers **105 unique cases, all passed, no skips**:
21 book cases in 1.34 seconds and 84 implementation cases in 31.35 seconds.
All **30 new cases** passed. The first implementation gate had three identity
failures among 80 cases; its evidence is retained alongside the successful
recheck and four additional relevant combiner regressions. No full-suite or
production-speed claim is made.

The original 31-date diagnostic correctly rejected a held KLAC mark at
`securityID=38946`, 2026-06-12. Re-reading the original ZIP member confirmed the
invalid OHLC row and unchanged factor/shares around a ten-for-one split verified
against issuer and SEC filings. This is an open
[source adjustment exception](../reviews/2026-09-19-klac-source-adjustment-audit.md),
not a missing value to fill. Exact prior transformation checks did not establish
economic correctness of the supplied factors.

A separately declared prefix through June 11 was used only to measure software
accounting. Its **29 dates, 212 instruments, 28 intervals and 409 trades** were
checked against an independent 50-digit decimal ledger: 308 daily numeric values
and all trades agreed within $1e-7; the largest observed dollar discrepancy was
below $1e-9. Repeated report manifests were identical. The diagnostic ended at
$964,139.9591809269 from $1m after $1,277.557552149 of trade fees and
$440.095691696 of borrow; these values establish reconciliation, not alpha merit.
The full-window rejection remains part of dataset acceptance.

The [checkpoint receipt](../reviews/2026-09-19-replay-validation.json) pins sources,
executables, failure/recheck logs, native artifacts, decimal verification and
source exception evidence. Final outputs are under
`data/tickerhistory_replay_final_20260919`; report ID:
`579afb153377618b1b298b1286d5d3ed54b3d306d1a2c99f4bb86a8a989fb3f1`.

## Checkpoint 6: first annual equity book, source-mark failure retained

Prepared the original archive for 2012-03-26 through 2013-12-31 under the existing
`tickerhistory-qa-v1` policy: **445 dates, 2,910,733 selected rows, 2,883,147
accepted and 27,586 quarantined**. Four positive date/ID collisions account for
eight rejected rows; invalid IDs and OHLC account for the remaining primary
rejections. Original accepted row bytes are preserved. The accepted ZIP SHA256 is
`6131dc6482ee9e292c541bc14a76c0ba2b86c92582f7ed21cb8452985f509249`
(326,897,120 bytes); preparation recomputed the original compressed ZIP hash and
verified the full member CRC. Outputs are under
`data/tickerhistory_training_20120326_20131231_20260919`.

The [bounded-panel design](../reviews/2026-09-19-bounded-equity-panel-design.md)
separates observed feature history, decision eligibility and scored evaluation
dates. The first declared evaluation begins **2013-04-04**, correcting the assumed
April 1 start before strategy metrics: the source has only 253 preceding
observations on April 1, while both frozen signals require 256. The accepted
context confirms that boundary. The overall training partition remains
2013–2019; no validation or final-holdout strategy metrics have been opened.

Native ingestion retained all 2,883,147 accepted rows and 7,650 source IDs. The
existing builder produced a **445 x 1,661 x 12** context in 66.94 seconds, with
**1,026,482,176 bytes peak working set** and 1,017,806,848 bytes peak private commit.
The conservative preflight was 2,775,826,250 bytes against a 3 GB budget; see the
[allocation-lifetime review](../reviews/2026-09-19-annual-panel-memory-review.md).
The monitor uses Windows lifetime peak counters; its polling guard is not an
instantaneous operating-system memory cap. A second panel builder is deferred.

An independent source comparison reproduced all 739,145 membership bits and
4,909,331 finite values across seven fields (adjusted OHLC, raw close, volume and
reported market cap), with every missing cell matching. The context artifact ID
is `ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079`.
These are source-fidelity measurements, not economic-data certification.

The new [`atx-impl equity-baseline`](../../atx-impl/README.md) command evaluates
the frozen slow-momentum pair on observed history, gates both signals on common
readiness and current eligibility, crops away warmup, equally blends ranked
positions, shapes weekly targets and invokes the daily holdings/cash replay.
It requires training dates and a fresh attempt directory, binds input identity
and resolved settings, measures target exposures, and retains failed attempts.
Bounded panel reads enforce both manifest and opened-file payload limits before
allocation. The payload snapshot still supplies hashing and decoding together.

The focused implementation gate passed **102/102 cases**, including **18 new
cases**, in 38.43 seconds. An initial test-only string/JSON comparison failed
compilation and was corrected before the successful build.

The full **2013-04-04 through 2013-12-31** native baseline produced 189 evaluation
observations and 38 weekly targets, then rejected a required held mark at period
6: **2013-04-12, security ID 150340**. Runtime was 8.96 seconds; peak working set
was 165,318,656 bytes and peak private commit was 154,447,872 bytes. Failure and
intermediate artifacts remain under `data/equity_baseline_training_2013_20260919`;
neither the baseline nor replay has a completed manifest. The evaluation was not
shortened or imputed. Full-window returns, Sharpe and executed turnover are unavailable.

An independent numerical verifier matched all 941,787 cropped price/volume cells
bit-for-bit, 185,108 finite combo cells within 5.85e-18, and all 63,118 target
weights within 9.54e-18. Common readiness excluded 3,892 otherwise eligible cells;
future or currently ineligible storage members did not enter weights. Native
pre-ranking signal magnitudes are not persisted, so the oracle verifies their
end-to-end ranked/blended result rather than a direct raw-signal dump.

All 38 proposed targets meet the declared numerical gross/net/name tolerances.
However, their successive target changes total **9.2187 times NAV**, and 31 of 37
post-entry changes exceed 0.20. These are target diagnostics, not executed
turnover or a hypothetical return continuation. They motivate turnover control
against actual marked holdings even for this slow signal family.

A full scheduled-support coverage audit identified **110 missing required cells
across 34 IDs and 74 dates**; the first matches the native failure exactly. After
that first gap, the inventory is counterfactual data coverage only. The next
[source reconciliation](../reviews/2026-09-19-equity-source-reconciliation-design.md)
will classify these keys against original rows together, preserving existing QA
and the rejected trial. The
[checkpoint receipt](../reviews/2026-09-19-equity-baseline-validation.json) pins
code, test/build logs, context/source verification, memory measurements, numeric
oracle and failed native artifacts. Source economics and book qualification remain open.

## Checkpoint 7: original-source reconciliation

The new `tools/audit_tickerhistory_reconciliation.py` classifies every requested
gap against the original ZIP and accepted input using the preparation tool's
shared, unchanged QA predicates. It binds the failed attempt and artifact axes,
preserves exact source rows and original neighbors in an evidence ZIP, and
reports daily/cumulative-factor residuals without repairing prices or treating
same-vendor agreement as economic verification. It enforces explicit input,
date-group and retained-evidence bounds and publishes a completed manifest last.

The focused Python gate passed **12/12 cases** (nine new, including the shared
predicate regression). Peer review found an adjusted-product overflow edge;
overflow/underflow now yields a noncomparable diagnostic instead of a misleading
finite return. Exact error-field parsing prevents numeric-prefix identity matches.

The measured audit completed in **38.41 seconds**, peaking at **85,217,280 bytes
working set** and 77,402,112 bytes private commit. Of the 110 required gaps,
**104 have no original source row and six were quarantined for OHLC ordering**.
All audited accepted groups match the original QA decisions and row bytes;
there is no observed preparation/panel-loss explanation for these gaps. Evidence
contains 52 original and 46 accepted rows, retaining 41,424 raw bytes. An
independent verifier checked the published identity, ZIP framing, every row hash,
all requested keys and the six OHLC contradictions with decimal arithmetic.

The auditor rehashed the complete original and accepted compressed ZIPs before
and after the pass. Original decompression stopped at the declared context end,
so this pass **does not claim a full original-member CRC check**; it fully read
and checked the accepted member. The earlier preparation's full original CRC
verification remains separately bound evidence.

Outputs are under `data/equity_source_reconciliation_2013_20260919`; audit ID
`7dda241005f6f4f4549bba3acfab1e5982aeff0d71c9bc66443fada7d06965ea`.
The [required-mark audit](../reviews/2026-09-20-equity-required-marks-audit.md)
separates intermittent source omissions from representative issuer/SEC-confirmed
terminal acquisitions. Missing rows cannot be fixed by weakening OHLC checks.
Corporate-event cash/share transformations and verified missing-bar evidence are
now concrete data/accounting requirements. The original trial remains rejected.

Implementation is also proceeding from the measured turnover finding: a replay
policy seam will expose actual marked holdings, while a separate constrained
preference adapter uses decision-time risk and the existing certified solver.
See the [allocation design](../reviews/2026-09-19-marked-holdings-allocation-design.md).
These changes do not retroactively complete the rejected fixed-target trial.

## Checkpoint 12: signed security transition and cash-claim payment planners

The new pure book planners in
[`book/security_transition.hpp`](../include/atx/engine/book/security_transition.hpp)
and [`security_transition.cpp`](../src/book/security_transition.cpp) plan one
mandatory predecessor-to-successor stock conversion with an optional signed USD
cash claim, and a separate identified full payment of that claim. Neither
function modifies holdings or runs a replay. The initial admission mode is
`SyntheticFixture`; historical and physical-share admission modes reject. See
the [transition guide](SECURITY_TRANSITIONS.md) and the
[design](../reviews/2026-09-20-iteration12-security-transition-design.md).

Native measurement took four attempts under clang-cl 18.1.8 (preset
`equity-dev`, static Debug, 4 build jobs, ccache disabled, PCH enabled — no
hygiene/include-clean claim). Attempt 1 failed at the wrapper: CTest `-O` is
an ambiguous PowerShell 5.1 parameter, so CTest never ran. Attempt 2 passed
**3/3** but PowerShell silently bound `--verbose` as a common parameter,
capturing zero measurement lines. Attempt 3 (stop-time source, `-VV`) passed
**3/3** with five `SECURITY_TRANSITION_MEASUREMENT` lines and an independent
comparator pass of 5/5. Attempt 4, against the post-review-fix source, passed
**4/4** (new targeted test group) with five measurement lines and a comparator
pass of 5/5. All four attempts' evidence is retained; nothing was overwritten.
Single-TU check and the `atx-engine-book-tests` target build both exited 0;
CTest `^SecurityTransition\.` ran serially.

An independent comparator,
[`iteration12_native_comparator.py`](../../build-equity/audits/iteration12_native_comparator.py)
(stdlib, no C++ source read, exact Fraction arithmetic), re-derived expected
values for five of fourteen oracle scenarios measured natively: long-canonical,
short-canonical, long-old_scale_8, long-successor_scale_4, and
long-independent_scales_8_4. The remaining nine oracle scenarios are recorded
as not measured natively and are not counted as passed. The comparator's
relative bound is `64 * 2^-52 * max(|actual|, |expected|)`, widened by
leg-derived bounds for near-zero quantities; the transition bridge's
leg-derived bound is 1.71e-12 against an observed native bridge error of about
2.6e-15.

An independent implementation review found 0 Critical, 4 Important and 9 Minor
findings. Three fixes were applied and re-reviewed clean across two rounds:
successor value-before is now derived from the same product as
`validate_values` (accepting a correct-but-not-bit-identical mark within
tolerance, previously falsely rejected when an existing position dominated the
increment); the bridge/accounting reconciliation is restated from the four
retained legs with tolerance scaled to the maximum leg magnitude (removing a
false rejection when a large removed value is compared against a small
value-after); and a symmetric absorbed-addition guard `added(a,b)` — rejecting
`(b≠0 ∧ a+b==a) ∨ (a≠0 ∧ a+b==b)` — now applies at all three addition sites.
A new test group,
`SecurityTransition.AdmittedMarkToleranceAndAbsorbedAdditionsAreEnforced`,
adds regression cases that fail on the pre-fix code. Nine Minors plus five
re-review notes are deferred, including that the accounting identity is a
restatement rather than an independent cross-check and a tolerance-policy
wording drift at `book/security_transition.hpp:134-136`.

Attempt 4's canonical native values carry zero residual against the
comparator. Long case: units added 0.6, claim 24.2946, bridge 0.2946, NAV
136 -> 136.2946, cash after full payment 124.2946. Short mirror case: NAV
64 -> 63.7054, cash after full payment 75.7054.

This is synthetic-fixture admission only: no real corporate action, including
MetroPCS/T-Mobile's 2013-05-01 transition, has been admitted, replayed or
backtested — that attempt remains frozen and rejected. Replay integration,
claims in NAV/allocation certificates, and settled-cash treatment are
checkpoint 13 work, not part of this checkpoint. Numerical netting does not
prove stock-loan discharge; no physical delivery is attested. This is not
throughput, capacity, Sharpe or production-readiness evidence, and no live
trading or broker action was performed or authorized. The
[validation receipt](../reviews/2026-09-20-security-transition-validation.json)
pins the design, guide, headers, source, tests, oracle, comparator, all four
measurement attempts, review rounds and producer executable by SHA-256.

A claims-aware state-ownership design for checkpoint 13 is in progress at the
[replay design](../reviews/2026-09-20-iteration13-claims-aware-replay-design.md);
its first component compiled and passed natively but is mid fix-round after
review and is not validated.

## Checkpoint 13: claims-aware replay seam (minimal-closed)

Checkpoint 13 was MINIMAL-CLOSED on 2026-09-20 by parent ruling after an
alpha-priority re-plan, covering T1 and T2 only. See the
[replay design](../reviews/2026-09-20-iteration13-claims-aware-replay-design.md)
and [checkpoint receipt](../reviews/2026-09-20-claims-aware-replay-validation.json)
(SHA-256 982edfc422961ee3bb4cd9c697712aae905aee81dd0340a66ec50beab7475072,
status `minimal_close_t1_t2_only`).

T1 adds a bounded `ClaimsBookState` with two-phase atomic
`commit_security_transition` and `commit_cash_claim_payment`, plus
`compute_claims_nav`
([`book/claims_state.hpp`](../include/atx/engine/book/claims_state.hpp),
[`claims_state.cpp`](../src/book/claims_state.cpp),
[`book_claims_state_test.cpp`](../tests/book/book_claims_state_test.cpp)),
passing 3/3 natively. T2 adds the claims-aware replay seam
`replay_scheduled_intents_with_events` with `ReplayClaimsConfig`,
`ReplayEventBatch` and `ReplayMandatoryEventPolicy`
([`book/replay.hpp`](../include/atx/engine/book/replay.hpp),
[`src/book/replay.cpp`](../src/book/replay.cpp),
[`book_claims_replay_test.cpp`](../tests/book/book_claims_replay_test.cpp)).
The empty-event-batch path is pinned bit-identical to
`replay_scheduled_intents` at both interval and per-allocation level.
Scenario A (a long receivable transition plus a cash claim payment)
preserves NAV and ledgers; retired predecessor targets are rejected
atomically.

Native measurement covers ten book suites, **62/62** under clang-cl 18
`/W4 /WX` (59 pre-existing plus 3 new). Each task was independently
reviewed, fixed in one round, and re-reviewed clean; all 10 T2 deviations
from the design were accepted, notably: `commit_cash_claim_payment` takes
no `marked_equities` parameter; the event batch for period `p` is
requested at iteration `p-1` because period `p`'s first valuation is the
end mark of `(p-1, p]`; and the mandatory-event policy is structurally
never queried outside `1 <= period < dates-1`.

T3 (engine scenarios B -- short payable with borrow -- and C -- atomic
rejections), T4 (`atx-impl` claims-aware allocation certification) and T5
(`atx-impl` equity-book wiring, movements/claims CSV, `--transitions`
flag) are deferred, not done. The seam is not wired into `atx-impl`;
equity-book output is unchanged. Design section 8 lists eight unmet
preconditions for real PCS admission that T3-T5 do not supply; research
([alpha pipeline research](../../.superpowers/sdd/equity-platform-parent-goal/research-alpha-pipeline.md))
found no forecast evaluation of any deployed signal exists anywhere in
the repository.

Real MetroPCS/T-Mobile 2013-05-01 admission remains rejected. Synthetic
fixtures are not alpha, capacity or production-readiness evidence. No
live trading.

## Checkpoint 14: cross-sectional forecast evaluation (Stage 1)

Checkpoint 14 (2026-09-20) completed cross-sectional forecast evaluation, Stage
1: the first forecast-quality measurement of a deployed signal anywhere in this
program. See the
[design](../reviews/2026-09-20-iteration14-cross-section-ic-design.md)
(Revision 5 plus ruling blocks §11.1-§11.9) and the
[checkpoint receipt](../reviews/2026-09-20-cross-section-ic-validation.json)
(SHA-256 38dd653f7069cb4935b51d003baf9cdf87401434eccc9af5df15e907b319d723,
status `validated_stage1_sign_and_shape_only`). The motivating
[alpha pipeline research](../../.superpowers/sdd/equity-platform-parent-goal/research-alpha-pipeline.md)
found no forecast evaluation of any deployed signal existed anywhere in the
repository before this checkpoint.

The engine gained `plan_cross_section_ic`, `compute_cross_section_ic` and
`apply_calendar_seal`
([`eval/cross_section_ic.hpp`](../include/atx/engine/eval/cross_section_ic.hpp),
[`cross_section_ic.cpp`](../src/eval/cross_section_ic.cpp)), exporting
`kValidationBeginNs` (2020-01-01) and `kSealedBeginNs` (2023-01-01), with
bounded maxima and a heap-free per-date loop; the circular block bootstrap is
byte-pinned. `tests/eval/eval_cross_section_ic_test.cpp` passes 63 native tests
under clang-cl 18 `/W4 /WX`. An independent stdlib exact oracle
(`build-equity/audits/iteration14_cross_section_oracle_v2.py`, 42 cases across
six families) matched natively 42/42 via `iteration14_native_comparator.py`.

`atx-impl` gained the `equity-ic` subcommand
(`atx-impl/src/stage_equity_ic.hpp`, `stage_equity_ic.cpp`), requiring
`--panel --baseline-dir --out --evaluation-start --evaluation-end
--trial-ledger` and accepting no `--config`. It reads the three deployed
signals (`momentum_252`, `momentum_126`, `blend_equal`) from the published
equity-baseline `combo.bin` and never recomputes them. A new hash-chained
append-only trial ledger library (`atx-impl/src/trial_ledger.hpp`,
`trial_ledger.cpp`, format `atx-trial-ledger-v1`) SHA-256-chains entries
including the LF, cross-checks a sidecar, and holds an exclusive lock file.
The ledger at `atx-engine/reviews/trial-ledger.jsonl` now holds two lines,
pre-registered then completed, `trial_id` `iteration14-cross-section-ic-0001`,
N=30 declared trials (3 signals x 5 horizons x 2 forward-return variants).
Stage tests pass 16/16 and trial-ledger tests pass 16/16. See
[`atx-impl/docs/EQUITY_IC.md`](../../atx-impl/docs/EQUITY_IC.md).

The real-data Stage-1 run (attempt 1) used the frozen 2013 training context
(189 observations, 1,661 instruments; window 2013-04-04 through 2014-01-01
exclusive), completing in 27.6 seconds wall and writing
`C:/atx/data/equity_ic_training_2013_20260920` (`ic.csv`, `ic_decay.csv`,
`quantile_spread.csv`, `coverage.csv`, `signal_autocorr.csv`, `seal.json`,
`request.json`, `manifest.json`, `ic_summary.json`). Pre-registered before
computing: horizons `{1, 5, 10, 21, 63}` panel rows, `Q=10` deciles, bootstrap
`B=2000` seed `20260920` block `L_h=max(5,ceil(h/2))`, both forward-return
variants (`DropMissingForward` and `IncludeAuditedTerminalV1` pricing only
HNZ/DELL/MOLX; PCS never applied), the ex-34-audited-ID restriction, the
common-sample prefix of 126 dates, and a cost model of `trade_bps=5`
(constexpr from `equity_allocation.hpp`) plus 365 bps annual borrow ACT/365 on
calendar days from session keys for the gross-2.0 long/short decile book.

Headline, full sample, `DropMissingForward` variant, rank IC by horizon:

| Horizon (h) | momentum_252 | momentum_126 | blend_equal |
| --- | --- | --- | --- |
| 1 | +0.029 | +0.030 | +0.031 |
| 5 | +0.056 | +0.052 | +0.057 |
| 10 | +0.070 | +0.064 | +0.071 |
| 21 | +0.092 | +0.093 | +0.099 |
| 63 | +0.146 | +0.154 | +0.160 |

The Pearson IC bootstrap 95% interval lower bound is above zero from h=5 for
all three signals (at h=1 the lower bound is below zero for `momentum_252`
only). h=63 intervals are null by pre-registered prediction (floor(126/32) = 3
< 10 under the frozen reportability rule; the emitted draw uses 4 circular
blocks). The net decile spread is positive at every reportable horizon
(`blend_equal` h=21: +0.0354 gross, +0.0317 net per period). Variant choice
and the ex-34 restriction move every IC mean by at most 0.0022 and every
decile spread by at most 0.0008; ICIR is more sensitive (up to 0.022 at the
reportable h=21 and 0.080 at the unreportable h=63), which is itself a
statement about how thin 189 observations are. Horizons
overlap, so naive t-statistics are invalid and ICIR is not comparable across
horizons.

Independent review found the parent-executed runner reported `accepted=false`
on attempt 1 solely because it compared recorded path strings (absolute
versus relative) with identical digests; this runner defect is now fixed, and
a versioned re-evaluation
(`build-equity/audits/iteration14-equity-ic-measurement-attempt1-reeval.json`)
recomputes `acceptance=true` from the recorded SHA-256s. No re-run was
performed, to avoid inflating declared trials.

This is sign-and-shape evidence only: 189 training observations of one year
(2013), not accepted alpha, not out-of-sample, no model fitted, IC is not net
P&L, survivorship bias from dropped forward returns is upward, and the
2023-2025 seal is non-vacuous by code and vacuous by data at Stage 1. No live
trading was performed or authorized. Stage 2 (the 2013-2019 panel,
memory-budgeted) is the follow-on.

Open and pre-existing, unrelated to this checkpoint:
`StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` fails
(`invalid stod argument`); `stage_run.cpp` and its test were last modified
before this checkpoint.

## Checkpoint 15: point-in-time universe builder

Checkpoint 15 (2026-09-20) completed point-in-time universe construction,
the input Stage 2 of the forecast evaluation needs before it can run across
2013-2019: the first membership series in this program ranked only on data
at or before each rank date and effective from the next session. It builds
universes and counts them; no IC, no forecast and no alpha claim is made.
See the
[design](../reviews/2026-09-20-iteration15-point-in-time-universe-design.md)
(Revision 3 plus the section 15 implementation rulings; frozen SHA-256
4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8, embedded
by the stage as `kEquityUniverseDesignNoteSha256` and re-hashed by the
runner before every launch) and the
[checkpoint receipt](../reviews/2026-09-20-point-in-time-universe-validation.json).

Receipt: atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json (SHA-256 2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e; status stage1_universe_construction_measured)

Data: seven prepared and natively ingested windows of the vendor archive,
all under preparation policy `tickerhistory-qa-v1` (quarantine every
positive duplicate date/id key). The 2014-2019 windows were prepared and
loaded by `build-equity/audits/iteration15_ingest_tickerhistory.py`
(receipt `iteration15-ingest-20260920-attempt2.json`, status `complete`,
source ZIP SHA-256 `7d2b7a61...` identical before and after every year,
`rows_malformed` 0); the 2012-2013 window is the 2026-09-19 ingestion that
checkpoint 14 also used, re-read as segments only (design section 2.1).
Accepted rows, dates written and distinct ids per window:

| Window | Accepted rows | Dates | Distinct ids |
| --- | --- | --- | --- |
| 2012-03-26..2013-12-31 | 2,883,147 | 445 | 7,650 |
| 2014 | 1,702,593 | 252 | 7,546 |
| 2015 | 1,751,122 | 252 | 7,754 |
| 2016 | 1,868,815 | 252 | 9,008 |
| 2017 | 1,928,461 | 251 | 9,146 |
| 2018 | 1,980,746 | 251 | 9,382 |
| 2019 | 2,079,730 | 252 | 9,512 |

The run attached 1,955 sessions (193 in 2012, then 252/252/252/252/251/251/
252) and 13,369 distinct source ids. The 2012-2013 and 2014-2019
directories were loaded by different `atx-impl` executables (`9415a6ab...`
and `ac3ab17f...`), pinned per directory in the manifest.

Pre-registered before the first run and frozen in design section 6:

| Parameter | Value |
| --- | --- |
| Rank key | median of `close x volume` over the trailing 63 sessions (ADV63) |
| Eligibility | at least 57 of 63 valid bars; raw `close` > 1.0 on the rank session; a bar on the rank session required |
| Cadence | monthly: the last attached session of each UTC month ranks, the next session takes effect |
| Rank dates | 2012-12-31 .. 2019-11-29, 84 rebalances (membership 2013-01-02 .. 2019-12-31) |
| Cuts | `top_n` {1000, 2000, 3000} x band {0.00, 0.10}, six cuts, all reported side by side |
| Band rule | incumbent kept iff rank <= `top_n + top_n * band_bp / 10000` |
| Warmup | 63 sessions; 2012 is warmup only |
| Seal | refuse any segment at or after 2020-01-01 (`RefuseAtOrAfterValidationBeginV1`) |
| Ledger | purpose `point-in-time-universe-construction`, `trial_count_declared` 0 |
| N_14 | 30, unchanged; declared(15) = 0 |

The engine gained `data::PitUniverseBuilder`
([`data/point_in_time_universe.hpp`](../include/atx/engine/data/point_in_time_universe.hpp),
[`point_in_time_universe.cpp`](../src/data/point_in_time_universe.cpp)),
a streaming, allocation-free-after-`create` builder with
`encode_membership_bin` / `decode_membership_bin`.
`tests/data/data_point_in_time_universe_test.cpp` passes 37/37 native tests
under clang-cl 18 `/W4 /WX` after one parent fix (the `membership.bin`
decoder now parses the body before verifying the trailer, so a short read
is `InvalidArgument` and only a well-formed body with a wrong digest is
`Internal`; design section 15.4). An independent stdlib oracle
(`build-equity/audits/iteration15-universe-oracle-v1.json`, 42 cases across
nine families) matched natively 42/42 via
`iteration15_native_comparator.py`, on the first native run and again after
the fix. The data-group regression passes 94/94. `atx-impl` gained the
`equity-universe` subcommand (`atx-impl/src/stage_equity_universe.hpp`,
`stage_equity_universe.cpp`), requiring `--segments-dirs
--preparation-manifests --out --rank-start --rank-end` and accepting no
`--top-n`, `--band`, `--rank-key`, `--cadence` or `--config`; the trial
ledger validator now accepts `trial_count_declared` 0 only for an
allow-listed non-trial purpose. Impl tests pass 60/60 after one parent fix
in the test loop (a range-for copying `fs::path`). The runner
(`iteration15_run_equity_universe.py`) crashed after the stage had finished
(a lineage note stored in its pin map); outputs and ledger were intact, the
runner was fixed, and a versioned re-evaluation
(`iteration15-equity-universe-measurement-attempt1-reeval.json`,
`accepted_reevaluated` true, no reasons) recomputed acceptance from the
recorded digests. The stage exit code was not captured and is inferred 0
from the terminal evidence (manifest `complete`, ledger `completed`, no
`failure.json`, empty stderr). No re-run was performed; a re-run would add
two ledger lines for no new information. See
[`atx-impl/docs/EQUITY_UNIVERSE.md`](../../atx-impl/docs/EQUITY_UNIVERSE.md).

The real run (attempt 1) wrote
`C:/atx/data/equity_universe_pit_2013_2019_20260920` (`membership.csv`
94.1 MB, `membership.bin` 12.1 MB, `delisting.csv`
26,004 rows, `churn.csv` 504 rows, `coverage_by_year.csv`,
`union_by_year.csv`, `survivorship.json`, `request.json`, `seal.json`,
`manifest.json`; `universe_id` `7f54d392...`, producer `atx-impl.exe`
`8425fc03...`) in 28.84 seconds wall with a peak working set of 87,539,712
bytes (runner samples at 100 ms) against the 3,000,000,000-byte budget.
The ledger holds two new lines, `iteration15-point-in-time-universe-0001`
pre-registered then completed, checkpoint 15. The seal refused nothing;
the latest attached session is 2019-12-31.

Headline, one-way turnover per monthly rebalance over the 83 non-seed
rebalances (the 2012-12-31 seed rebalance is 0.5 by construction; including
it gives 5.19 / 3.42 / 4.41 / 2.99 / 4.21 / 2.89 percent in the order
below), the cumulative 2013-2019 union (which equals `ever_members`), and
the fraction of ever-members whose last valid bar precedes 2019-12-31:

| Cut | Mean | Median | Mean x 12 | Cumulative union | Ever-members ended |
| --- | --- | --- | --- | --- | --- |
| top 1000, band 0.00 | 4.65% | 3.50% | 0.56 | 2,278 | 467 (20.5%) |
| top 1000, band 0.10 | 2.86% | 1.60% | 0.34 | 2,159 | 432 (20.0%) |
| top 2000, band 0.00 | 3.86% | 2.60% | 0.46 | 4,447 | 1,004 (22.6%) |
| top 2000, band 0.10 | 2.43% | 1.20% | 0.29 | 4,220 | 942 (22.3%) |
| top 3000, band 0.00 | 3.66% | 2.37% | 0.44 | 6,624 | 1,651 (24.9%) |
| top 3000, band 0.10 | 2.32% | 1.07% | 0.28 | 6,276 | 1,547 (24.6%) |

Over the same 83 rebalances the band-0.00 cuts added 3,860 / 6,415 / 9,119
names and dropped 3,014 / 4,692 / 6,454 by rank and 846 / 1,723 / 2,665 by
last bar (top 1000 / 2000 / 3000). Only two rebalances in any cut exceed
10% one-way turnover, both in the data hole described below; excluding them
the means are 3.39 / 1.62 / 2.61 / 1.19 / 2.38 / 1.06 percent. Per-year
exits of the top-1000 band-0.00 members present at each year's first
session are 2.6 / 2.3 / 4.1 / 4.5 / 4.5 / 3.2 / 3.1 percent for 2013-2019,
against the 5.23 percent per year of the Russell 3000 2019 reconstitution
comparison recorded in `survivorship.json`; both are lower bounds.

Coverage by year (year attributed by the rank session, as measured: the
2012 row carries the 2012-12-31 rebalance and 2019 carries eleven; the
union table attributes by the effective session):

| Year | Sessions | Ids seen | Valid-bar median | Eligible median | Rejected rows | Dates with duplicates (keys) |
| --- | --- | --- | --- | --- | --- | --- |
| 2012 | 193 | 7,100 | 6,474 | 5,805 | 6,362 | 2 (2) |
| 2013 | 252 | 7,238 | 6,471 | 5,854 | 21,224 | 2 (2) |
| 2014 | 252 | 7,546 | 6,755 | 6,086.5 | 17,144 | 7 (75) |
| 2015 | 252 | 7,754 | 6,946 | 6,292.5 | 6,807 | 3 (3) |
| 2016 | 252 | 9,008 | 7,437 | 6,522 | 113,928 | 10 (28) |
| 2017 | 251 | 9,146 | 7,815 | 6,930 | 110,959 | 0 (0) |
| 2018 | 251 | 9,382 | 7,983 | 7,062 | 68,873 | 83 (86) |
| 2019 | 252 | 9,512 | 8,258.5 | 7,305 | 51,230 | 0 (0) |

The members median equals `top_n` in every year and cut, and the median
count of members without a GICS code is 0 everywhere. Distinct top-3000
band-0.00 members by effective year are 3,574 / 3,498 / 3,567 / 3,547 /
5,022 / 3,523 / 3,512 for 2013-2019; the 2017 value is contaminated as
explained next.

Data hole, measured and diagnosed, not tuned away. The rebalance ranked on
2016-12-30 dropped 556 / 1,093 / 1,674 top-1000 / 2000 / 3000 band-0.00
incumbents as `drops_last_bar` and replaced them; the 2017-01-31 rebalance
reversed it (one-way turnover 0.498-0.558 in all six cuts at both dates).
The read-only investigation
(`.superpowers/sdd/equity-platform-parent-goal/cp15-idgap-investigation.md`)
found no id remap (0 of 4,034 vanished ids returned under a new id; 4,001
returned under the same id within five sessions, 3,938 on 2017-01-03) and
no delisting wave (5 of the 556 truly ended). The cause is corrupted
open/high/low in the vendor archive on the session before every NYSE
holiday from 2016-01-15 through 2018-02-16 (19 sessions: 9 in 2016, 8 in
2017, 2 in 2018), where `open` equals the prior close on 98.7 percent of
2016-12-30 rows, so roughly half of each such session fails
`ohlc_order_violation` under `tickerhistory-qa-v1` (2016-12-30: 3,957
accepted of 8,102 raw); close and volume spot-check correct. 2016-12-30 is
the only hole that is also a month-end rank date, and the builder requires
a bar on the rank session. Contaminated: `churn.csv` at 2016-12-30 and
2017-01-31 for all cuts; the 2016 `drops_last_bar` totals (614 / 1,214 /
1,846 versus 30-164 in every other year); `union_by_year.csv` 2017
distinct (1,679 / 3,327 / 5,022 band 0.00) and every cumulative column from
2017 on; the 2016 and 2017 `nonmissing_fraction` medians (0.9876, 0.9808);
`valid_observations` of 59-62 for members whose 63-session window holds
hole sessions. Not contaminated: every other rebalance, `delisting.csv`
first/last-bar extents, survivorship per-year fractions. Even accepted rows
on the 19 sessions carry unusable open/high/low. Ruling R15-17: the run
stands as measured because it was pre-registered; the hole is recorded in
the receipt qualifications, here and in the handoff; remediation (a
preparation policy `tickerhistory-qa-v2` accepting rows with valid close
and volume while flagging missing open/high/low, and/or a hole-aware rank
rule ranking on the last full session at or before the rank date) is a new
pre-registration for checkpoint 16 or a checkpoint 15b, and the frozen
design is not edited.

What this does not establish: no alpha, no forecast, no Sharpe and no
capacity, since membership lists are inputs to a future measurement; no
float or common-stock eligibility (ETFs, ADRs, preferreds and funds can
rank in; `instrument_type_eligibility` is `unknown` and GICS gaps are
reported, not applied); survivorship fractions are lower bounds (archive
backfill policy unknown, no delisting date, code or return in the archive,
names never in the archive are invisible); `vendor_market_cap_usd` is
`shares x close` with no filing vintage, reported only and never a rank
key; the 2018 thinning from 83 dates (86 rows) with quarantined duplicate
keys is counted, not repaired; securityID reuse (77622) makes a handful of
histories composite. The cumulative unions of 6,624 (top 3000) and 4,447
(top 2000) exceed `kMaxIcInstruments` 4096, and so does the contaminated
2017 top-3000 distinct count of 5,022, so checkpoint 16 must use per-year
contexts or rule on raising the cap before any full-block evaluation. No
sanitizer, static analyser, include-clean build or CI covers this work. No
live trading was performed or authorized.

Open and pre-existing, unrelated to this checkpoint:
`StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard`
remains untouched and failing as recorded under checkpoint 14. The end
review's deferred minors (I-4, I-5, M-1 through M-12) are listed in
`.superpowers/sdd/equity-platform-parent-goal/cp15-end-review.md` and none
changes a measured number.

## Checkpoint 16: first alpha scorecard (lean path)

Checkpoint 16 (2026-09-20) produced the first multi-year, net-of-cost forecast-economics
number on this branch: an annualised net decile-spread Sharpe with a bootstrap interval,
for the three checkpoint 14 signals, on two checkpoint 15 universe cuts, across 2013-2019.
It is a measurement, not a tradeable result: all three signals FAIL the acceptance bars
written before the run, and the checkpoint closes with NO CANDIDATE. See the
[alpha scorecard](../reviews/2026-09-20-equity-alpha-scorecard-cp16.md) (SHA-256
b36ab30b...), its
[receipt](../reviews/2026-09-20-equity-alpha-scorecard-cp16-receipt.json) (6d2c2b50...),
the frozen [design](../reviews/2026-09-20-iteration16-alpha-scorecard-design.md) (SHA-256
a83484037cae527e2fbfba5dce04289742d1c81666b2c03c132f3da9268c04d1) and
[addendum 1](../reviews/2026-09-20-iteration16-alpha-scorecard-design-addendum-1.md)
(7173cc33...).

What was run: 13 (year x cut) cells - the checkpoint 14 configurations UNCHANGED
(`momentum_252`, `momentum_126`, `blend_equal`; horizons {1,5,10,21,63}; both variants;
both restrictions; Q=10; B=2000; seed 20260920; 5 bps trade + 365 bps annual borrow;
N = 30) as AR-7 restrictions, per calendar year 2013-2019, on the checkpoint 15 top-1000
and top-3000 cuts at band 0.00. The 2017 top-3000 cell was pre-declared NOT FIT before any
number existed (union 5,072 > `kMaxIcInstruments` 4096; the cap was not raised), so
7 top-1000 + 6 top-3000 cells ran. Membership is applied ONCE at panel compaction as the
YEAR UNION of the rebalances effective in the year plus the last one effective before it -
NOT as-of, labelled `year-union, not as-of` on every output row. The 19 corrupted
pre-holiday sessions of 2016-01-15..2018-02-16 were run AS-IS on `tickerhistory-qa-v1`,
contamination pre-registered as a prediction and flagged per row (`*` below). 2013 is a
PARTIAL year from 2013-04-04.

RESULT - headline cell (`IncludeAuditedTerminalV1`, restriction `full`), pooled 2013-2019,
net of cost, h = 21, 2.5/97.5 bootstrap interval, rounded from `scorecard.csv`:

| signal | cut 1000 Sharpe [ci_lo, ci_hi] | n | cut 3000 Sharpe [ci_lo, ci_hi] | n |
| --- | --- | --- | --- | --- |
| `momentum_252` | -0.04 [-0.75, +0.72] | 74 | -0.26 [-0.99, +0.69] | 63 |
| `momentum_126` | -0.19 [-0.70, +0.67] | 74 | +0.08 [-0.45, +0.70] | 63 |
| `blend_equal` | -0.23 [-0.80, +0.66] | 74 | -0.10 [-0.69, +0.65] | 63 |

Per-year Sharpe at h = 21 (same cell; `*` = hole-flagged; every per-year cell has
n_obs < 20, so the intervals behind these points are wide and indicative only):

| cut | signal | 2013 | 2014 | 2015 | 2016 | 2017 | 2018 | 2019 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1000 | `momentum_252` | +1.17 | +0.58 | +1.22 | -1.49* | +0.28* | -1.46* | +1.17 |
| 1000 | `momentum_126` | +1.33 | +0.29 | +0.61 | -0.41* | -0.05* | -1.14 | +0.37 |
| 1000 | `blend_equal` | +1.15 | +0.49 | +0.94 | -1.10* | -0.24* | -1.17* | +0.52 |
| 3000 | `momentum_252` | -0.06 | -0.15 | +1.47 | -1.80* | NOT FIT | -0.99* | +0.49 |
| 3000 | `momentum_126` | +0.99 | +0.13 | +0.76 | -0.77* | NOT FIT | -0.06 | -0.09 |
| 3000 | `blend_equal` | +0.42 | +0.05 | +1.12 | -1.19* | NOT FIT | -0.55* | +0.05 |

Supporting rows at h = 21: rank-IC mean 0.028/0.032/0.030 (top-1000
`momentum_126`/`blend_equal`/`momentum_252`) and 0.025/0.023/0.017 (top-3000); implied
turnover (1 - rho_rank, lag 1) 0.0012-0.0068; breadth (mean names used) 998.0 at top-1000
and 2,772.9 at top-3000; sign stability 2/7-4/7 and 2/6-4/6.

ACCEPTANCE BARS (R16-8, written before the run; passing = candidate, not tradeable): bar 1
= pooled `ci_lo` > 0 at h = 21 on BOTH cuts; bar 2 = sign stability n/n; bar 3 = turnover
and breadth printed. All three signals FAIL bars 1 and 2 on both cuts and PASS bar 3.
Verdict: NO CANDIDATE. Nothing was tuned in response - no horizon, variant, cut,
restriction, annualisation or offset re-picked, no year dropped.

Post-hoc diagnostic, NOT pre-registered and NOT a trial (parent-computed after the verdict;
headline cell, h = 21, pooled): gross vs net Sharpe - top-1000 `momentum_252` +0.11/-0.04,
`momentum_126` -0.06/-0.19, `blend_equal` -0.12/-0.23; top-3000 `momentum_252` -0.09/-0.26,
`momentum_126` +0.24/+0.08, `blend_equal` +0.06/-0.10; mean cost drag about 38-40 bps per
21-session period. Reading, as diagnostic and not evidence: the GROSS spreads are already
near zero over 2013-2019, so the failure is signal, not cost.

Materiality (R16-9 test, R16-26 ruling): the pre-registered test - a 2016 or 2017 per-year
Sharpe outside the union of the 2015 and 2019 intervals on the same cut - FIRES for
`momentum_252` on both cuts and does not fire for `momentum_126` or `blend_equal`. That
AUTHORISES a pre-registered attempt 2 (`tickerhistory-qa-v2` re-ingest or a hole-aware rank
rule); it was NOT started, because the test cannot separate the archive hole from a genuine
2016 momentum reversal and the per-year cells carry about 12 observations each.

Code delta - three edits, all panel/baseline side; `equity-ic`, `stage_equity_ic.cpp` and
the engine IC unit are untouched and the recipe version is unchanged. (1) The panel
allow-list: `HistoryDataConfig::allow_ids` plus
`HistoryPanel::allow_list_excluded_columns`, zeroed into `in_universe` before compaction
(`history_panel.cpp` step 5a), reached by three new `panel` flags `--universe-membership`
/ `--universe-cut` / `--universe-eval-start` (all three or none) with additive recipe keys
`universe_membership_sha256`, `universe_cut`, `universe_eval_start`, `allow_list_size`,
`membership_rule`; covered by one new `DataHistoryPanel` test and two new
`AtxImplPanelMembership` tests. (2) R16-24, baseline pin widening:
`stage_equity_baseline.cpp`'s context-recipe pin now accepts EITHER the checkpoint 14
screen OR the checkpoint 16 membership recipe, nothing in between - this drifts the
checkpoint 13/14 receipts' source pin on that file, recorded here and never patched into a
receipt. (3) R16-25, a signals-only baseline commit for membership contexts, because the
PIT top-3000 admits thin names with missing closes while the replay's
`held_missing_price_policy` is `reject-entire-run`, and `equity-ic` consumes only
`evaluation.bin`, `combo.bin` and their parentage. Those directories commit `status =
"complete-signals-only"`, `qualification = "not-attempted"` and `replay =
"skipped-membership-context-signals-only-no-book-result"`:
`C:/atx/data/equity_scorecard16_base_*` are NOT book baselines and must never be cited as
one. Checkpoint 14 screen contexts are unchanged and still run the full replay.

Memory and timing, measured over all 13 cells plus the measure-first 2015 top-3000 cell:
`panel` wall 69-92 s, peak working set 2.51-2.76 GB against a 3 GB budget - far above the
design's 1.46 GB extrapolation, because the peak is set by roughly 9k source columns x
~510 dates before compaction, not by the compacted K; baseline signals-only 5.5-17.8 s at
383 MB; `equity-ic` 26.6-134.8 s at peak <= 0.38 GB. Contexts are 445-511 dates x
{1,228-1,697 | 3,544-3,608} instruments, matching the predicted unions exactly and
respecting the 4,096 cap. The one-off span ingest took 754 s to prepare (peak 106 MB) and
615 s to load (peak 80 MB), 1,386 s total.

Ledger accounting: the canonical `atx-engine/reviews/trial-ledger.jsonl` was not opened and
still holds its 4 lines (checkpoint 14 N=30, checkpoint 15 declared 0). Checkpoint 16 wrote
to a separate hash-chained sidecar `trial-ledger-cp16-restrictions.jsonl`, now 26 lines
(13 cells x pre-registered + completed). Because the binary is unmodified, every sidecar
line carries `checkpoint 14`, purpose `training-only-forecast-evaluation` and
`trial_count_declared 30`: that 30 is the binary's compile-time constant, NOT 30 new
trials, so the sidecar's `declared_trials_for_checkpoint` reads 390 - an artefact that must
never be used as an `N`. The 13 cells are AR-7 restrictions of the same 30 configurations,
published side by side and never selected between, so N_14 STAYS 30.

Evidence: receipts `build-equity/audits/iteration16-cells-attempt1-full.json`
(`all_run_cells_ok` true), `...-measure2015e.json` and
`iteration16-ingest-span-2012_2019-attempt1.json`; scorecard outputs
`C:/atx/data/equity_scorecard16_scorecard_20260920/` (`scorecard.csv` 1,080 rows, SHA-256
9bab19ff..., plus `scorecard.md`, `receipt.json`); per-cell inputs
`C:/atx/data/equity_scorecard16_{ctx,base,ic}_{year}_t{cut}_20260920/`; logs
`build-equity/audits/iteration16-cells-logs/`,
`iteration16-cells-{full,measure2015}.*.log`, `iteration16-ingest-span-run.*.log`,
`iteration16-build-atx-impl.log`; scripts
`iteration16_{ingest_span,run_cells,equity_scorecard}.py`. All 13 cells were produced by
`atx-impl.exe` SHA-256 d1224b32...; the measure-first cell's panel was built by the earlier
33c6ca8b... (panel code identical; the recipe binds the digest, so the difference is
disclosed, not hidden).

Caveats, all load-bearing and printed beside the numbers in the scorecard: membership is
the YEAR UNION, not as-of, so a name that joined mid-year is admitted for the whole year -
a declared, uncorrected within-year selection look-ahead; the 19 corrupted sessions
contaminate 2016 and 2017 for all signals and 2018 for `momentum_252` and `blend_equal`,
and every pooled row inherits the flag; 2013 is a partial year from 2013-04-04; outside
2013 `IncludeAuditedTerminalV1` collapses onto `DropMissingForward` and `_ex34` is
2013-specific, so the honest per-year configuration count is 15, not 30; the cells are AR-7
restrictions, not new trials; there is no capacity, borrow-availability or impact model -
decile spreads at +/-1.0/n gross-2.0 weights with a flat 5 bps and 365 bps borrow
convention only; per-year cells with n_obs < 20 have wide, indicative-only intervals;
`implied_turnover` is keyed by signal alone in `signal_autocorr.csv`, so one value serves
every horizon of a signal; checkpoint 16 contexts are NOT checkpoint 14 contexts, so the
2013 row is not the checkpoint 14 number and that anchor is printed beside it; the
bootstrap is stdlib Python seeded 20260920, reproducible from the script but not
bit-comparable to any engine interval.

What this establishes: over 2013-2019, on point-in-time top-1000 and top-3000 cuts, the
three deployed momentum-family signals produce a pooled net decile-spread Sharpe whose
2.5-97.5 % interval contains zero at the headline horizon, with a sign that flips between
years. What it does NOT establish: that the family is dead outside this specification (one
horizon grid, one weighting, one cost convention, one universe construction, one
contaminated archive); that the spread is achievable (no borrow, capacity, impact or
fills); anything out-of-sample - 2020-01-01 onward has never been read and 2023-2025 remain
sealed; and it does not carry checkpoint 14's 2013-only numbers forward as alpha evidence -
those are one favourable year inside a seven-year measurement that is flat.

## Next candidates, subject to measurement

1. Post-scorecard decision (checkpoint 16 returned NO CANDIDATE; the
   momentum family's pooled GROSS spread is already about zero over
   2013-2019, so cost reduction is not the lever). Three options, to be
   chosen by the user rather than drifted into:
   (a) Stage 3 - alpha families beyond momentum, evaluated through the
   machinery checkpoint 16 just built (per-year membership-restricted
   contexts, the pre-registered Sharpe recipe, the same bars). This is
   the option the scorecard argues for: the measured failure is signal,
   not implementation.
   (b) Hole remediation attempt 2, authorised by ruling R16-26 because
   the materiality test fired for `momentum_252` on both cuts, and NOT
   started: a `tickerhistory-qa-v2` re-ingest accepting valid
   close/volume with OHL flagged, and/or a hole-aware rank rule on the
   last full session, as a fresh pre-registration under a versioned
   attempt. It buys clean 2016/2017 rows; it cannot by itself turn a
   flat seven-year result positive.
   (c) Retire the lean shortcuts if the numbers are to be defended
   harder: an as-of membership predicate in `equity-ic` instead of the
   year-union allow-list, a checkpoint 16 ledger mode writing to the
   canonical ledger, an oracle suite and a separate receipt writer, and
   a design review. This changes no signal; it removes the declared
   within-year selection look-ahead and the sidecar-ledger objection.
   Decay-driven cadence, hysteresis and turnover pricing (research D-3)
   still follow only after a candidate exists.
2. Complete checkpoint 13's deferred T3-T5: engine scenarios B (short
   payable with borrow) and C (atomic rejections), `atx-impl` claims-aware
   allocation certification, and `atx-impl` equity-book wiring
   (movements/claims CSV, `--transitions` flag), per the
   [claims-aware replay design](../reviews/2026-09-20-iteration13-claims-aware-replay-design.md).
   Design section 8 lists eight unmet preconditions for real PCS
   admission that these tasks do not supply. Then attempt admission of
   the frozen MetroPCS/T-Mobile 2013-05-01 book failure against it.
3. Build the first working `atx-impl` book from the reviewed
   [equity book baseline](../../atx-impl/docs/EQUITY_BOOK_BASELINE.md): fixed slow
   momentum signals, equal/single/covariance combiner controls, 12 declared
   cadence/hysteresis recipes, actual turnover/cost comparisons and a sealed final
   period. First implement memory-bounded training panels with observation history
   distinct from trading eligibility, evaluation-only decisions and explicit
   warmup. Preparing 2012-03-26 through 2013-12-31 begins the first training block;
   no validation or final-holdout strategy metrics are opened by this preparation.
4. Use that working book to drive past-only risk, actual-holdings turnover/trade
   constraints, exact final portfolio feasibility, and fitted-model application
   across evaluation blocks. Existing position shaping can lose neutrality after
   caps; whole-panel risk/conviction and final-date participation are not qualified
   book paths. Keep fees, borrow and liquidity assumptions explicit at the $10m,
   $100m and $1bn provisional evaluation sizes; these are not capacity claims.
5. Resolve source economic exceptions and define publication, universe-decision
   and execution times before accepting performance. Identity and one-observation
   delay do not establish original vintages, common-stock/listing eligibility,
   delistings or borrow availability. Complete signal-library/resolved-configuration
   lineage and trial accounting. Meta allocation still needs completed prior
   holding returns and capital-rotation trading; the legacy capacity backcast must
   not become a strategy estimate.
6. Correct historical bar information timestamps. The
   [bar availability audit](../reviews/2026-09-19-bar-availability-audit.md)
   confirms that Databento interval-start timestamps currently reach the generic
   segment feed as though they were completed-bar times. Normalize availability
   explicitly, preserve generic timestamp compatibility, and measure the existing
   loader/bridge/feed fixture before trusting replay schedules.
7. Connect point-in-time warehouse publications to an engine dataset manifest,
   including security identities, universe membership, delisting policy, input
   hashes, and availability semantics. Coordinate with the separate warehouse work.
8. Add promotion criteria, paper-trading reconciliation, operational risk limits,
   persistence/recovery, monitoring, and a broker adapter only after those contracts
   are demonstrated. Production deployment and actual orders need a separate scope.
9. Let evidence guide signal quality, covariance calibration, portfolio construction,
   execution scheduling, and throughput improvements.

The [learning availability audit](../reviews/2026-09-19-learning-availability-audit.md)
also records finite-but-unmatured label selection and externally fitted
augmentation reused across CV folds. Its proposed correction is research only;
these issues must be resolved before accepting affected learned-model diagnostics.

## Research references

- [CMake add_subdirectory](https://cmake.org/cmake/help/latest/command/add_subdirectory.html):
  subdirectories are processed immediately; an equity-only configuration must exclude
  absent modules and their install/export rules.
- [SEC EDGAR APIs](https://www.sec.gov/search-filings/edgar-application-programming-interfaces):
  filing dissemination differs from the period described by a fundamental fact.
- [Cvxportfolio constraints](https://www.cvxportfolio.com/en/stable/constraints.html):
  portfolio policies need explicit leverage, exposure, position, and turnover limits.
- [Cvxportfolio holding costs](https://www.cvxportfolio.com/en/1.3.1/costs.html):
  financing depends on the time held, including weekends; dividends must not be
  counted twice when returns already include them.

The linked component audits document the sources and limits of each implementation.
