# atx-vol Vega-Flat Quoted-Variance Dispersion Sprint

**Date:** 2026-07-10

**Status:** implementation-ready plan. Implementation starts only after
`sprints/2026-07-09-american-pricing-portfolio-throughput-sprint.md` is merged and
its full correctness and performance gates pass.

**Parent module:**
`sprints/pf2/2026-07-08-atx-vol-qis-dispersion-northstar-workmodule.md`

**Previous sprint:**
`sprints/pf2/2026-07-10-atx-vol-multiname-corpus-qualification-serialization-sprint.md`

**Northstar contribution:** replace the current ATM-straddle dispersion demo with
the first methodology-faithful, backtestable vega-flat variance implementation:

1. compute 30-day constituent and index variance directly from real OPRA quote
   strips under the public Cboe rules;
2. preserve the exact selected contracts and coefficients as a deterministic
   corpus sidecar;
3. construct a fixed-expiry, finite option-strip proxy whose index and basket
   vegas are flat at each entry and roll; and
4. run that book through the existing American surface, portfolio, P&L explain,
   lifecycle, and daily delta-hedge machinery without weakening the legacy path.

This sprint does **not** declare the northstar complete. It establishes the QIS
instrument and signal layer. The following sprint adds calibrated execution costs,
between-roll vega drift control, and the real multi-date pilot/reconciliation.

---

## 0. Executive decision

Build one vertical path with two deliberately separate valuation domains:

```text
Databento OPRA CBBO + point-in-time contract definitions
  -> public-rule quote filtering and expiry selection
  -> raw quoted 30-day variance oracle
  -> deterministic quoted-strip archive + exactness report
  -> point-in-time dispersion signal and survivor weights
  -> finite near/next option-strip proxy
  -> actual served-surface vega normalization
  -> fixed-expiry lots held until the configured roll
  -> daily delta hedge + American surface MTM + P&L explain
```

The raw quote oracle and the served-surface reprice are not interchangeable:

- **Quoted methodology lane:** raw valid OPRA mids determine forwards, `K0`, wing
  truncation, discrete variance, 30-day interpolation, and the DSPX-style signal.
- **Tradable/model lane:** the exact contracts selected by the quoted lane are
  repriced and risked on `PricedSurface`; these marks size and MTM the option proxy.

Every result records which lane produced it. A fitted surface must never be used to
invent missing quotes, extend a zero-bid wing, or make a public-methodology result
look available when the raw board failed the validity rules.

### 0.1 Corrections to the parent S2 outline

The parent workmodule's short S2 sketch is superseded in four places:

1. **No square-root-correlation scaling.** The public variance-dispersion result is
   the variance-notional ratio

   ```text
   alpha_i = N_i / N_index = w_i * sigma_index / sigma_i
   ```

   for a vega-flat book. The old `sqrt(rho)` statement has no supporting public
   source in the reviewed methodology and is not implemented.

2. **Vega-flat is the locked product.** Gamma-flat and theta-flat are useful research
   variants, but bundling them into this sprint increases the behavioral surface
   without advancing the locked deliverable. They remain future comparators.

3. **DSPX is not an implied-correlation formula.** DSPX is a variance-difference
   measure. The existing ATM `implied_corr` remains a labeled comparator. The new
   quoted clean correlation and the DSPX-style measure are separate outputs.

4. **A constant-maturity signal is not a constant-maturity position.** The signal is
   recomputed at 30 days on every observation. An opened trade holds the actual near
   and next listed expiries fixed until roll. The backtest may not silently restrike
   or replace the held strip every day.

---

## 1. Sprint definition of done

The sprint is complete only when all of the following are true:

1. A strict quoted-variance kernel implements the public Cboe expiry, quote,
   forward, `K0`, zero-bid, minimum-option, discrete-variance, and 30-day time
   interpolation rules.
2. Point-in-time contract metadata is joined by `(trade_date, instrument_id,
   raw_symbol)`. Strict mode rejects unknown settlement/eligibility facts rather
   than guessing.
3. Each contract contributes one point-in-time quote selected at or before the
   valuation timestamp under an explicit CBBO sampling policy. Future or ambiguous
   observations are rejected.
4. Each `(date, symbol)` has exactly one stable methodology disposition and reason:
   `Valid`, `InvalidMetadata`, `InvalidQuoteSnapshot`, `NoNearTerm`, `NoNextTerm`,
   `InvalidForward`, `InsufficientOtm`, `InvalidVariance`, `SourceMismatch`, or
   `NotInUniverse`.
5. Raw selected contracts, mids, term coefficients, aggregate variance, source
   fingerprint, methodology fingerprint, and exactness flags round-trip through a
   deterministic, CRC-protected sidecar archive.
6. The variance archive joins one-to-one with the qualified surface corpus by date,
   canonical symbol, valuation timestamp, and source fingerprint. A mismatch is a
   hard error, never a warning.
7. The quoted DSPX-style signal uses current valid names only and renormalizes the
   surviving FMC weights for back-tested EOD mode; it never pulls a stale variance
   forward.
8. The vega-flat book uses the public analytic variance-notional ratios as an oracle
   and the actual finite option portfolio vegas as the executable normalization.
   Both residuals are reported.
9. Opened lots retain their listed near/next expiry timestamps and their strikes and
   quantities until roll. The earliest held expiry drives the roll decision.
10. The QIS preset requests the existing daily delta-to-zero hedge and rebuilds the
   vega-flat strip at each roll. The default legacy ATM strategy remains unchanged.
11. A synthetic multi-name corpus runs through archive load, strip load, signal,
    book construction, backtest, hedge, and P&L closure deterministically.
12. A cached-real OPRA gate runs when its data and definition sidecars are available,
    emits a complete exactness/availability report, and never downloads data.
13. All pre-existing correctness tests and the landed American/portfolio/archive
    performance regression gates remain green.

---

## 2. Methodology contract

### 2.1 What is replicated exactly

For each constituent board, strict `BacktestedEod` mode implements these public
Cboe rules:

- eligible standard monthly, standard quarterly, and Friday weekly expiries;
- near term in `[10, 30]` calendar days and next term in `(30, 120]` days;
- standard expiries preferred, with the closest eligible Friday weekly fallback;
- a valid quote is two-sided, has nonzero ask, and has `ask >= bid`;
- a valid strip contains at least one strike with valid call and put quotes;
- the parity strike minimizes `abs(call_mid - put_mid)`, with the lowest strike
  winning an exact tie;
- `F = K + exp(r*T) * (call_mid - put_mid)` at that parity strike;
- `K0` is the listed strike at or immediately below `F`;
- puts below `K0`, calls above `K0`, and the call/put midpoint at `K0` enter the
  variance sum;
- two consecutive zero bids truncate each wing, including any later nonzero bids;
- invalid and remaining zero-bid options are excluded;
- both near and next terms require at least three valid OTM calls and three valid
  OTM puts;
- term variance is

  ```text
  sigma_j^2 = (2 / T_j) * sum_k[
                  DeltaK_k / K_k^2 * exp(r_j*T_j) * Q(K_k)
              ]
              - (1 / T_j) * (F_j / K0_j - 1)^2
  ```

- near and next variance are interpolated to 30 calendar days with exact expiration
  minutes and `525600` minutes per year; and
- a missing current constituent variance is dropped for a back-tested EOD result,
  with surviving FMC weights renormalized. No prior value is pulled forward.

The raw calculation queries an explicitly point-in-time rate curve at each term's
methodology maturity and records its source/as-of fingerprint. A missing rate is
unavailable in strict mode. A supplied rate proxy may run under a downgraded fidelity
flag, but is never presented as the official VIX rate input.

The implementation exposes the term option coefficients as first-class output.
That makes selection reviewable and lets the executable proxy use the same contracts
as the signal.

### 2.2 Dispersion outputs

Let normalized surviving FMC weights be `w_i`, quoted 30-day constituent variance
be `v_i`, quoted index variance be `v_I`, and `sigma_i = sqrt(v_i)`.

The core signals are:

```text
basket_variance = sum_i w_i * v_i

dspx_opra_proxy = 100 * sqrt(max(basket_variance - v_I, 0))

rho_clean = (v_I - sum_i w_i^2 * v_i)
            / ((sum_i w_i * sigma_i)^2 - sum_i w_i^2 * v_i)
```

`dspx_opra_proxy` is the required self-contained result because both sides come from
our OPRA inputs. It is **not** labeled `DSPX` unless all of these are supplied and
provenance-complete:

- the official effective DSPBX constituent set;
- official point-in-time FMC weights; and
- the official VIX level for the observation.

When those optional inputs are present, the library may additionally emit:

```text
dspx_public_replica = 100 * sqrt(max(basket_variance - (VIX/100)^2, 0))
```

The existing ATM-straddle `implied_corr` remains `atm_implied_corr`. An optional
diagonal-omitting proxy may be exposed under an explicit research name, but it is
not part of the public-methodology acceptance gate and must not be called DSPX.

### 2.3 Vega-flat sizing

For an ideal variance swap with remaining time `tau` and original maturity `T`,
variance notional `N` has volatility vega proportional to:

```text
Vega_var = N * 2 * sigma * tau / T
```

At inception, equal maturity makes a public vega-flat dispersion book satisfy:

```text
N_i / N_I = w_i * sigma_I / sigma_i
```

The finite listed-option proxy is not an ideal continuous variance swap. Therefore
the implementation records two normalizations:

1. `analytic_var_notional_ratio_i = w_i * sigma_I / sigma_i`; and
2. `executed_strip_scale_i`, solved from the sum of the actual signed
   `PricedSurface::greeks(...).vega` values of the selected near/next options.

The index side is scaled to `target_vega`. Each constituent receives its `w_i`
share of that gross vega with the opposite sign. The resulting executable residual
must satisfy:

```text
abs(index_vega + sum_i constituent_vega_i)
  / target_vega <= 1e-10
```

on deterministic synthetic boards. The difference between analytic and executable
scales is a published discretization/model-basis diagnostic, not hidden rounding.

### 2.4 Signal, trade, and mark separation

The following distinctions are binding:

| Object | Source | Maturity behavior | Purpose |
|---|---|---|---|
| Quoted variance signal | Current raw OPRA mids | Recomputed at constant 30 days | Methodology oracle |
| Opened strip proxy | Contracts selected on entry/roll date | Fixed listed near/next expiries | Executable position |
| Strip model mark | `PricedSurface` on the same contracts | Ages with each fixed expiry | P&L and Greeks |
| Continuous log strip | Existing `derivatives.cpp` Simpson integral | Model tenor | Research comparator only |

The raw strip and surface reprice both calculate aggregate quoted/model variance on
the exact same contract selection. Their difference is `variance_model_basis`.
The option-sum PV difference is `strip_model_basis`. Admission policy may cap either;
neither is silently booked as zero.

### 2.5 Fidelity levels

Every snapshot emits one `MethodologyFidelity`:

| Level | Requirements | Permitted label |
|---|---|---|
| `ExactPublicInputs` | Strict metadata, official-compatible rates, official universe/weights, official VIX where required | `DSPX public replica` |
| `ExactQuotedRules` | Strict metadata, point-in-time rate provenance, exact quote rules, supplied proxy universe, self-computed index strip | `DSPX OPRA proxy` |
| `Compatibility` | Date-derived eligibility or configured settlement fallback | `variance-strip research proxy` |
| `Unavailable` | Any required fact or valid variance missing | no level; reason required |

The real northstar run requires at least `ExactQuotedRules`. Compatibility output is
useful for development but cannot satisfy the final reconciliation gate.

---

## 3. Current implementation map and gaps

Line numbers are from local `main` at `44b38b6` and must be re-audited after the
performance merge.

### 3.1 What already exists and must be reused

- `QuoteFrame`/`QuoteRow` preserve bid, ask, size, expiry, strike, side, and source
  timestamp (`include/atx/vol/data.hpp`).
- `OpraPanel` preserves a row-aligned daily `instrument_id` plane and the
  `(instrument_id, raw_symbol)` dictionary (`opra_panel.hpp:127-143`).
- qualified corpus admission, date-streaming archive construction, source and market
  fingerprints, deterministic reports, and explicit quarantine reasons exist in
  `corpus.hpp`/`corpus.cpp`.
- `PricedSurface` prices and risks exact `(K,T,side)` listed contracts.
- `Portfolio`, `Lot`, `PortfolioState`, `PortfolioPricer`, and `BacktestResult`
  already support fixed-expiry option positions, American Greeks, P&L explain,
  daily delta hedging, frictions, financing, and unpriced-lot policy.
- `DispersionStrategy` already has correct no-trade-before-mutation behavior,
  symbol resolution per snapshot, survivor/drop reporting, and roll lifecycle.
- the landed performance sprint supplies the benchmark framework, prepared/fused
  pricing path, worker reuse, and committed regression comparator.

### 3.2 Gaps this sprint closes

1. `QuoteFrame` has only an expiry date. It cannot prove standard/weekly eligibility,
   exact settlement minutes, exercise style, or multiplier.
2. `OpraPanel` source identities are discarded by `corpus_board_from_opra` after a
   fingerprint is copied. A quoted methodology artifact must be extracted before
   that move.
3. `derivatives.cpp` integrates smooth fitted European prices over a configured
   log-strike grid. It does not implement discrete Cboe quote selection, two-zero-bid
   truncation, `K0`, near/next interpolation, or exact quoted contract retention.
4. Its `integration_error_est` is intentionally `NaN`; there is no independent
   refinement estimate. It cannot be the public quote oracle.
5. `DispersionConfig`, `DispersionSignal`, and `DispersionBook` are ATM-straddle
   only. Each leg becomes exactly one call and put at one synthetic target tenor.
6. `DispersionStrategy` converts every position to one synthetic absolute expiry
   `base_ts + target_T`. A two-expiry variance proxy needs each listed expiry.
7. existing signal output contains only ATM `implied_corr` and drop count.
8. existing missing-name handling knows surface availability, not quoted-variance
   validity, metadata validity, source mismatch, or method fidelity.
9. existing `DispersionUniverse` is static. A methodology replica needs an
   effective-dated universe and point-in-time weights, even if the first real pilot
   uses a supplied proxy schedule.

### 3.3 Why a new sidecar is required

The fitted surface archive is insufficient by design. It discards the raw quote
validity state and contract identity needed to reproduce the public selection rules.
Embedding raw strips into `PricedSurface` would contaminate the hot pricing/archive
ABI and conflict with the performance sprint.

This sprint therefore adds a sibling quoted-strip archive. It shares no mutable
state with the surface archive, but is joined by stable fingerprints at run time.

---

## 4. Target architecture and public APIs

Names below are concrete defaults. P0 may adjust them to the landed naming style,
but may not weaken the contracts.

### 4.1 Point-in-time definition and series policy

New `include/atx/vol/opra_definition.hpp`:

```cpp
enum class ExpiryClass : std::uint8_t {
  StandardMonthly,
  StandardQuarterly,
  FridayWeekly,
  Other,
  Unknown,
};

enum class SettlementKind : std::uint8_t { Am, Pm, Unknown };
enum class ExerciseStyle : std::uint8_t { American, European, Unknown };

struct OpraContractDefinition {
  std::string trade_date;
  std::uint32_t instrument_id{0};
  std::string raw_symbol;
  std::string option_root;
  std::int64_t definition_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  ExpiryClass expiry_class{ExpiryClass::Unknown};
  SettlementKind settlement{SettlementKind::Unknown};
  ExerciseStyle exercise_style{ExerciseStyle::Unknown};
  double multiplier{0.0};
  std::uint64_t source_fingerprint{0};
};

class OpraDefinitionTable {
 public:
  static Result<OpraDefinitionTable> create(
      std::vector<OpraContractDefinition> definitions);
  const OpraContractDefinition* find(std::string_view trade_date,
                                     std::uint32_t instrument_id,
                                     std::string_view raw_symbol) const;
};
```

`OpraSeriesRuleTable` supplies facts that are exchange-series conventions rather
than inferable from OSI text. Each rule has a source/as-of tag. Strict mode requires
definition/rule agreement; compatibility mode may derive a Friday expiry class from
the calendar but records that derivation in fidelity flags.

`OpraQuoteSnapshotPolicy` defines the CBBO observation used for the calculation:
target timestamp, last-at-or-before selection, interval semantics, and optional
maximum age. Strict mode rejects future records and multiple conflicting records for
one contract/time bucket. Any maximum-age filter is labeled as an ATX execution-data
quality rule, not a Cboe formula rule.

The key is date-scoped because Databento only guarantees `instrument_id` uniqueness
within one day. A naked numeric ID is never persisted as a cross-date identity.

### 4.2 Quoted variance types

New `include/atx/vol/quoted_variance.hpp`:

```cpp
enum class VarianceMethodologyMode : std::uint8_t {
  BacktestedEodStrict,
  Compatibility,
};

enum class VarianceDisposition : std::uint8_t {
  Valid,
  InvalidMetadata,
  InvalidQuoteSnapshot,
  NoNearTerm,
  NoNextTerm,
  InvalidForward,
  InsufficientOtm,
  InvalidVariance,
  SourceMismatch,
  NotInUniverse,
};

struct VarianceMethodologySpec {
  VarianceMethodologyMode mode{VarianceMethodologyMode::BacktestedEodStrict};
  std::int64_t target_minutes{30 * 24 * 60};
  std::int64_t year_minutes{365 * 24 * 60};
  std::uint32_t min_otm_calls{3};
  std::uint32_t min_otm_puts{3};
  bool allow_pulled_forward{false};
};

struct QuotedVarianceOption {
  std::uint32_t instrument_id{0};
  std::string raw_symbol;
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  Side side{Side::Call};
  double bid{0.0};
  double ask{0.0};
  double mid{0.0};
  double delta_k{0.0};
  double term_variance_coefficient{0.0};
  double constant_maturity_coefficient{0.0};
  bool is_k0{false};
};

struct QuotedVarianceTerm {
  std::int64_t expiry_ts_ns{0};
  std::int64_t minutes_to_expiry{0};
  double methodology_T{0.0};
  double rate{0.0};
  double forward{0.0};
  double k0{0.0};
  double variance{0.0};
  std::uint32_t n_otm_calls{0};
  std::uint32_t n_otm_puts{0};
  std::vector<QuotedVarianceOption> options;
};

struct QuotedVarianceResult {
  std::string date;
  std::string symbol;
  std::int64_t valuation_ts_ns{0};
  VarianceDisposition disposition{VarianceDisposition::InvalidMetadata};
  MethodologyFidelity fidelity{MethodologyFidelity::Unavailable};
  QuotedVarianceTerm near_term;
  QuotedVarianceTerm next_term;
  double near_weight{0.0};
  double next_weight{0.0};
  double variance_30d{0.0};
  std::uint64_t source_fingerprint{0};
  std::uint64_t definition_fingerprint{0};
  std::uint64_t methodology_fingerprint{0};
  VarianceFidelityFlags flags{VarianceFidelityFlags::None};
};
```

The option vector stores two `K0` rows, one call and one put, each with half of the
public `K0` coefficient. Their combined raw contribution is therefore exactly the
call/put midpoint. This preserves both source identities and gives the executable
proxy an unambiguous contract split.

### 4.3 Surface reprice and executable strip

New `include/atx/vol/variance_strip.hpp` separates contract selection from marks:

```cpp
struct StripContract {
  std::uint32_t uid{0};
  std::int64_t expiry_ts_ns{0};
  double K{0.0};
  Side side{Side::Call};
  double unit_quantity{0.0};
  double raw_mid{0.0};
};

struct StripReprice {
  double raw_option_sum{0.0};
  double model_option_sum{0.0};
  double raw_variance{0.0};
  double model_variance_same_contracts{0.0};
  double unit_vega{0.0};
  double unit_delta{0.0};
  double unit_gamma{0.0};
  double unit_theta{0.0};
  double strip_model_basis{0.0};
  double variance_model_basis{0.0};
  std::vector<StripContract> contracts;
};
```

`reprice_quoted_strip` consumes a valid `QuotedVarianceResult`, a `PricedSurface`,
and its uid. It uses the exact selected strikes and listed expiries. It does not
resample a model grid and does not call the existing smooth Simpson strip.

`methodology_T` is exact minutes divided by `525600`, as required by the quoted
calculation. Surface pricing separately resolves model `T` from absolute nanoseconds
using the library's existing `365.25`-day convention. The two values are never reused
for each other and their day-count difference is included in model-basis provenance.

At `K0`, the signal uses the public call/put midpoint. The executable proxy divides
the `K0` coefficient equally between the call and put so its option price contribution
matches that midpoint and avoids an arbitrary directional option choice.

### 4.4 Quoted-strip archive and joined corpus

New `quoted_variance_archive.hpp/.cpp` writes one `<date>.atxqva` file per date and
one `variance_manifest.tsv` for the corpus.

The binary format has:

- fixed magic, version, endian marker, schema hash, date, valuation timestamp,
  methodology fingerprint, and whole-file CRC-32C;
- a sorted symbol directory with disposition, fidelity, source/definition
  fingerprints, offsets, counts, and per-symbol CRC;
- fixed-width near/next term records;
- fixed-width selected-option records using date-scoped instrument IDs and a raw
  symbol dictionary; and
- no absolute path or timing measurement in deterministic fingerprints.

`QuotedVarianceArchive::open_file` mirrors the landed surface archive's buffered and
mapped-open naming and ownership conventions. It does not copy the surface archive's
private implementation or claim zero-copy reconstructed objects.

`QisCorpusIndex::create(surface_manifest, quality_report, variance_manifest,
universe_schedule)` performs the hard join. For every backtest cell it verifies:

```text
date
canonical symbol
valuation timestamp
source fingerprint
surface disposition
variance disposition
universe effective date
```

It emits the explicit intersection and every excluded cell/reason. The strategy
never computes this intersection ad hoc.

### 4.5 Effective-dated universe and weights

New `DispersionUniverseSchedule` is a sorted set of effective-dated snapshots:

```cpp
struct DispersionUniverseSnapshot {
  std::string effective_date;
  DispersionUniverse universe;
  WeightSource source{WeightSource::ProvidedProxy};
  std::string source_name;
  std::string as_of;
  std::uint64_t fingerprint{0};
};
```

Weights are raw positive FMC weights and are normalized only after current variance
validity is known. A static `DispersionUniverse` constructor remains for legacy ATM
tests and delegates to a one-snapshot schedule.

The sprint does not redistribute or commit licensed DSPBX pro-forma data. The real
runner accepts an operator-supplied schedule and labels a static or reconstructed
schedule `ProvidedProxy`.

### 4.6 Vega-flat book and strategy

Add an opt-in instrument without changing the existing default:

```cpp
enum class DispersionInstrument : std::uint8_t {
  AtmStraddleLegacy,
  QuotedVarianceStrip,
};

struct DispersionConfig {
  // existing fields unchanged
  DispersionInstrument instrument{DispersionInstrument::AtmStraddleLegacy};
  VarianceMethodologySpec variance{};
  HedgeSpec hedge{};
};
```

Do not force a raw-variance dependency through the legacy
`build_dispersion_book(universe, surfaces, cfg)` signature. Add a distinct overload:

```cpp
Result<VarianceDispersionBook> build_variance_dispersion_book(
    const DispersionUniverse& universe,
    const SurfaceSet& surfaces,
    const QuotedVarianceSnapshot& quoted,
    const DispersionConfig& cfg);
```

`VarianceDispersionBook` contains:

- quoted signal and fidelity;
- normalized survivor weights;
- public analytic variance-notional ratio per name;
- executable strip scale and model-basis metrics per leg;
- flat `StripPosition` rows with an absolute expiry timestamp;
- net/gross delta, gamma, vega, theta at construction;
- analytic and executable vega residuals; and
- all dropped names with stage-specific reason codes.

The existing `DispersionBook` and ATM functions remain source-compatible and
bit-identical.

`DispersionStrategy` receives the optional `VarianceCorpus`/schedule through a new
constructor used only by `QuotedVarianceStrip`. At entry/roll it:

1. resolves the effective universe and quoted snapshot at `base.ts_ns()`;
2. builds the entire new book before mutating held lots;
3. on valid construction, clears the previous cohort if rolling;
4. opens every selected option with its true expiry timestamp;
5. sets `front_expiry_` to the earliest held expiry;
6. retains all quantities and contracts until the next roll; and
7. exposes the configured `HedgeSpec` through `hedge_spec()`.

The QIS preset is daily `DeltaToZero` with a documented band. The default constructor
continues to return `HedgeSpec::None` and preserves the legacy result.

---

## 5. Task graph

```text
P0 post-performance integration audit + freeze method spec
 |
 P1 point-in-time definitions and exactness policy
 |
 P2 raw quoted 30-day variance kernel
 |\
 | P3 exact-contract surface reprice and risk
 |/
 P4 deterministic quoted-strip archive + qualified-corpus join
 |
 P5 vega-flat variance dispersion signal and executable book
 |
 P6 lifecycle, daily hedge, backtest diagnostics, and P&L closure
 |
 P7 synthetic/cached-real acceptance, benchmark baseline, and documentation
```

P1 and the pure arithmetic portion of P2 can be developed in parallel after P0,
but their merge order is P1 then P2. P5 does not start until both raw and served
lanes are independently tested.

---

## 6. Detailed implementation tasks

### P0 - Rebase on the performance sprint and freeze contracts

**Goal:** remove stale assumptions before code changes and make every public formula
and implementation adaptation reviewable.

**Files:** this sprint document, parent workmodule S2 status note,
`.superpowers/sdd/progress.md`, and a new short
`atx-vol/docs/qis_methodology_contract.md` if implementation documentation belongs
outside the sprint file.

**Steps:**

1. Rebase/merge from the exact performance-sprint completion commit.
2. Re-audit changes in `priced_surface.hpp/.cpp`, `portfolio_pricer.cpp`,
   `backtest.cpp`, archive framing, benchmark targets, and test CMake ownership.
3. Run the full landed gate before feature edits and record commit, compiler, preset,
   CPU, test counts, and benchmark-baseline identity.
4. Add a methodology decision table with each formula classified as `Cboe exact`,
   `public academic`, `bank public description`, or `ATX implementation choice`.
5. Record the explicit adaptations: self-computed index variance instead of official
   VIX, proxy universe when official DSPBX inputs are absent, finite listed strip
   instead of an OTC continuous variance swap, and American model marks for equity
   options.
6. Confirm the performance merge's archive/API changes do not supply a general
   sidecar mechanism. Reuse one if it exists; otherwise retain the sibling archive
   design above.

**Tests/gates:**

- pre-change full `atx_vol` test label green;
- performance baseline comparator green with no regenerated JSON;
- no code implementation begins with unresolved hot-file conflicts; and
- every formula in the public header documentation cites a primary source.

**Commit:** `docs(atx-vol): freeze vega-flat quoted-variance contract`

---

### P1 - Point-in-time OPRA contract definitions

**Goal:** make expiry eligibility and minutes factual, date-scoped, and auditable.

**Files:** new `opra_definition.hpp/.cpp`, `opra_panel.hpp/.cpp`, `opra_batch.hpp/.cpp`,
optional definition-Parquet adapter in `atx-core`, `vol.hpp`, CMake files, and new
`tests/opra_definition_test.cpp`.

**Red tests first:**

1. the same `instrument_id` on two dates maps to different raw symbols without a
   collision;
2. one date/id mapping to two raw symbols is rejected;
3. a definition timestamp after the quote timestamp is rejected as look-ahead;
4. raw symbol, expiry, strike, or side disagreement between quote and definition is
   rejected in strict mode;
5. missing settlement timestamp or unknown expiry class is `InvalidMetadata` in
   strict mode;
6. compatibility-mode Friday inference succeeds but sets a non-exact flag;
7. AM and PM settlement for the same calendar expiry produce different exact minute
   counts; and
8. a future CBBO observation is rejected and last-at-or-before selection is stable;
9. conflicting records for one contract/time bucket are rejected; and
10. serialization/fingerprint output is invariant to definition row order.

**Implementation:**

1. Add structured loaders for the operator-supplied point-in-time definition
   Parquet/TSV schema. Do not parse definition facts out of arbitrary strings when
   a structured column exists.
2. Canonicalize and sort by `(trade_date, instrument_id, raw_symbol)` and reject
   duplicates with different economics.
3. Validate definition `as_of <= quote timestamp` and date scope before lookup.
4. Add `OpraSeriesRuleTable` for exchange-series conventions not represented in the
   definition record. Rules require source/as-of provenance and a fingerprint.
5. Apply `OpraQuoteSnapshotPolicy` before methodology grouping so each daily
   instrument has one non-look-ahead quote and a recorded quote age.
6. Add `prepare_opra_methodology_board(...)` that joins the row-aligned ID plane and
   identity dictionary before `OpraPanel` is moved into `CorpusBoard`.
7. Keep `corpus_board_from_opra` source-compatible. Add a combined helper returning
   both the corpus board and methodology board so callers cannot accidentally consume
   the panel before extracting the quote plane.
8. If a live historical-definition pull helper is added, keep it operator-gated,
   cost-preflighted, and outside tests. This sprint must not issue a network pull.

**Acceptance:** exact minute-to-expiry, expiry class, settlement, exercise style, and
multiplier are present for every strict-board row; incomplete rows yield one stable
reason. Existing OPRA loader tests remain bit-identical in compatibility mode.

**Commit:** `feat(atx-vol): add point-in-time OPRA definition join`

---

### P2 - Raw quoted 30-day variance oracle

**Goal:** implement the public arithmetic as a pure deterministic kernel independent
of fitting and pricing.

**Files:** new `quoted_variance.hpp/.cpp`, `vol.hpp`, CMake, new
`tests/quoted_variance_test.cpp`, and compact hand-calculated fixtures under
`tests/fixtures/qis/`.

**Red tests first:**

1. valid quote truth table: nonzero ask, `ask >= bid`, two-sided fields present;
2. standard-expiry preference and closest-Friday-weekly fallback;
3. exact `[10,30]` and `(30,120]` boundary behavior;
4. forward parity minimization and lowest-strike tie break;
5. `K0` selection immediately below forward;
6. separate call and put two-consecutive-zero-bid truncation, including a later
   nonzero bid that must remain excluded;
7. invalid quotes and remaining zero bids removed after truncation;
8. `K0` price equals the mean of call and put mids;
9. endpoint/interior `DeltaK` rules;
10. minimum three OTM calls and puts on both terms;
11. discrete term variance against an independently hand-calculated fixture;
12. exact-minute 30-day interpolation against an independent fixture;
13. nonpositive/nonfinite final variance rejected;
14. input row permutation and duplicate-row policy determinism; and
15. no previous-day value accepted in `BacktestedEodStrict` mode.

**Implementation:**

1. Group rows structurally by exact expiry, strike, and side. Detect duplicate quote
   observations for one snapshot rather than accepting last-row-wins behavior.
2. Select expiries using the joined metadata and stable tie-break rules.
3. Build paired call/put strike maps for the forward; use full-precision mids.
4. Filter each OTM wing in strike-distance order, preserving why each contract was
   excluded for optional diagnostics.
5. Calculate `DeltaK`, option coefficients, forward correction, and term variance in
   a fixed deterministic reduction order.
6. Interpolate total variance to 30 days using integer expiration minutes until the
   final floating operations.
7. Return a populated invalid result with reason/evidence for expected methodology
   failures; reserve `Err` for malformed specs, contradictory input, overflow, or
   internal invariants.
8. Fingerprint the resolved spec and all selected source identities/quotes.

**Acceptance tolerances:** hand-calculated term and 30-day variance fixtures match to
`1e-12` absolute; row permutation produces equal result objects and fingerprints;
all branch fixtures have one stable disposition/reason.

**Commit:** `feat(atx-vol): implement strict quoted variance oracle`

---

### P3 - Exact-contract surface reprice and Greek plane

**Goal:** risk the public-rule selected contracts on the served American surface
without confusing model output with raw methodology output.

**Files:** new `variance_strip.hpp/.cpp`, possibly a thin non-owning helper in
`priced_surface.hpp/.cpp` after P0, `derivatives_test.cpp`, and new
`tests/variance_strip_test.cpp`.

**Red tests first:**

1. every selected non-`K0` option maps one-to-one to one served contract;
2. `K0` coefficient splits equally between call and put and reproduces the public
   midpoint price contribution;
3. true listed expiry timestamps produce the exact residual `T` expected by
   `PortfolioPricer`;
4. aggregate price and all Greeks equal an explicit serial sum of per-contract
   `PricedSurface` results;
5. a missing or failed contract marks the whole strip unavailable; no partial vega;
6. exact-contract model variance uses the raw selection and coefficients, including
   the same forward correction policy;
7. raw mids generated from a no-dividend synthetic served surface have negligible
   model basis; and
8. denser strike ladders converge toward the existing continuous Simpson
   `var_swap_fair_strike` comparator.

**Implementation:**

1. Materialize stable `StripContract` rows in term/strike/side order.
2. Resolve residual `T` from absolute expiry and valuation timestamps using the
   backtest's year convention. Do not reuse 30-day target `T` for every contract.
3. Call the landed prepared/fused full-Greeks API when available. Do not add another
   thread pool or contract cache.
4. Reduce unit PV and Greeks serially in stable order after any parallel pricing,
   matching portfolio determinism.
5. Calculate raw/model option sums and variance basis separately.
6. Keep the existing continuous derivative template unchanged unless a thin
   `PricedSurface` adapter is still useful as a research comparator. It is never the
   strict oracle.

**Acceptance:** aggregate Greeks match explicit per-contract sums to the existing
portfolio tolerance; output is bit-identical across worker counts; the legacy
derivatives tests are unchanged.

**Commit:** `feat(atx-vol): reprice quoted variance strips on served surfaces`

---

### P4 - Quoted-strip corpus archive and hard join

**Goal:** preserve the exact raw methodology inputs at corpus scale and prevent a
surface/strip mismatch from reaching a strategy.

**Files:** new `quoted_variance_archive.hpp/.cpp`, new
`qis_corpus.hpp/.cpp`, `opra_batch.hpp/.cpp`, `vol.hpp`, CMake, new
`tests/quoted_variance_archive_test.cpp` and `tests/qis_corpus_test.cpp`.

**Red tests first:**

1. `parse(serialize(x)) == x` for valid and invalid methodology rows;
2. deterministic bytes across input order and worker counts;
3. corruption in header, directory, term, option, or raw-symbol dictionary is caught
   by the appropriate CRC before object construction;
4. unknown version/schema/enum rejected;
5. duplicate canonical symbol rejected;
6. buffered and mapped opens return equal records;
7. source fingerprint, date, symbol, or timestamp disagreement with the surface
   corpus makes `QisCorpusIndex::create` fail;
8. an admitted surface with invalid variance remains visible in the joined exclusion
   report;
9. a valid variance for a quarantined surface cannot enter the backtest; and
10. date checkpoints resume only when both surface and variance fingerprints match.

**Implementation:**

1. Finalize the binary format with checked multiplication/addition before reading any
   offset/count pair.
2. Plan directory and payload offsets before parallel materialization. Write disjoint
   buffers and reduce status in symbol order.
3. Reuse the landed archive CRC and mapped-file primitives where they are public.
   Do not copy private surface-archive code into a subtly different fork.
4. Add `QuotedVarianceCorpusSession` with strictly ascending dates, pending-file
   commit, date checkpoints, a final deterministic manifest, and bounded live date
   memory.
5. Build surface and variance inputs from the same prepared OPRA cell. Record the
   original panel fingerprint in both outputs.
6. Add `QisCorpusIndex` and a deterministic join/exclusion TSV. This is the only
   object the new QIS example accepts.
7. Keep raw OPRA/definition data outside git. Archive test fixtures are synthetic and
   compact.

**Acceptance:** a many-symbol synthetic date round-trips exactly, archive bytes are
thread-count invariant, all corruptions fail cleanly, and every planned corpus cell
appears in the joined report once.

**Commit:** `feat(atx-vol): persist and join quoted variance corpus`

---

### P5 - Vega-flat signal and executable dispersion book

**Goal:** construct the locked product with explicit public and executable sizing
diagnostics while leaving ATM behavior untouched.

**Files:** `dispersion.hpp/.cpp`, new or extended
`tests/variance_dispersion_test.cpp`, existing `dispersion_test.cpp`.

**Red tests first:**

1. three-name hand calculation for `basket_variance`, `dspx_opra_proxy`, and
   `rho_clean`;
2. negative dispersion variance clamps the DSPX-style level to zero but leaves raw
   signed spread diagnostic available;
3. unavailable name drops in backtested EOD mode and surviving weights renormalize;
4. index variance is never droppable;
5. below `min_names` produces no trade with complete reasons;
6. analytic ratios equal `w_i * sigma_I / sigma_i`;
7. ideal dense synthetic strips make executable scales converge to analytic ratios;
8. actual finite-strip net vega residual meets the stated `1e-10` relative bound;
9. positions have exact near/next expiries and deterministic IDs/order;
10. side reversal changes every quantity sign and no magnitude;
11. quote/model basis threshold rejection is explicit; and
12. every existing ATM result and serialized/golden output remains bit-identical
    under default config.

**Implementation:**

1. Resolve the one survivor set once from surface admission, variance validity, and
   point-in-time universe membership. Normalize weights once and pass that immutable
   result to signal and book construction.
2. Calculate quoted variance signals only from raw results.
3. Reprice each survivor's index/name strips and reject any partial Greek plane.
4. Set index unit scale from `target_vega`. Allocate each name its normalized `w_i`
   gross-vega share with the opposite sign.
5. Emit analytic ratios before finite-strip normalization and executable scales after
   it. Record per-name and aggregate residuals.
6. Expand each scaled strip into flat option rows. Split `K0` evenly call/put.
7. Use the source definition multiplier per contract. Reject mixed or unknown
   multipliers in strict mode; never silently replace them with 100.
8. Aggregate construction Greeks and model basis using the same position rows that
   the strategy will open.
9. Keep `AtmStraddleLegacy` as the zero-value/default enum and preserve old overloads.

**Acceptance:** public closed forms, survivor handling, analytic ratios, and actual
portfolio vega all pass; the legacy ATM test binary output is unchanged.

**Commit:** `feat(atx-vol): build vega-flat quoted variance dispersion book`

---

### P6 - Fixed-expiry lifecycle, hedge, diagnostics, and P&L closure

**Goal:** run the executable book through the existing backtest without daily
restriking or silent methodology degradation.

**Files:** `strategy.hpp`, `dispersion_strategy.cpp`, minimal changes to
`backtest.hpp/.cpp` only if a required diagnostic cannot use `signals`,
`strategy_test.cpp`, `backtest_test.cpp`, and `variance_dispersion_test.cpp`.

**Red tests first:**

1. entry opens both near and next expiry lots with absolute source expiries;
2. a non-roll observation changes no held strike, expiry, or quantity;
3. earliest held expiry drives `RollAtHorizon`;
4. roll builds the complete replacement before clearing the old cohort;
5. an unavailable roll date leaves the prior cohort intact under the configured
   no-trade policy;
6. a source fingerprint mismatch is fatal even under drop/renormalize;
7. QIS preset requests daily delta-to-zero and legacy preset requests no hedge;
8. post-hedge delta is within the configured band on every synthetic step;
9. inception and post-roll vega are within tolerance;
10. between-roll vega drift is measured but not silently rebalanced;
11. P&L axes plus hedge, financing, cost, settlement, and unexplained close to total;
12. `UnpricedLotPolicy::Error` aborts any held strip with an unavailable surface; and
13. signal series stay full length with `NaN` plus reason/count diagnostics on an
    unavailable date.

**Implementation:**

1. Add a variance-aware constructor while preserving the existing constructor and
   behavior.
2. Resolve the effective universe and strip snapshot by valuation timestamp, not by
   vector position or raw date string alone.
3. Convert `StripPosition` directly to `Lot` with its true expiry and source
   multiplier. Do not route through the synthetic-tenor `Position` adapter.
4. Cache/load the date's variance archive once per snapshot; `on_step`, `signals`,
   `build_book`, and `dropped_on` share it.
5. Override `hedge_spec()` to return the config. Reuse the engine-owned shares ledger;
   do not implement a strategy-local delta hedge.
6. Emit stable signal names:

   ```text
   quoted_index_variance_30d
   quoted_basket_variance_30d
   dspx_opra_proxy
   quoted_rho_clean
   n_names_used
   n_names_dropped
   method_fidelity
   model_basis_variance
   model_basis_option_pv
   net_vega_at_entry
   net_vega_current
   vega_drift
   ```

7. If string reason series do not fit `BacktestResult`, emit numeric stable enum codes
   and write their vocabulary in the report metadata. Do not encode reasons as magic
   NaN payloads.
8. Record raw-to-model entry basis as a diagnostic. This sprint does not charge it as
   execution cost; the next sprint calibrates and books costs.

**Acceptance:** the strategy runs at least three dates with one roll, daily hedge,
one temporarily invalid name, fixed held contracts between rolls, and closed P&L.
Output is bit-identical across pricer thread counts.

**Commit:** `feat(atx-vol): backtest fixed-expiry vega-flat variance strips`

---

### P7 - End-to-end gates, performance baseline, and operator artifact

**Goal:** prove the complete sprint path and leave an honest bridge to the real pilot.

**Files:** new/extended `examples/dispersion_backtest.cpp` or a dedicated
`examples/vega_flat_dispersion_backtest.cpp`, `multiname_pipeline_test.cpp`, new
`tests/qis_variance_pipeline_test.cpp`, new cached-real test, benchmark source under
the landed `atx-vol/bench`, baseline JSON, benchmark README, and QIS docs.

### P7.1 Synthetic acceptance corpus

Build at least three dates containing:

- one index and at least ten constituents;
- dense, sparse, low-price, one-sided-wing, hard-borrow, and event-month profiles;
- valid standard and weekly expiry selection;
- AM and PM settlement fixtures;
- one invalid metadata cell, one insufficient-wing cell, and one missing board;
- one universe weight change; and
- enough dates to enter, hold without restriking, roll, and re-enter.

Hard assertions:

- every cell has surface and variance dispositions;
- admitted/valid intersection is deterministic;
- raw closed forms and archive round-trips pass;
- inception and post-roll net vega pass;
- daily post-hedge delta passes;
- held contracts do not change between rolls;
- P&L closes at every step;
- all outputs are identical across fit/archive/pricer worker counts; and
- the default ATM pipeline remains bit-identical.

### P7.2 Cached-real OPRA gate

Add an environment-gated test/example using existing local data only:

```text
ATX_OPRA_QIS_ROOT
ATX_OPRA_DEFINITION_ROOT
ATX_QIS_UNIVERSE_SCHEDULE
```

It must:

1. refuse compatibility fidelity when strict mode is requested;
2. report per-date/symbol selected expiries, valid option counts, variance,
   disposition, and fidelity;
3. report the surviving weight fraction and every dropped reason;
4. build and run the backtest when the joined intersection satisfies `min_names`;
5. write a deterministic TSV/JSON reconciliation artifact outside git; and
6. skip cleanly with one precise message when inputs are absent.

This gate performs no Databento API call and has no API-key code path.

### P7.3 Benchmark and regression contract

Extend the landed benchmark harness with:

- `BM_QuotedVarianceTerm` over varied quote counts and sparsity;
- `BM_QuotedVarianceSnapshot` over index plus 10/50/100 names;
- `BM_QuotedVarianceArchiveWrite/Open/Map` at corpus date scale;
- `BM_RepriceQuotedStrip` prices-only and full-Greeks; and
- `BM_BuildVegaFlatBook` with 10/50/100 names.

First record an honest final Release baseline on the pinned host. Do not invent a
throughput claim before measurement. Commit the JSON and comparator configuration so
subsequent changes fail at the landed framework's standard tolerance. Verify the
comparator with a deliberately slowed local binary, then remove the slowdown.

Existing American, portfolio, backtest, corpus, and surface-archive baselines must not
regress. If the new strip archive misses the measured service budget, optimize it in
this sprint; do not weaken the existing gate or mislabel a buffered copy as zero-copy.

### P7.4 Closeout

- update the parent workmodule: S2 is replaced by this vega-flat implementation;
- record commits and review status in the progress ledger;
- publish methodology exactness and known deviations alongside the example;
- list the cached-real result as pass, fail, or unavailable with evidence; and
- run a final spec review, code-quality review, and broad regression review.

**Commit:** `test(atx-vol): gate vega-flat variance dispersion pipeline`

---

## 7. Acceptance matrix

| Contract | Hard gate |
|---|---|
| Quote validity | Full truth-table tests; no fitted quote substitution |
| Expiry selection | Boundary, preference, fallback, exact-minute tests |
| Forward and `K0` | Tie-break and hand-calculated fixtures |
| Wing truncation | Two-zero-bid adversarial tests on both sides |
| Term/30d variance | Independent fixture, `1e-12` absolute |
| Backtested EOD missing data | Drop current invalid name, renormalize, no pull-forward |
| Source identity | Date-scoped ID/raw-symbol join; no look-ahead |
| Archive | Deterministic bytes, CRC corruption matrix, schema rejection |
| Corpus join | Date/symbol/timestamp/fingerprint exact match |
| Public sizing | `alpha_i = w_i * sigma_I / sigma_i` |
| Executable sizing | relative net vega residual `<= 1e-10` synthetic |
| Fixed positions | strikes/expiries/qty unchanged between rolls |
| Hedge | post-daily-hedge delta inside configured band |
| P&L | every component plus unexplained closes to total |
| Missing held mark | production QIS run uses `UnpricedLotPolicy::Error` |
| Legacy | ATM default bit-identical and source-compatible |
| Determinism | equal results/archives across all worker-count matrices |
| Performance | all landed baselines green; new measured baseline committed |
| Real-data honesty | strict cached-real report or explicit unavailable, never synthetic pass |

---

## 8. File ownership and expected change map

### New files

- `include/atx/vol/opra_definition.hpp`
- `src/opra_definition.cpp`
- `include/atx/vol/quoted_variance.hpp`
- `src/quoted_variance.cpp`
- `include/atx/vol/variance_strip.hpp`
- `src/variance_strip.cpp`
- `include/atx/vol/quoted_variance_archive.hpp`
- `src/quoted_variance_archive.cpp`
- `include/atx/vol/qis_corpus.hpp`
- `src/qis_corpus.cpp`
- `tests/opra_definition_test.cpp`
- `tests/quoted_variance_test.cpp`
- `tests/variance_strip_test.cpp`
- `tests/quoted_variance_archive_test.cpp`
- `tests/qis_corpus_test.cpp`
- `tests/variance_dispersion_test.cpp`
- `tests/qis_variance_pipeline_test.cpp`
- cached-real QIS test following the existing skip pattern
- benchmark sources/fixtures under the landed benchmark layout

### Existing files expected to change

- `include/atx/vol/opra_panel.hpp`, `src/opra_panel.cpp`
- `include/atx/vol/opra_batch.hpp`, `src/opra_batch.cpp`
- `include/atx/vol/dispersion.hpp`, `src/dispersion.cpp`
- `include/atx/vol/strategy.hpp`, `src/dispersion_strategy.cpp`
- `include/atx/vol/vol.hpp`
- `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt`, landed benchmark CMake
- example and methodology documentation

### Hot files changed only if P0 proves necessary

- `priced_surface.hpp/.cpp`
- `portfolio_pricer.hpp/.cpp`
- `backtest.hpp/.cpp`
- `surface_archive.hpp/.cpp`
- `derivatives.hpp/.cpp`

The default design uses adapters/new sibling modules so those performance-owned paths
need little or no change.

---

## 9. Integration and conflict policy

The performance sprint currently owns `PricedSurface`, `PortfolioPricer`, benchmark
CMake, benchmark baselines, and test registration. Implementation must begin from its
completion commit and consume its final APIs.

Conflict rules:

1. Do not restore pre-performance query loops, workers, output frames, or archive
   assumptions from this document's line anchors.
2. Add new benchmark cases to the landed harness; do not create a second harness.
3. If performance added a general prepared-contract API, use it for strip repricing.
4. If performance changed `PriceOptions` or route diagnostics, flow them through the
   new path and assert route provenance.
5. Do not extend the surface archive framing for raw quotes. Keep the methodology
   artifact independent unless the landed code has an explicit typed-sidecar facility.
6. Any default behavior change requires a separate review. This sprint's new behavior
   is opt-in through `QuotedVarianceStrip` or a named QIS preset.

---

## 10. Non-goals

1. Gamma-flat, theta-flat, or tactical switching among flatness modes.
2. Square-root-correlation vega scaling without a primary public source.
3. Claiming exact DSPX with proxy constituents, proxy weights, self-computed SPX
   variance, or missing official VIX.
4. Pulled-forward live/intraday constituent variance. This is a back-tested EOD path.
5. Silent surface extrapolation for absent raw wing quotes.
6. A native OTC variance-swap lot, realized-variance settlement ledger, corridor
   variance, gamma swaps, or correlation swaps.
7. Intraday hedge optimization. The sprint uses the existing daily hedge cadence.
8. Calibrated bid/ask, market impact, borrow, funding, and basket-crossing costs.
9. Between-roll vega reflattening. Drift is measured; the next sprint sets a band and
   trades it.
10. Paid Databento acquisition or redistribution of licensed DSPBX/VIX data.
11. Corporate-action continuity beyond strict date-scoped identity and an effective
    universe schedule.

---

## 11. Known risks and stop conditions

| Risk | Required response |
|---|---|
| Definition data lacks settlement/expiry class | strict result unavailable; add sourced series rule, never guess |
| Official DSPBX schedule unavailable | run `ProvidedProxy`; prohibit exact DSPX label |
| Index strip differs materially from official VIX | publish reconciliation and root cause; do not tune quote rules to force a match |
| American surface model basis is large | quarantine/cap under policy and expose per-name basis |
| Finite strip vega differs from analytic variance vega | use executable normalization, publish both ratios |
| Near expiry rolls before next expiry | close whole cohort and rebuild both terms; report turnover |
| Name invalid on a roll date | preserve old book under no-trade policy or fail under strict execution policy; never clear first |
| Held surface disappears | QIS production config fails via `UnpricedLotPolicy::Error` |
| Sidecar harms corpus throughput | profile and optimize independent archive; existing surface gate cannot be weakened |
| Public bank convention remains undisclosed | label ATX choice and keep it configurable; do not invent desk-specific facts |

Stop implementation and return to design review if:

- exact expiration minutes cannot be sourced for the intended index option class;
- the performance merge removes stable absolute-expiry option pricing;
- quote/source fingerprints cannot join the surface and strip artifacts; or
- an executable option strip cannot reproduce its explicitly summed Greeks.

These are methodology/correctness blockers, not tolerances to widen.

---

## 12. Verification commands

Use the landed presets and target names after P0. Expected shape:

```powershell
cmake --preset rel
cmake --build build --target atx-vol-tests -j
$env:ATX_VOL_FIT_WORKERS='1'
ctest --test-dir build -L atx_vol -j16 --output-on-failure --timeout 900
Remove-Item Env:ATX_VOL_FIT_WORKERS
```

Focused iteration:

```powershell
build\atx-vol\tests\atx-vol-tests.exe `
  --gtest_filter='OpraDefinition*:QuotedVariance*:VarianceStrip*:VarianceDispersion*:QisCorpus*'
```

Run the landed benchmark command and comparator exactly as documented in
`atx-vol/bench/README.md`. The sprint may add cases and a baseline, but may not alter
the established machine/build fingerprint or tolerance merely to obtain a pass.

Cached-real execution is explicit and offline:

```powershell
$env:ATX_OPRA_QIS_ROOT='C:\path\to\cached\opra'
$env:ATX_OPRA_DEFINITION_ROOT='C:\path\to\cached\definitions'
$env:ATX_QIS_UNIVERSE_SCHEDULE='C:\path\to\universe.tsv'
build\atx-vol\tests\atx-vol-tests.exe --gtest_filter='*Qis*Real*'
```

No test or example reads `DATABENTO_API_KEY`.

---

## 13. Primary public references

1. Cboe/S&P DSPX methodology, including OPRA source, eligible strips, forward,
   `K0`, two-zero-bid truncation, valid variance, exact-minute interpolation,
   back-tested EOD survivor handling, and DSPX formula:
   <https://cdn.cboe.com/resources/indices/documents/methodology-the-dispersion-index.pdf>
2. Cboe VIX mathematics methodology for the discrete variance foundation:
   <https://cdn.cboe.com/resources/indices/Cboe_Volatility_Index_Mathematics_Methodology.pdf>
3. BNP Paribas public QIS summary describing ATM/strike-strip implementation,
   vega-flat/gamma-flat/theta-flat risk profiles, dynamic rebalancing, hedging, and
   variance-swap proxies:
   <https://globalmarkets.cib.bnpparibas/equity-dispersion-trading/>
4. Jacquier and Slaoui, *Variance Dispersion and Correlation Swaps*, for variance
   swap Greeks, P&L decomposition, and the vega-flat relation
   `alpha_i = w_i * sigma_I / sigma_i`:
   <https://eprints.bbk.ac.uk/id/eprint/26900/1/26900.pdf>
5. Databento standards: `instrument_id` is only guaranteed unique within a day:
   <https://databento.com/docs/standards-and-conventions>
6. Databento schemas/definitions: instrument definitions are point-in-time reference
   data:
   <https://databento.com/docs/schemas-and-data-formats/cbbo>

---

## 14. Next sprint after this one

The immediate successor is **real OPRA execution and reconciliation**:

- operator-approved, cost-capped definition/CBBO acquisition only for missing cache;
- at least one index plus a liquidity-screened multi-name universe across a real
  multi-roll window;
- calibrated option spread, basket crossing, funding, dividends, and borrow;
- between-roll vega-drift band and reflatten trades;
- external VIX/DSPX comparison where licensed inputs are available;
- correlation-swap/P&L identity cross-check; and
- the final tearsheet, fit-quality, exactness, survivor, model-basis, Greek-residual,
  attribution, cost, and performance reconciliation artifact.

That sprint consumes this one unchanged. It must not rewrite the quote methodology or
vega-flat sizing to make the real result look cleaner.
