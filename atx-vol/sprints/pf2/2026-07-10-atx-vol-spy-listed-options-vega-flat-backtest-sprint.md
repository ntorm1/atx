# atx-vol Traditional SPY Vega-Flat Listed-Options Backtest Sprint

**Date:** 2026-07-10

**Status:** implementation-ready plan. This replaces the earlier quoted-variance
scope. Implementation starts after
`sprints/2026-07-09-american-pricing-portfolio-throughput-sprint.md` is merged and
its correctness and performance gates pass.

**Parent module:**
`sprints/pf2/2026-07-08-atx-vol-qis-dispersion-northstar-workmodule.md`

**Previous sprint:**
`sprints/pf2/2026-07-10-atx-vol-multiname-corpus-qualification-serialization-sprint.md`

## Core deliverable

Ship one reproducible, end-to-end backtest of a traditional long-dispersion book
using **real Databento OPRA data and actual listed contracts**:

```text
short one listed ATM SPY straddle
long listed ATM straddles on the selected S&P 500 constituents
constituent notionals allocated by index weight
index and basket scaled to zero net vega at entry and every roll
per-underlier delta hedged daily
held strikes, expiries, and quantities fixed between rolls
```

The backtest must prove the full production path:

```text
OPRA quote files
  -> point-in-time market inputs
  -> diverse single-name fits
  -> qualification/quarantine
  -> deterministic surface serialization
  -> a separate process reloads only serialized surfaces
  -> actual listed contract selection
  -> vega-flat sizing
  -> American pricing and Greeks
  -> daily delta hedge and monthly rolls
  -> P&L plus independent reconciliation artifact
  -> committed throughput/regression evidence
```

The sprint is not a signal-research sprint. It does not build DSPX, implied
correlation signals, variance swaps, gamma-flat/theta-flat modes, tactical timing,
or a strategy dashboard. Those are follow-on work only after this basic book is
correct, fast, reproducible, and externally reviewable.

---

## 0. Executive decisions

### 0.1 The instrument is an ATM listed straddle

Cboe's public description of traditional long dispersion is the reference
structure: sell an ATM index straddle and buy ATM straddles on component stocks.
This sprint implements that structure with SPY as the index-option proxy and
single-name U.S. equity options as the basket.

Every option in the backtest must be a contract observed in the OPRA source:

- real raw symbol and date-scoped `instrument_id`;
- actual listed strike;
- actual listed expiration timestamp;
- call and put at the same strike and expiry; and
- standard, unadjusted deliverable with the source contract multiplier; and
- source bid/ask at the selection timestamp.

The current synthetic `K = forward_at(target_T)` and
`expiry = valuation_ts + target_T` path is not sufficient. It prices a valid model
point, but it is not a listed option trade.

### 0.2 SPY is an explicit proxy, not SPX

The public Cboe reference uses SPX. The requested deliverable uses SPY. The run and
all reports therefore use the label `SPY listed-options dispersion proxy` and record
these known differences:

- SPY options are ETF options rather than SPX index options;
- SPY options are American-style while SPX options are European-style;
- contract multipliers, settlement, dividends, and early exercise differ; and
- a supplied constituent schedule may approximate, rather than reproduce, Cboe's
  official top-50 basket.

The sprint may compare construction and broad diagnostics with Cboe public material,
but it must not claim that its P&L or any derived level equals COR3M, DSPX, or an SPX
desk book.

### 0.3 The backtest consumes serialized surfaces only

Corpus building and backtesting are separate executable phases. The backtest process
receives:

- a committed run specification;
- the qualified corpus manifest and quality report;
- per-date serialized surface archives; and
- a deterministic listed-contract trade schedule.

It does not receive live `VolaSession`, fitter objects, raw calibration state, or
in-memory surfaces from the build phase. This process boundary is the proof that fit
serialization is production currency rather than a test-only round trip.

Raw OPRA is still read by the independent quote-reconciliation phase so exact held
contracts can be checked against observed mids. It is not used to replace a failed
surface mark inside the primary backtest.

### 0.4 Keep the output basic

The required time series are:

- total and component P&L;
- NAV;
- model and quote-mid P&L reconciliation;
- option and hedge-share P&L;
- net delta before/after hedge;
- net and gross vega;
- turnover at rolls;
- open/unpriced lot counts; and
- fit, archive, and pricing throughput.

No implied-correlation, DSPX, forecast, timing, or allocation signal is required.

---

## 1. Locked trade contract

The implementation may expose parameters, but the acceptance run uses the locked
values below. Changing them creates a different run identity and invalidates direct
artifact comparison.

### 1.1 Universe

The index leg is `SPY`. The basket is an effective-dated set of liquid S&P 500
constituents with positive point-in-time weights.

Two run sizes are required:

| Run | Purpose | Required size |
|---|---|---:|
| Development slice | fast deterministic CI and debugging | SPY + at least 10 names |
| Core real run | sprint deliverable and corpus/performance proof | SPY + 50 names |

The 50-name size mirrors the breadth of Cboe's public implied-correlation reference,
but the report must still identify the schedule source and call it a proxy unless it
is the exact official effective basket.

The schedule is fixed before market data is processed and contains:

```text
effective_date
symbol
raw_weight
source
as_of
```

At each roll, unavailable/quarantined names are dropped and surviving weights are
renormalized. The run fails rather than trades when fewer than the configured
minimum survive. The development slice minimum is 10. The core run requires at
least 40 of the requested 50 names and at least 80% of requested basket weight at
every roll. The report always shows requested, admitted, traded, and dropped weight
coverage.

### 1.2 Observation and run window

The core run must contain:

- at least 60 trading dates;
- at least three completed roll events;
- one close-time OPRA snapshot per date and symbol;
- no future quote, definition, rate, dividend, or weight input; and
- enough profile diversity to include an index/ETF, ordinary liquid names, a
  dividend payer, a lower-price name, and at least one event-month board.

The exact date window is stored in the run specification. No test or example silently
changes the window based on which files happen to exist.

### 1.3 Common listed expiry

At each entry or roll:

1. enumerate standard monthly equity-option expirations observed for SPY;
2. keep expirations whose exact DTE lies in `[21, 60]` calendar days;
3. order by `abs(DTE - 30)`, then earlier expiration;
4. for each candidate, determine which admitted names have a valid call/put pair;
5. select the first candidate satisfying `min_names`; and
6. drop and renormalize the other names with explicit reasons.

SPY and every traded constituent use the same expiration timestamp. Exact expiration
timestamps come from point-in-time contract definitions or a sourced series rule;
the implementation does not assume `date + target_T`.

Weekly, quarterly, and 0DTE contracts are out of the locked acceptance run. Supporting
them later must not change the monthly selector.

### 1.4 Listed ATM strike

For each symbol at the selected expiry:

1. read `F(T)` from the reloaded `PricedSurface`;
2. retain listed strikes with both a valid call and valid put OPRA quote;
3. select the strike minimizing `abs(K - F(T))`;
4. break an exact tie with the lower strike; and
5. preserve both raw contract identities and quotes.

A valid quote is finite, nonnegative, noncrossed, and has positive ask. The selector
does not invent a missing side from put-call parity or the surface. The locked run
rejects adjusted or unknown deliverables; the selected call and put must both be the
standard 100-share deliverable.

This is a transparent ATM-forward listed-strike convention. Cboe's published COR3M
analytics use ATM/delta-relative constant maturity. The reconciliation report lists
this difference rather than calling the conventions identical.

### 1.5 Vega-flat sizing

All internal Greeks use the library's per-unit-decimal-volatility convention. The
human-facing report converts to dollars per one volatility point:

```text
straddle_vega_per_contract_per_vol_point
  = (call_vega + put_vega) * contract_multiplier * 0.01
```

Let `V_I` be SPY straddle vega per contract per vol point, `V_i` the corresponding
constituent vega, normalized survivor weights `w_i`, and the configured gross index
vega target `G`. For classic long dispersion:

```text
q_SPY = -G / V_I
q_i   = +(w_i * G) / V_i
```

The acceptance backtest uses continuous strategy notionals, so quantities may be
fractional while the underlying contracts, strikes, and expiries are real listed
options. This matches index/QIS notional construction and makes the vega-flat
identity directly testable. Integer execution rounding is a later execution feature.

Hard entry/roll gate:

```text
abs(net_vega_per_vol_point) / G <= 1e-10
```

The report also lists each leg's target vega, achieved vega, quantity, multiplier,
and contribution to the residual.

### 1.6 Lifecycle and hedge

- Open on the first valid date.
- Hold every option strike, expiration, and quantity fixed between rolls.
- Roll the entire option cohort when common-expiry DTE is at or below 7 calendar
  days.
- Construct the replacement book completely before closing the old cohort.
- If a roll date cannot build a valid replacement, leave the old cohort intact and
  record no trade; strict mode may instead abort, but may not clear first.
- Delta hedge every underlier independently at each daily observation using the
  existing engine-owned per-uid share ledger.
- Recompute vega-flat quantities only at entry and roll. Between-roll vega drift is
  measured, not traded.

The core backtest is frictionless at midpoint. Bid/ask and calibrated transaction
costs are deliberately deferred until the price/Greek/P&L path is reconciled.

### 1.7 Primary and reference marks

The primary backtest is mark-to-model:

- exact listed contracts;
- reloaded `PricedSurface` American fair values and Greeks;
- daily surface-to-surface P&L explain; and
- `UnpricedLotPolicy::Error` for any held contract without a valid model mark.

The independent reference is mark-to-observed-mid when an exact held contract has a
valid quote on both dates. It is calculated outside the pricing engine from exported
contract keys, quantities, multipliers, and raw mids.

The reconciliation reports:

```text
surface mark - raw mid
whether surface mark is inside [bid, ask]
model P&L - quote-mid P&L
coverage of held lots with valid raw mids
```

Quote-mid P&L is a validation series, not a fallback. Missing raw quotes reduce its
coverage and are reported; they never overwrite model P&L.

---

## 2. What can be verified publicly

### 2.1 Public construction anchor

Cboe publicly describes long dispersion as selling an ATM SPX straddle and buying
ATM straddles on component stocks. Cboe also documents a top-50 value-weighted
component basket and fitted ATM option analytics for COR3M. BNP Paribas publicly
describes practical dispersion implementation with liquid plain-vanilla ATM options
or strike strips, dynamic rebalancing, hedging, and variance-swap proxies.

The sprint maps every locked choice to those sources in `methodology_map.tsv`:

| Choice | Public anchor | ATX adaptation |
|---|---|---|
| Short index ATM straddle | Cboe traditional dispersion description | SPY replaces SPX |
| Long component ATM straddles | Cboe traditional dispersion description | supplied SPY constituent proxy basket |
| Top-50 breadth | Cboe COR3M public description | exact schedule only when operator supplies it |
| Surface-fitted prices/Greeks | Cboe Hanweck public analytics description | atx-vol American fitter/pricer |
| Daily hedging and rolls | BNP public implementation description | daily close, monthly listed expiry |
| Vega-flat sizing | direct Greek identity | continuous notional, actual served vegas |

### 2.2 Numeric verification

There is no public SPY-option dispersion P&L series with the same OPRA snapshots,
constituent schedule, dividends, rates, fills, and roll rules. The plan must not set
an impossible gate that claims exact P&L equality to COR3M or DSPX.

Numeric verification instead has four independent layers:

1. **Public closed-form structure:** selected positions have the published short-index,
   long-components ATM-straddle shape.
2. **Independent book arithmetic:** a small standalone reference calculator reads
   exported TSV rows and recomputes quantities, net vega, daily quote-mid P&L,
   cumulative NAV, hedge-share P&L, and roll turnover without linking `atx-vol`.
3. **Market-data reconciliation:** selected and held contracts are joined back to
   exact OPRA identities and quotes, with surface-in-spread and model-vs-mid errors.
4. **Existing pricing oracles:** all American price/Greek, fit-quality, archive
   round-trip, portfolio/P&L closure, and performance regression tests remain hard
   dependencies.

An optional contextual appendix may place the run window beside publicly available
COR3M levels. It is not a strategy signal and is not a pass/fail gate because SPY and
SPX books are economically different.

### 2.3 Independent reference calculator contract

Add `atx-vol/tools/reference_spy_dispersion.py`. It must:

- use only Python standard-library parsing and arithmetic unless an existing pinned
  repository dependency is already approved;
- read deterministic TSV exports, never surface archives or C++ objects;
- recompute formulas from primitive rows rather than copying C++ aggregate columns;
- fail on duplicate contract keys, missing roll actions, nonfinite inputs, or broken
  date order;
- emit `reference_reconciliation.tsv`; and
- exit nonzero when vega, P&L, NAV, or turnover differs from the C++ export beyond
  the committed tolerances.

The C++ and Python implementations share data schemas and formula documentation, not
implementation code.

---

## 3. Required run artifact

One successful core run produces a self-contained metadata/result directory. Raw
licensed OPRA files remain outside it.

```text
spy-dispersion-run/
  run_spec.tsv
  methodology_map.tsv
  universe_schedule.tsv
  input_inventory.tsv
  surface_manifest.tsv
  quality.tsv
  archives/<date>.atxvsa
  trade_schedule.tsv
  contract_marks.tsv
  backtest.tsv
  reference_reconciliation.tsv
  performance.json
  run_summary.md
```

### 3.1 `run_spec.tsv`

Contains every behavior-changing input:

- run ID and schema version;
- date window and close timestamp;
- symbols and universe fingerprint;
- rate/dividend/definition provenance fingerprints;
- fit policy and admission-policy fingerprint;
- target DTE, DTE window, roll horizon, and minimum names;
- gross index vega target and units;
- missing-name and unpriced-lot policies;
- price route/math mode/thread count; and
- compiler/build/machine identity for performance claims.

### 3.2 `trade_schedule.tsv`

One row per call/put position opened at each roll:

```text
roll_date, cohort, symbol, uid, instrument_id, raw_symbol,
expiry_ts_ns, strike, side, quantity, multiplier,
raw_bid, raw_ask, raw_mid, model_mark,
delta, vega_per_unit_vol, vega_per_vol_point,
target_vega, achieved_vega,
source_fingerprint, surface_archive_fingerprint
```

Rows sort by roll date, index before names, normalized weight order, call before put.
Parsing verifies unique contract keys and aggregate vega.

### 3.3 `contract_marks.tsv`

One row per held contract per date with raw quote and model mark. A status column
distinguishes `Ok`, `NoRawQuote`, `CrossedQuote`, `NoSurface`, and `PricingError`.
The primary run permits only the first three; `NoSurface`/`PricingError` are fatal.

### 3.4 `backtest.tsv`

Preserve existing P&L explain columns and add only the audit columns required by this
book:

```text
cohort
common_expiry_ts_ns
dte
n_names_requested
n_names_traded
weight_coverage
net_vega_per_vol_point
gross_vega_per_vol_point
vega_drift_from_entry
delta_before_hedge
delta_after_hedge
quote_mid_pnl
quote_mid_coverage
model_minus_quote_pnl
```

These are audit/risk columns, not alpha signals.

### 3.5 Deterministic identity

The run ID hashes behavior and source fingerprints, excluding absolute paths, timing
measurements, and worker count where output must be thread invariant. Repeating the
same run at different fit/archive/pricer worker counts must produce byte-identical
manifests, trade schedules, contract marks, backtest TSV, and reconciliation TSV.

`performance.json` is excluded from byte identity because elapsed time is measured.

---

## 4. Current implementation map and gaps

### 4.1 Reuse

- real OPRA CBBO loading, date-scoped instrument identities, source fingerprints,
  per-date rates/dividends, and fit context;
- unified fit routing, diverse-board profiles, qualification/quarantine, and complete
  quality sidecars;
- date-streaming corpus build and one deterministic surface archive per date;
- `PricedSurface` American price and Greeks;
- portfolio contract dedup, deterministic parallel pricing, and P&L explain;
- absolute-expiry `Lot`, lifecycle helpers, daily per-uid delta hedge, friction and
  financing ledgers, and unpriced-lot policy;
- survivor/drop-and-renormalize behavior in dispersion code; and
- the landed benchmark harness and committed performance comparator.

### 4.2 Gaps

1. `dispersion_signal` and `build_dispersion_book` resolve a synthetic forward strike
   and synthetic target tenor, not observed listed contracts.
2. `DispersionStrategy` stamps every position with
   `base.ts_ns + target_T * kNsPerYear`, not a source expiration.
3. the strategy rebuild path does not consume a deterministic external trade
   schedule or retain OPRA identities.
4. current examples are synthetic and do not run the real qualified corpus through a
   separate-process serialize/reload boundary.
5. primary model P&L has no exact-contract raw-mid companion series.
6. there is no independent calculator over exported primitive rows.
7. the benchmark suite measures kernels, but not this complete corpus-to-backtest
   workflow or its orchestration overhead.

---

## 5. Task graph

```text
P0 post-performance audit + freeze run specification
 |
 P1 real OPRA inventory + actual listed straddle selector
 |
 P2 fit, qualify, serialize, and process-boundary reload
 |
 P3 vega-flat schedule construction + independent arithmetic oracle
 |
 P4 fixed-contract strategy + delta hedge + model/raw P&L reconciliation
 |
 P5 core real run artifact + public-reference review
 |
 P6 end-to-end throughput baseline + asserted regression gate
```

The sprint is not complete after synthetic tests. P5's real artifact is the core
deliverable. Paid data acquisition remains operator-approved and cost-capped, but
absence of approval means the sprint is externally blocked rather than complete.

---

## 6. Detailed implementation tasks

### P0 - Integrate performance work and freeze the run

**Goal:** start from the actual landed performance APIs and make the acceptance run
immutable before implementation.

**Files:** this plan, new `examples/spy_dispersion_run_spec.tsv`, methodology mapping,
and progress ledger.

**Steps:**

1. Rebase/merge the completed performance sprint and record its completion commit.
2. Run the full correctness and performance gates before feature changes.
3. Re-audit `PricedSurface`, portfolio prepared/fused routes, archive APIs, backtest
   state reuse, and benchmark registration.
4. Freeze the real window, target close, SPY plus 50-name schedule, rate/dividend
   sources, target vega, DTE/roll rules, and minimum names.
5. Inventory cached OPRA and definition files without downloading.
6. For missing cells, produce a free Databento cost preflight and hard cap. Do not
   pull until the operator approves the exact cost/window.
7. Record the SPY-versus-SPX deviations and what the public comparison can and cannot
   prove.

**Gate:** pre-change tests and baselines green; run spec parses and fingerprints;
input inventory covers every requested cell or records it missing; no paid call.

**Commit:** `docs(atx-vol): freeze traditional SPY dispersion run`

---

### P1 - Select actual listed ATM straddles

**Goal:** replace synthetic strike/tenor resolution with a deterministic selector
over real OPRA contract pairs.

**Files:** new `listed_dispersion.hpp/.cpp`, minimal OPRA definition/snapshot helper
if needed, `vol.hpp`, CMake, and new `tests/listed_dispersion_test.cpp`.

**Core types:**

```cpp
struct ListedOptionRef {
  std::string symbol;
  std::uint32_t instrument_id{0};
  std::string raw_symbol;
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  Side side{Side::Call};
  double bid{0.0};
  double ask{0.0};
  std::int64_t quote_ts_ns{0};
  double multiplier{100.0};
};

struct ListedStraddle {
  std::string symbol;
  std::uint32_t uid{0};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  ListedOptionRef call;
  ListedOptionRef put;
};

struct ListedDispersionSelection {
  std::string roll_date;
  std::int64_t expiry_ts_ns{0};
  ListedStraddle index;
  std::vector<ListedStraddle> names;
  std::vector<DroppedName> dropped;
};
```

**Red tests first:**

1. only an expiry shared with SPY is eligible;
2. DTE boundaries and target-distance/earlier-expiry tie break;
3. monthly-only eligibility;
4. no future quote/definition record;
5. call and put must share strike, expiry, symbol, and multiplier;
6. adjusted/unknown deliverables and non-100 multipliers are ineligible;
7. closest-to-forward strike and lower-strike tie break;
8. invalid/crossed/missing quote side skips the pair;
9. an unknown or date-reused instrument ID cannot cross dates;
10. missing name drops and weights renormalize only after the expiry is selected;
11. index failure is fatal and below-minimum names is unavailable;
12. input row order does not change selection; and
13. every chosen option can be found again by its full economic/source key.

**Implementation:**

1. Build one point-in-time contract snapshot per date/symbol from row-aligned OPRA
   identities and definitions.
2. Key identity by `(trade_date, instrument_id, raw_symbol)` and economics by
   `(symbol, expiry_ts_ns, strike, side, multiplier)`.
3. Apply the locked common-expiry and ATM-forward rules exactly.
4. Return explicit per-name drop reasons; expected market unavailability is data,
   contradictory identity is an error.
5. Keep this module pure: it selects contracts and never fits or prices a surface.

**Gate:** synthetic adversarial matrix green; a cached one-date SPY plus 10-name
snapshot selects only real OPRA identities and exact expiries.

**Commit:** `feat(atx-vol): select listed SPY dispersion straddles`

---

### P2 - Fit, qualify, serialize, and reload across a process boundary

**Goal:** make the real multi-name fitted corpus the only pricing input to schedule
construction and backtest.

**Files:** existing OPRA batch/corpus APIs where necessary, new
`examples/spy_dispersion_backtest.cpp`, `tests/spy_dispersion_pipeline_test.cpp`, and
run artifact helpers.

**Steps:**

1. Add a `build-corpus` command that loads real OPRA cells from the frozen inventory,
   applies point-in-time inputs and fit context, and calls the qualified streaming
   corpus builder.
2. Require source provenance and the profile-specific quality policy for the core run.
3. Write the normal surface archives, manifest, and quality report. Do not create a
   second surface format.
4. Exit the build process after writing and verifying the artifact.
5. Add `build-schedule` and `run-backtest` commands that open only the persisted
   manifest/quality/archive files for surfaces.
6. Assert that mapped/reconstructed surfaces match the build-time reference on a
   deterministic contract sample.
7. Join raw selector inputs to surfaces by date, canonical symbol, valuation
   timestamp, and source fingerprint. A mismatch is fatal.
8. Produce a complete requested/admitted/quarantined/missing/failed scoreboard.

**Red tests:**

- build and run in one process is not the acceptance seam;
- corrupt archive/manifest/quality/source fingerprint fails;
- quarantined surfaces cannot enter a schedule;
- every selected contract prices after reload;
- output is invariant across fit/archive worker counts; and
- peak live fitted surfaces stays within the streaming bound.

**Gate:** SPY plus 10-name development corpus builds in process A; process B reloads
it, prices every selected listed contract, and has no live fitter/session dependency.

**Commit:** `feat(atx-vol): drive SPY dispersion from serialized corpus`

---

### P3 - Build and verify the vega-flat trade schedule

**Goal:** size the selected listed straddles on reloaded American surfaces and persist
the exact book opened at every roll.

**Files:** `listed_dispersion.hpp/.cpp`, new `listed_dispersion_schedule.hpp/.cpp`,
`dispersion.hpp/.cpp` only for shared survivor/sizing helpers, tests, and the Python
reference calculator.

**Red tests first:**

1. each straddle's vega equals explicit call-plus-put surface vega;
2. per-vol-point unit conversion includes multiplier and `0.01` exactly once;
3. index quantity is negative and constituent quantities positive;
4. each constituent achieved vega equals normalized weight times gross target;
5. aggregate relative residual is `<= 1e-10`;
6. reversing side changes signs only;
7. invalid/nonpositive vega, multiplier, target, or weight is rejected;
8. schedule serialize/parse round-trips full precision;
9. schedule rows/fingerprint are thread-count and input-order invariant;
10. raw/model entry basis is reported per contract;
11. the independent calculator reproduces quantities and residual; and
12. the legacy synthetic ATM book remains bit-identical.

**Implementation:**

1. Price and risk the exact selected contracts through the landed prepared/fused
   portfolio path.
2. Normalize only the names present in the valid selection and admitted corpus.
3. Apply the locked sizing equations without integer rounding.
4. Persist primitive rows plus aggregate audit metadata in `trade_schedule.tsv`.
5. Verify `surface mark in [bid, ask]` for selected contracts where a valid spread is
   available. A configurable fit-policy ceiling may quarantine a leg; it may not
   alter the mark to the midpoint.
6. Implement the independent reference calculator from TSV primitives.

**Gate:** C++ and reference calculator agree on every roll's quantity, leg vega, net
vega, and schedule totals; selected real contracts meet fit/admission policy.

**Commit:** `feat(atx-vol): persist vega-flat listed dispersion schedule`

---

### P4 - Backtest fixed listed contracts and reconcile P&L

**Goal:** run the schedule through the existing high-performance backtest and prove
that marks, hedge P&L, and aggregation are correct.

**Files:** new `ListedDispersionStrategy` in `strategy.hpp` or a focused header,
`dispersion_strategy.cpp`, minimal backtest audit columns, tests, and artifact writer.

**Red tests first:**

1. entry opens exactly the schedule rows and exact expiry timestamps;
2. no non-roll date changes strike, expiry, quantity, or cohort;
3. roll builds replacement before clearing the old cohort;
4. unavailable replacement leaves old book intact;
5. each underlier is delta hedged independently to the configured band;
6. entry and post-roll net vega pass; between-roll drift is reported only;
7. a missing held surface fails the core run;
8. expired lots settle under existing engine rules;
9. P&L components, shares, financing, cost, settlement, and unexplained close to
   total at every step;
10. exact held contracts join to raw quotes without changing model marks;
11. independently summed quote-mid P&L matches the reference calculator;
12. cumulative NAV equals the ordered sum of step P&L; and
13. results are bit-identical across pricer worker counts.

**Implementation:**

1. `ListedDispersionStrategy` consumes the immutable parsed trade schedule. It does
   not call ATM selection or size Greeks during the backtest.
2. On a scheduled roll, materialize all new lots, validate their surface availability,
   then replace the cohort atomically.
3. Return daily `DeltaToZero` through the existing `HedgeSpec`; reuse the per-uid
   engine share ledger.
4. Keep `signals()` empty. Add only typed audit columns needed by the output contract.
5. Build `contract_marks.tsv` by querying the exact held keys in raw OPRA alongside
   surface marks.
6. Calculate quote-mid P&L in a separate reconciliation pass. Do not feed raw mids
   back into `PortfolioPricer` or patch missing model values.
7. Use frictionless/default financing for the locked run while still asserting all
   ledger columns close.

**Gate:** the development slice completes at least one roll with zero unpriced model
lots, in-band daily delta, entry/roll vega tolerance, model and quote P&L series, and
full P&L closure.

**Commit:** `feat(atx-vol): backtest listed SPY dispersion schedule`

---

### P5 - Produce the real core artifact and public-reference review

**Goal:** run the full SPY plus 50-name, 60-date, three-roll backtest and make its
correctness inspectable without reading C++.

**Steps:**

1. Run `build-corpus`, `build-schedule`, `run-backtest`, and `verify` as separate
   commands over the frozen real inventory.
2. Require strict source/market-input provenance and the qualified-corpus gate.
3. Require at least 40 traded names and 80% requested basket-weight coverage at
   every roll.
4. Run the independent reference calculator and fail on any arithmetic mismatch.
5. Generate the complete artifact tree from section 3.
6. In `run_summary.md`, report:
   - exact window, universe source, and SPY/SPX adaptation;
   - requested/admitted/traded boards and weight coverage;
   - fit-quality distribution and every quarantine;
   - selected expiries/strikes and roll dates;
   - entry/roll vega residuals and daily delta bands;
   - model-in-spread rate and model-vs-mid errors;
   - model and quote-mid P&L/NAV reconciliation;
   - serialization and backtest performance; and
   - every known deviation from the public references.
7. Review the artifact against the Cboe/BNP methodology map. Do not add a signal or
   tune a rule based on backtest profitability.

**Hard completion gate:** a real Databento OPRA artifact exists and passes. A cleanly
skipped cached-real test is acceptable for CI portability, but is not sufficient to
mark this sprint complete.

**Commit:** `test(atx-vol): gate real SPY dispersion backtest`

---

### P6 - Assert end-to-end throughput

**Goal:** prove the basic strategy does not give back the performance and serialization
work completed by the preceding sprints.

**Metrics:**

```text
OPRA boards loaded/s
qualified fits/s and total fit wall time
surface archive write MB/s
surface archive open/map surfaces/s
listed contract selection rows/s
schedule pricing contracts/s
backtest steps/s and option contracts/s
end-to-end wall time
peak RSS
```

**Implementation:**

1. Add focused selector/schedule/backtest cases to the landed Google Benchmark
   harness. Do not create another benchmark framework.
2. Run the core workflow in Release and write `performance.json` with phase timings,
   counts, bytes, machine, compiler, route, and thread configuration.
3. Keep all landed American-price, Greek, portfolio, backtest, corpus, and archive
   regression comparisons green.
4. Measure the new orchestration baseline honestly, commit its benchmark JSON, and
   add it to the existing comparator at the established tolerance.
5. Assert that selection/schedule/export overhead is no more than 10% of combined fit,
   serialization, reload, and pricing wall time on the pinned run. If it exceeds the
   budget, profile and fix the orchestration rather than weakening pricing gates.
6. Verify the comparator with a deliberate local slowdown, then remove it.
7. Run thread-count matrices and confirm deterministic non-timing artifacts.

**Gate:** existing baselines pass; new baseline and phase budget pass; the full real
run completes within the recorded resource envelope with no unbounded date/surface
retention.

**Commit:** `perf(atx-vol): gate SPY dispersion workflow throughput`

---

## 7. Acceptance matrix

| Area | Hard acceptance |
|---|---|
| Real data | Databento OPRA, SPY + 50 requested names, >=40 traded and >=80% weight coverage per roll, >=60 dates, >=3 rolls |
| Contracts | every call/put has source identity, actual strike, exact expiry, quote |
| Structure | short SPY ATM straddle, long weighted component ATM straddles |
| Expiry | common listed monthly expiry, deterministic 30-day target selection |
| ATM | nearest valid listed call/put strike to reloaded-surface forward |
| Vega | index vs basket residual `<=1e-10` at entry and roll |
| Delta | per-uid post-daily-hedge delta inside configured band |
| Holding | strikes, expiries, quantities unchanged between rolls |
| Fitting | every traded surface admitted; full failure/quarantine reasons |
| Model marks | every held contract prices; selected-contract spread diagnostics |
| Serialization | process B uses only persisted surfaces; round-trip fidelity passes |
| P&L | model explain closes; independent quote-mid P&L and NAV agree |
| Missing model | fatal via `UnpricedLotPolicy::Error` |
| Determinism | all non-timing artifacts byte-identical across worker counts |
| Performance | landed gates green; new workflow baseline and <=10% overhead budget |
| Public review | methodology map complete; SPY/SPX deviations explicit |
| Legacy | existing ATM/synthetic behavior and tests bit-identical by default |

---

## 8. Expected file changes

### New

- `include/atx/vol/listed_dispersion.hpp`
- `src/listed_dispersion.cpp`
- `include/atx/vol/listed_dispersion_schedule.hpp`
- `src/listed_dispersion_schedule.cpp`
- `examples/spy_dispersion_backtest.cpp`
- `examples/spy_dispersion_run_spec.tsv`
- `tools/reference_spy_dispersion.py`
- `tests/listed_dispersion_test.cpp`
- `tests/spy_dispersion_pipeline_test.cpp`
- cached-real SPY dispersion test following the existing skip convention
- benchmark cases in the landed `atx-vol/bench` layout

### Existing, narrowly changed

- `include/atx/vol/strategy.hpp`
- `src/dispersion_strategy.cpp`
- `include/atx/vol/backtest.hpp`, `src/backtest.cpp` only for required audit columns
- OPRA/corpus helpers only where exact contract identities or fingerprints are not
  currently retained long enough
- `include/atx/vol/vol.hpp`
- CMake/test/benchmark registration
- parent sprint/progress documentation

### Avoid unless P0 proves necessary

- American pricing kernel
- fitted curve implementations
- `PricedSurface` query ABI
- portfolio result layout beyond typed audit output
- surface archive framing
- derivatives/variance-swap module

The plan consumes those completed systems; it does not redesign them.

---

## 9. Non-goals

1. DSPX or VIX-style quoted variance strips.
2. Implied-correlation, dispersion, timing, or allocation signals.
3. Native variance swaps, gamma swaps, or correlation swaps.
4. Gamma-flat, theta-flat, or risk-profile switching.
5. Weekly, quarterly, 0DTE, delta-relative, or constant-maturity option positions.
6. Integer-lot optimization.
7. Intraday hedge optimization or vega rebalancing between rolls.
8. Calibrated spreads, commissions, impact, borrow, or funding.
9. Claiming exact COR3M/DSPX/SPX replication from a SPY proxy book.
10. A UI, dashboard, tearsheet-design project, or signal research notebook.

These are intentionally excluded so the sprint closes the basic fit -> serialize ->
reload -> price -> risk -> backtest loop first.

---

## 10. Stop conditions and honest failures

Stop and return to design review if:

- exact listed expiries or date-scoped identities cannot be recovered from the real
  inputs;
- schedule and surface source fingerprints cannot be joined;
- a selected real contract cannot be reproduced after archive reload;
- per-contract Greeks do not sum to schedule Greeks;
- model P&L does not close through the existing explain identity; or
- the independent calculator disagrees with exported primitive arithmetic.

Do not widen fit bands, substitute synthetic strikes, carry stale quotes, or drop held
P&L to force a run through. An honest failed real artifact with a root cause is useful
engineering evidence; it is not sprint completion.

---

## 11. Verification commands

Exact presets/targets are re-audited after the performance merge. Expected shape:

```powershell
cmake --preset rel
cmake --build build --target atx-vol-tests atxvol_spy_dispersion_backtest -j
$env:ATX_VOL_FIT_WORKERS='1'
ctest --test-dir build -L atx_vol -j16 --output-on-failure --timeout 900
Remove-Item Env:ATX_VOL_FIT_WORKERS
```

Focused tests:

```powershell
build\atx-vol\tests\atx-vol-tests.exe `
  --gtest_filter='ListedDispersion*:SpyDispersion*'
```

Core workflow:

```powershell
build\atx-vol\atxvol_spy_dispersion_backtest.exe build-corpus `
  --spec atx-vol\examples\spy_dispersion_run_spec.tsv --out C:\atx-runs\spy-dispersion

build\atx-vol\atxvol_spy_dispersion_backtest.exe build-schedule `
  --run C:\atx-runs\spy-dispersion

build\atx-vol\atxvol_spy_dispersion_backtest.exe run-backtest `
  --run C:\atx-runs\spy-dispersion

python atx-vol\tools\reference_spy_dispersion.py `
  --run C:\atx-runs\spy-dispersion

build\atx-vol\atxvol_spy_dispersion_backtest.exe verify `
  --run C:\atx-runs\spy-dispersion
```

No executable automatically reads `DATABENTO_API_KEY`. Any missing-data pull uses a
separate operator-approved command after free cost preflight.

---

## 12. Primary public references

1. Cboe, *Implied Correlation*: traditional long dispersion is short an ATM SPX
   straddle and long ATM component straddles; the reference also describes its
   top-50, value-weighted, fitted-analytics construction:
   <https://cdn.cboe.com/resources/indices/documents/Cboe_USO_ImpliedCorrelation_0421_v2.0.2.pdf>
2. Cboe COR3M launch description: ATM index/component straddles, top 50
   value-weighted S&P 500 names, and fitted option prices/Greeks:
   <https://ir.cboe.com/news/news-details/2021/Cboe-Announces-Launch-of-New-Cboe-3-Month-Implied-Correlation-Index-07-01-2021/default.aspx>
3. BNP Paribas public QIS summary: practical use of liquid plain-vanilla ATM options
   or strike strips, hedging, dynamic rebalancing, and flat-risk implementations:
   <https://globalmarkets.cib.bnpparibas/equity-dispersion-trading/>
4. Databento standards: `instrument_id` is only guaranteed unique within a day:
   <https://databento.com/docs/standards-and-conventions>
5. Databento schema documentation: instrument definitions are point-in-time data:
   <https://databento.com/docs/schemas-and-data-formats/cbbo>

---

## 13. Sprint close statement

The sprint closes with a real, reviewable SPY listed-options dispersion backtest, not
with a new API demonstrated only on synthetic data. Its core evidence is:

- real diverse-board fit quality;
- serialized-surface round-trip and process-boundary consumption;
- exact listed ATM option identities;
- vega-flat entry and roll books;
- daily delta hedge and closed P&L;
- model-versus-market and independent-calculator reconciliation; and
- asserted performance evidence.

Only after that artifact is correct do variance strips, correlation signals, costs,
or more sophisticated QIS conventions become the next priority.
