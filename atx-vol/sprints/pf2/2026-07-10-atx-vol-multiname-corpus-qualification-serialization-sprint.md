# atx-vol Multi-Name Corpus Qualification and Serialization Sprint

**Date:** 2026-07-10

**Status:** implementation-ready plan. This plan assumes
`sprints/2026-07-09-american-pricing-portfolio-throughput-sprint.md` has landed and
passed its definition of done before implementation starts.

**Parent module:**
`sprints/pf2/2026-07-08-atx-vol-qis-dispersion-northstar-workmodule.md`

**Northstar contribution:** close the unfinished multi-name admission gate and
prove both library pillars on the artifact the dispersion backtest will actually
consume:

1. diverse single-name boards are fitted, independently scored, admitted or
   quarantined with an explicit reason, and never silently disappear; and
2. the admitted surfaces are written and loaded at corpus scale under a committed,
   asserted performance-regression gate.

This is the next sprint after the American pricing/portfolio performance sprint.
The following sprint is the traditional SPY listed-options vega-flat backtest in
`2026-07-10-atx-vol-spy-listed-options-vega-flat-backtest-sprint.md`. It first proves
the basic fit -> serialize -> reload -> listed-contract price/risk -> backtest loop on
qualified surfaces. Variance strips and additional flatness modes are deliberately
deferred.

---

## 0. Executive decision

The existing pf2 ladder called the next methodology unit "S2," but pf2 S1 is not
closed in the current tree. S1-1 through S1-3c landed: symbol-derived uids,
symbol-to-uid resolution, drop-and-renormalize, and explicit unpriced-lot/unpriced-
Greek accounting. The remaining S1-4 requirement is still absent:

- `CorpusConfig` has fit/template/thread/archive options only
  (`include/atx/vol/corpus.hpp:116-129`);
- `CorpusEntry` records only `chosen_kind`, `n_slices`, and one `oos_in_band`
  scalar (`corpus.hpp:87-97`);
- a direct unified-policy route does not run `CurveSelector`, so `fit_board`
  leaves `oos_in_band` at `0.0` (`src/corpus.cpp:49-57,111-124`); and
- `CorpusFitStatus` has only `Ok`, `Failed`, and `Skipped`, with no admission or
  quarantine vocabulary (`corpus.hpp:73-77`).

The 2026-07-09 unified-fit work improved breadth materially, but it did not make
the corpus self-qualifying. The cached real breadth test fits boards one by one;
it does not build one admitted corpus or assert archive throughput
(`tests/opra_breadth_corpus_test.cpp:19`). The synthetic multi-name test covers
four symbols per date, not the requested index plus at least ten diverse names
(`tests/multiname_pipeline_test.cpp:178` and following).

The archive pillar is also unfinished. The writer materializes each blob and CRC
in one sequential loop (`src/surface_archive.cpp:477-566`), `open_file` copies the
whole file into a `vector<byte>` (`surface_archive.cpp:750-770`), and reconstruction
allocates a curve object for every slice (`surface_archive.cpp:906-948`). The old
hand-timed `surface_archive_bench` has no repetitions, no write measurement, and no
regression assertion. A stale 2026-07-05 Release binary reports about 0.39M random
surface maps/s and 622 MB/s bulk reconstruction on the local i7-1260P; that is a
diagnostic, not a baseline.

Therefore this sprint is one vertical unit:

```
planned OPRA cells
  -> point-in-time source validation
  -> per-cell market inputs and fit context
  -> unified fit/fallback
  -> independent quality score
  -> Admit | Quarantine | SourceFailed | FitFailed | Empty
  -> one deterministic archive per date
  -> quality report + legacy-compatible corpus manifest
  -> reload and multi-name dispersion smoke
  -> committed benchmark JSON + asserted comparison gate
```

The sprint closes only when a poor board cannot enter the backtest silently and
the exact post-performance archive payload has a durable performance contract.

---

## 1. Assumptions inherited from the performance sprint

Implementation starts from the merge commit that completes the 2026-07-09
performance plan, not from the line numbers in this document. Task P0 re-audits
all touched APIs. The expected landed capabilities are:

- the Google Benchmark harness, JSON output, CV/p95 statistics, checked-in
  machine baselines, and a baseline comparison script;
- fused/prepared `PricedSurface` and portfolio paths;
- persistent workers/workspaces and stateful backtest advancement;
- the selected reference/production pricing modes and route diagnostics; and
- any optional correction-cache payload attached to `PricedSurface` and the
  archive by performance P4.

This plan extends those mechanisms. It must not create a second benchmark
framework, executor, route enum, cache fingerprint, or archive-cache format.

If performance P4 changes archive framing, P0 updates the archive references and
fixture sizes before feature work. The correctness and benchmark contracts below
remain binding regardless of the final archive version.

---

## 2. Current implementation map and verified gaps

Line references are the 2026-07-10 pre-performance snapshot and are anchors for
P0, not permission to ignore the landed diff.

### 2.1 Corpus ownership and lifecycle

`build_corpus` currently fits every board first into a `slots` vector, then walks
dates and writes archives (`src/corpus.cpp:145-304`). This has three consequences:

1. peak live fitted-surface memory scales with all symbols x all dates, even
   though archives are partitioned by date;
2. source failures are not first-class cells: callers simply omit a missing
   `CorpusBoard`, so the corpus alone cannot distinguish "not planned" from
   "planned but unavailable"; and
3. archive writing begins only after every fit completes, preventing a bounded
   date-at-a-time fit/write/release pipeline.

`build_corpus` does provide valuable contracts we retain: stable `(date, symbol,
input-index)` ordering, disjoint worker slots, one archive per date, symbol-derived
uid stamping, atomic archive replacement, and a deterministic manifest
(`corpus.cpp:177-304`).

### 2.2 Fit policy versus fit quality

The unified policy already has the right routing facts: session phase, event
phase, event distance, forward dispersion, effective yield, HTB, and vol-product
flags (`include/atx/vol/fit_policy.hpp:47-56`). `CorpusBoard` does not carry a
`FitContext`, so corpus fitting cannot supply most of those facts. It only has a
per-board curve override (`corpus.hpp:57-68`).

`VolaSession` already calculates useful in-sample diagnostics:

- worst/mean price-in-band;
- mean reduced chi-square;
- mean vol RMSE;
- calendar-arbitrage status and pre-repair count; and
- fitted slice/quote counts
  (`include/atx/vol/session.hpp:150-159`).

Those diagnostics are not copied into the corpus. They also become zeros when
`score_parity=false`, a documented HFT behavior (`session.hpp:93-99`,
`surface_parity.hpp:123-127`). Zero therefore means either "measured zero" or
"not measured" today. The quality schema must use presence bits/optionals; an
unmeasured value is never serialized as a clean zero.

`CurveSelector` owns genuine held-out scores (`CandidateScore::oos_in_band`,
`oos_vw`, `n_holdout`, `n_slices` in
`include/atx/vol/curve_selector.hpp:33-40`). Direct high-confidence routes bypass
it by design. Admission must score the final chosen curve independently when its
policy requires OOS evidence; it must not run all candidate families again.

### 2.3 Market-input truth

The batch loader accepts one rate curve for every file in the whole request
(`include/atx/vol/opra_batch.hpp:48-70`). `OpraLoadSpec` can carry cash dividends,
but `load_opra_daterange` does not populate them (`src/opra_batch.cpp:186-192`).
The corpus cannot currently express per-date rates, per-symbol dividends, or
per-cell event context in one deterministic run specification.

There is a deeper rate bug than the old work module states. `OptionChain` reduces
the environment to the front-expiry representative rate
(`src/chain.cpp:36`). `PricedSurface` then uses the one
`PricingContext::r` for every expiry (`src/priced_surface.cpp:123,141,157,160,175`).
The curve objects already archive a per-slice discount factor in
`ArchiveSliceHeader::df`; the missing work is to fit and query with that term rate.
We can fix the term structure without changing flat-rate output or necessarily
changing the archive layout.

### 2.4 OPRA identity and provenance

The Databento pull requests instrument-id output and uses the metadata symbol map,
but the Parquet writer drops `instrument_id`; it persists only timestamp,
underlying, raw symbol, quote prices, and sizes
(`atx-core/src/external/databento_pull.cpp:225-259`, public schema comment at
`atx-core/include/atx/external/databento.hpp:93-97`).

Databento's official contract matters here:

- every schema contains an `instrument_id`, but it is guaranteed unique only
  within one day;
- raw symbols are point-in-time historical symbols and can be reused on different
  dates; and
- instrument definitions are point-in-time data.

Therefore neither a naked `instrument_id` nor a naked raw OSI symbol is a stable
cross-date economic id. This sprint retains `(trade_date, instrument_id,
raw_symbol)` as source provenance, verifies the daily mapping, and continues to
key fitted surfaces by canonical underlier symbol/uid. A future corporate-action
module may map generations; this sprint makes ambiguity visible instead of
pretending the daily id is permanent.

Primary references:

- Databento standards/symbology:
  <https://databento.com/docs/standards-and-conventions/symbology>
- Databento schemas and point-in-time definitions:
  <https://databento.com/docs/schemas-and-data-formats/cbbo>
- OPRA.PILLAR dataset/schema coverage:
  <https://databento.com/datasets/OPRA.PILLAR>

### 2.5 Archive throughput

The archive format has strong correctness properties already: fixed framing,
schema hash, layered CRC-32C, deterministic symbol order, O(1) symbol lookup, and
owned reconstructed surfaces. This sprint preserves those properties.

The performance opportunities are narrower than "make it zero-copy":

- blob materialization and blob CRC are independent by surface and can write
  disjoint preplanned offsets;
- buffered `open_file` pays an avoidable whole-file allocation and copy;
- lookup/directory metadata can be views into owned/mapped immutable bytes; but
- a normal `PricedSurface` still owns polymorphic curve objects, so an mmap does
  **not** make `map_all()` zero-copy.

We will call the new path "mapped open," not "zero-copy surfaces." Archive-backed
curve views are a separate redesign and are out unless P4's measured spike proves
they are required to meet the system budget.

### 2.6 Why qualification precedes the listed-options backtest

The public Cboe description of traditional dispersion sells an ATM index straddle
and buys ATM straddles on component stocks. It is a many-board trade, so one silently
bad single-name surface can corrupt contract sizing, Greeks, and P&L. The next sprint
therefore consumes only admitted, serialized surfaces and reconciles every selected
listed contract to OPRA. This sprint makes those inputs auditable and reproducible.

---

## 3. Scope and non-goals

### In scope

1. A first-class, deterministic fit-quality record with `NA` semantics.
2. Profile-aware admission/quarantine rules applied to the final fitted surface.
3. OOS scoring of the selected curve without rerunning the whole selector ladder.
4. Corpus `FitContext` wiring for event/HTB/session facts.
5. Per-cell market inputs: per-date rate pillars, per-symbol cash dividends, and
   per-cell fit context.
6. Per-expiry rate use in fit and `PricedSurface` repricing, flat-rate bit identity.
7. OPRA source provenance for `(date, instrument_id, raw_symbol)` and a strict
   pilot mode that rejects incomplete/ambiguous provenance.
8. A date-streaming corpus builder with bounded live surface memory and a
   compatibility wrapper for today's `build_corpus`.
9. A quality/admission report covering every planned `(date, symbol)` cell,
   including missing, corrupt, empty, failed, quarantined, and admitted cells.
10. A mixed-profile synthetic corpus, cached-real breadth corpus gate, and
    deterministic fuzz/property corpus.
11. Parallel deterministic archive materialization where it wins.
12. Buffered and mapped archive-open modes, accurately named and benchmarked.
13. A corpus/archive Google Benchmark matrix, checked-in baseline JSON, and an
    asserted regression comparison on the pinned runner.
14. End-to-end archive reload plus the existing ATM-straddle dispersion smoke.

### Explicit non-goals

- Variance-swap/log-strip dispersion, vega/gamma/theta flatness modes, DSPX level
  calculation, correlation-swap validation, or published-index reconciliation.
  Those are the next methodology sprint.
- A paid Databento pull. Cached files and synthetic fixtures only. A later
  operator-gated pilot owns spend.
- Native discrete-dividend American exercise. This sprint continues the existing
  escrowed-forward treatment, records it in provenance, and tests it honestly.
- A corporate-action master. We preserve point-in-time source identity and reject
  ambiguity; we do not infer economic continuity across splits/mergers.
- New curve families or changed fit mathematics.
- A new archive format solely for performance. Version/schema changes require a
  correctness need from term rates or the already-landed correction-cache payload.
- Claiming mmap makes reconstructed `PricedSurface` objects zero-copy.
- Absolute GB/s marketing targets copied from unrelated serialization formats.

---

## 4. Locked behavioral contracts

1. **Default compatibility.** Existing `build_corpus(boards, out, CorpusConfig{})`
   retains its public signature and produces numerically identical fitted surfaces
   and legacy manifest semantics. Qualification is a new API/configuration path.
2. **No missing-as-zero.** Unmeasured quality fields serialize as `NA` with an
   explicit presence bit in memory. Zero is a real measured value only.
3. **No silent cell loss.** Every cell in the run spec produces exactly one quality
   row and one terminal disposition.
4. **Score what ships.** Admission evaluates the final curve/preset/fallback and
   the exact surface that would be archived, never only the rejected primary.
5. **Quarantine is not failure.** Source errors, fit errors, and a successful fit
   rejected by policy remain distinct.
6. **Profile-specific floors.** There is no universal SPY-derived threshold for
   thin or wide boards. Rules are keyed by `ProfileKind`, with a required minimum
   sample count before ratio metrics can pass.
7. **Reference versus production is recorded.** Quality and archive fingerprints
   include the fit preset, pricing mode, curve kind, fallback use, archive schema,
   and any correction-cache fingerprint established by the performance sprint.
8. **Term-rate truth.** With a term curve, expiry `i` is de-Americanized,
   discounted, archived, and repriced with `r(T_i)`. With a flat curve, every
   result remains bit-identical.
9. **Point-in-time identity.** Databento `instrument_id` is scoped to a trade date.
   It is never promoted to the stable cross-date surface uid.
10. **Deterministic bytes when requested.** Strict corpus builds require a fixed
    archive creation stamp/build fingerprint; thread count cannot change archive
    bytes, quality rows, or manifest rows.
11. **Bounded memory.** The streaming builder holds at most the configured in-flight
    date batches plus the current date's fitted surfaces, not the whole corpus.
12. **Performance assertions are runner-qualified.** A pinned runner id, compiler,
    ISA, power plan, and archive schema must match the baseline. Other machines
    report measurements but do not compare against the wrong baseline.
13. **Honest retention.** An optimization that misses its stated crossover or
    regresses p99/correctness is removed; the measured negative remains in the
    decision report.

---

## 5. Target public model

Names are indicative. Reuse landed performance-sprint types where they already
solve the contract.

```cpp
enum class CorpusDisposition : std::uint8_t {
  Admitted,
  Quarantined,
  SourceFailed,
  FitFailed,
  Empty,
};

enum class CorpusAdmissionReason : std::uint16_t {
  None,
  MissingSource,
  InvalidSourceSchema,
  AmbiguousSourceIdentity,
  EmptyBoard,
  FitError,
  QualityUnavailable,
  TooFewQuotes,
  TooFewSlices,
  TooFewHoldouts,
  InBandBelowFloor,
  OosInBandBelowFloor,
  OosVegaWeightedBelowFloor,
  VolRmseAboveCeiling,
  ReducedChi2AboveCeiling,
  CalendarArbitrage,
  NonFiniteMetric,
  RoundTripMismatch,
};

template <class T>
struct Measured {
  T value{};
  bool present{false};
};

struct CorpusQualityMetrics {
  ProfileKind profile{};
  FitDecisionSource decision_source{};
  FitPreset preset{};
  VolCurveKind primary_kind{};
  VolCurveKind final_kind{};
  bool used_fallback{false};
  bool provenance_complete{false};
  std::uint32_t n_raw_quotes{};
  std::uint32_t n_two_sided{};
  std::uint32_t n_slices{};
  std::uint32_t n_holdout{};
  Measured<double> fit_in_band{};
  Measured<double> oos_in_band{};
  Measured<double> oos_vega_weighted{};
  Measured<double> mean_vol_rmse{};
  Measured<double> mean_reduced_chi2{};
  Measured<std::uint32_t> calendar_violations{};
};

struct CorpusAdmissionRule {
  std::uint32_t min_quotes{};
  std::uint32_t min_slices{};
  std::uint32_t min_holdout{};
  std::optional<double> min_fit_in_band{};
  std::optional<double> min_oos_in_band{};
  std::optional<double> min_oos_vega_weighted{};
  std::optional<double> max_mean_vol_rmse{};
  std::optional<double> max_mean_reduced_chi2{};
  bool require_calendar_arb_free{true};
  bool require_source_provenance{false};
};

struct CorpusAdmissionPolicy {
  bool enabled{false};
  std::array<CorpusAdmissionRule, kProfileKindCount> by_profile{};
};

struct QualifiedCorpusEntry {
  std::string date;
  std::string symbol;
  CorpusDisposition disposition{};
  CorpusAdmissionReason reason{};
  ErrorCode source_or_fit_error{ErrorCode::Unknown};
  CorpusQualityMetrics quality{};
  std::string archive_path;
};
```

Do not force the old `CorpusManifest` to carry every new field. Keep
`manifest.tsv` readable by current backtest code and add a versioned,
round-trip-safe `quality.tsv` (or `quality.jsonl` if the landed artifact utilities
already standardize JSONL). `QualifiedCorpusManifest` owns both views and asserts
their admitted rows agree.

### Date-streaming builder

```cpp
class CorpusBuildSession {
 public:
  static Result<CorpusBuildSession> create(std::string_view out_dir,
                                           const QualifiedCorpusConfig&);

  Status append_date(std::string_view date,
                     std::span<const CorpusCellInput> cells);

  Result<QualifiedCorpusManifest> finish();
};
```

`append_date` requires strictly ascending dates and unique canonical symbols
within the date. It fits admitted candidates in parallel, writes the date archive
atomically, records all dispositions, then releases frames/surfaces before the
next call. `finish` writes the legacy manifest and quality report atomically.

`build_corpus` remains a wrapper: stable-sort the supplied boards, construct date
batches, and delegate to the shared engine with admission disabled. There must not
be two fit implementations.

`CorpusCellInput` represents both a loaded `CorpusBoard` and a planned cell with
a source failure. Use a tagged variant, not sentinel empty frames.

---

## 6. Work packages

### P0 - Post-performance rebase audit and baseline

**Estimate:** 1 engineer-day. **Risk:** low. **Depends on:** completed performance
sprint.

#### P0.1 Rebase and API inventory

Record the performance-sprint merge SHA and re-audit:

- `PricedSurface` local-state/query/cache ownership;
- archive header/blob/slice layouts and schema hash;
- benchmark utilities, baseline comparator, runner metadata, and counters;
- persistent executor/workspace APIs;
- stateful `MarketSnapshot`/backtest load path; and
- archive compatibility tests.

Update this plan only where landed names/layouts differ. Do not start feature
implementation against unresolved performance-sprint changes.

#### P0.2 Clean correctness baseline

From a clean Release build, capture:

- full `atx_vol` test count and wall time;
- synthetic multi-name backtest result/fingerprint;
- cached real SPY/breadth fit results when fixtures exist;
- current archive bytes for a fixed deterministic fixture; and
- v3/vNext archive round-trip results.

#### P0.3 Canonical corpus/archive benchmark fixture

Add one shared data-free fixture builder under `bench/support/`:

- profiles: dense index, liquid ETF, mega-cap/event C8, ordinary eSSVI/SVI,
  sparse/wide, low-price, and HTB/dividend metadata;
- surface counts: 16, 64, 256, 1024 per date;
- 6 expiries per surface by default;
- dense node counts representative of landed policy; and
- optional post-performance correction-cache payloads in the production case.

The fixture is deterministic and constructed outside timed regions. It emits
payload mix, logical bytes, padded archive bytes, surface count, slice count, and
cache bytes as benchmark counters.

#### P0.4 Baseline measurement

Extend the landed Google Benchmark harness; do not use a gtest wall clock. Measure:

| Benchmark | Required counters |
|---|---|
| in-memory serialize | bytes/s, surfaces/s, slices/s, worker count |
| atomic file persist | file bytes/s, p50/p95, temp bytes |
| buffered open | archive bytes/s, transient bytes |
| mapped open | open us, mapped bytes, page-fault mode |
| random `map_symbol` | surfaces/s, blob bytes/s |
| bulk `map_all` | surfaces/s, slices/s, reconstructed blob bytes/s |
| one date append (fit excluded) | cells/s, admitted surfaces/s, bytes/s |
| end-to-end date append | cells/s, fit ms, quality ms, serialize ms |

Use at least 0.5 s warm-up, 7 repetitions for the archive suite, median/p95/CV,
and JSON output. Run CPU/CRC paths hot; label file-cache state instead of calling a
warm filesystem run "cold."

**P0 acceptance:** clean correctness gate; canonical JSON captured as
`before.json`; every metric has a nonzero denominator; CV <=5% for in-memory and
<=10% for file-I/O cases or the case is labeled noisy and excluded from hard
comparison.

---

### P1 - Quality scoring and admission contract

**Estimate:** 3 engineer-days. **Risk:** medium. **Depends on:** P0.

#### P1.1 Pure quality evaluator

Extract/reuse session diagnostic scoring into a pure function that evaluates the
final fitted surface against the original board. It must not mutate the fit or
surface. It reports presence explicitly.

For direct routes with `require_oos=true`, add a selected-family OOS scorer:

1. use the same deterministic even/odd split and quote filter as `CurveSelector`;
2. fit only the **final selected family** on the training half;
3. score only held-out observations;
4. use the final preset/carry/dividend/rate policy; and
5. record `n_holdout`; no held observations means unavailable, not zero.

Expose the single-family scorer from `curve_selector` detail or a new
`fit_quality` module. Do not make the corpus call a second all-family selector.

#### P1.2 Metric definitions

Lock these definitions in headers and tests:

- `fit_in_band`: fraction of scorable observations whose re-Americanized fair
  value lies in the cleaned bid/ask;
- `oos_in_band`: the same fraction on held-out observations;
- `oos_vega_weighted`: sum of the existing selector weights for in-band heldouts
  divided by all scorable heldout weights;
- `mean_vol_rmse`: mean of per-expiry RMSE in implied-vol units over scored
  expiries;
- `mean_reduced_chi2`: mean of per-expiry reduced chi-square using the existing
  parity implementation; and
- `calendar_violations`: count over the same declared k-grid used by the served
  surface acceptance check.

Ratios always carry numerator and denominator in the detailed report so a 100%
score on two quotes cannot masquerade as broad evidence.

#### P1.3 Deterministic admission decision

Implement a pure `evaluate_admission(metrics, rule)` with a fixed reason priority:

1. source/provenance unavailable;
2. non-finite measured field;
3. sample-count insufficiency;
4. calendar arbitrage;
5. in-sample floor;
6. OOS floor;
7. RMSE/chi-square ceiling.

Return the first primary reason and a bitset/list of all failed predicates for
diagnostics. Stable priority makes reports bit-identical and reviewable.

#### P1.4 Wire `FitContext` and final fallback provenance

Add `FitContext` to `CorpusBoard`/qualified input. Copy the final
`PricerFitter::decision`, including `primary_curve`, `curve`, `source`, preset,
and `used_fallback`, into quality metrics. Assert the reported final curve equals
every archived slice kind where the policy requires one family.

#### P1.5 Quality report serialization

Write a versioned deterministic quality report:

- full round-trip precision for doubles;
- `NA` for absent fields;
- stable enum integer plus stable text name, or a schema table pinned by tests;
- rows sorted date then canonical symbol;
- a header fingerprint covering policy/rules, fit/pricing mode, compiler/ISA,
  archive schema, and input-spec hash; and
- aggregate counts/distributions recomputed and verified during parse.

Timing data does **not** enter the deterministic quality artifact. Benchmark JSON
owns timings.

**P1 tests:** direct route no longer fabricates OOS=0; pinned route with OOS
disabled reports `NA`; fallback scores the final rung; each rejection predicate;
multiple failures obey priority; serialization round trip; default admission-off
path preserves today's fitted surfaces and legacy manifest.

**P1 acceptance:** a successfully fitted engineered one-sided board is
`Quarantined`, not `Failed` or `Admitted`; its exact failed metrics and reason are
present. A good sparse board can pass its sparse-profile rule without being held
to the SPY rule.

---

### P2 - Point-in-time OPRA provenance and per-cell market inputs

**Estimate:** 3 engineer-days. **Risk:** medium. **Depends on:** P1 can proceed in
parallel after shared types settle.

#### P2.1 Preserve Databento source identity

Extend the OPRA Parquet schema additively with `instrument_id` from every CBBO
record. Keep `symbol` as the mapped point-in-time raw OSI symbol. The loader:

- accepts legacy files without `instrument_id` in compatibility mode;
- in strict mode requires it;
- asserts one `(trade_date, instrument_id)` maps to one raw symbol in the file;
- records unmapped numeric-symbol fallbacks as incomplete provenance; and
- never uses `instrument_id` as the stable surface uid.

Store the source id on the kept observation or in an aligned provenance plane;
do not duplicate raw strings per numerical quote if a dictionary/index is cheaper.

#### P2.2 Per-cell market input table

Add a deterministic sorted value table keyed by canonical `(date, symbol)` with:

- optional spot override;
- per-date yield-curve pillars;
- per-symbol cash-dividend schedule visible as of that date;
- `FitContext` (session/event/HTB/vol-product facts); and
- a source/as-of tag for each external input.

`OpraBatchSpec` global rate remains the compatibility fallback. The qualified
pipeline requires explicit policy on missing market inputs: `UseFallback`,
`Quarantine`, or `Error`. The real pilot will use `Quarantine`/`Error`, never
silently flat zero rates and zero dividends.

#### P2.3 Thread cash dividends into every cell

Populate `OpraLoadSpec::cash_divs`, `QuoteFrame::divs`, `MarketEnv`,
`SessionInputs`, the quality report, and surface/archive provenance from the cell
table. Record the treatment as `EscrowedForward`; zero-dividend input remains
bit-identical.

#### P2.4 Per-expiry rate plumbing

Use `r_i = rate_at(T_i)` for each expiry during de-Americanization, discounting,
fit scoring, and re-Americanization. The minimum-compatible design is:

1. populate an expiry-aligned rate vector from `OptionChain::env()`;
2. pass it through the surface-fit inputs, with empty meaning legacy scalar `r`;
3. build each curve with `df_i = exp(-r_i*T_i)`;
4. compute each `q_eff_i = r_i - log(F_i/S)/T_i`;
5. resolve `(F, r, q_eff)` together in `PricedSurface`; and
6. use resolved `r(T)` in price/Greeks/delta.

Prefer deriving archived `r_i` from the already-stored per-slice `df` rather than
growing `ArchiveSliceHeader`. Validate `df > 0`, `T > 0`, and flat-rate equality.
Old archives use blob `PricingContext::r` as the compatibility fallback.

#### P2.5 Provenance fingerprint

The quality header fingerprints:

- sorted planned cells;
- per-cell market-input content/as-of tags;
- source schema version;
- archive/fit/pricing config; and
- fixed archive creation stamp.

It must not hash absolute local file paths or timing values.

**P2 tests:** daily instrument-id reuse across two dates is legal and remains two
date-scoped source keys; one id mapping to two symbols on one date is rejected;
legacy Parquet compatibility; strict provenance rejection; two-expiry non-flat
rate case against hand calculations; flat curve bit identity; dividend-heavy
synthetic forward/early-exercise check; no-lookahead/as-of ordering.

**P2 acceptance:** a qualified multi-date corpus can prove which point-in-time
quotes, rate curve, dividends, and fit context produced every admitted surface.
The non-flat curve changes only the intended term-rate case.

---

### P3 - Streaming qualified corpus and robustness battery

**Estimate:** 4 engineer-days. **Risk:** medium-high. **Depends on:** P1, P2.

#### P3.1 Shared date-streaming engine

Implement `CorpusBuildSession` and refactor current `build_corpus` through it.
Within one date:

1. validate/canonicalize all planned symbols;
2. retain source failures as terminal rows;
3. fit loaded boards in parallel with disjoint slots;
4. score each final fit;
5. decide admission;
6. restamp admitted surfaces with stable symbol uids;
7. archive admitted surfaces in canonical symbol order;
8. validate a readback sample or all surfaces per strictness policy; and
9. release the date's frames, sessions, and surfaces.

`max_inflight_dates=1` is the deterministic default. More than one in-flight date
is opt-in and must commit results in date order.

#### P3.2 Crash-safe output and resume

Each date archive is temp-write + atomic rename. `finish()` writes quality and
manifest temp files, fsync/close according to existing platform policy, then
renames. A resumable build may reuse an existing date archive only when its input,
policy, and archive fingerprints match; otherwise rebuild or fail loudly.

Do not treat "file exists" as a cache hit.

#### P3.3 Synthetic multi-profile corpus

Build a deterministic fast-gate fixture with:

- 1 index plus at least 12 names;
- at least 3 dates;
- dense ETF, ordinary liquid, sparse, one-sided, low-price, event, HTB/dividend,
  missing-source, corrupt-source, and deliberately bad-fit cases;
- at least one fallback fit; and
- at least one date that falls below the dispersion minimum survivor count.

Every planned cell gets a quality row. Admitted weights renormalize; quarantined
names follow the same missing-name policy as absent names but retain the distinct
reason in diagnostics.

#### P3.4 Cached-real breadth corpus

Turn the existing QQQ/IWM/XOM/SOUN/VXX/AAPL/AMZN fixtures plus SPY fixtures into a
qualified corpus test when files are present. It must:

- use per-fixture `FitContext` for opening/closing/event/vol-product regimes;
- report per-profile and per-symbol distributions;
- pin only justified floors with sample counts;
- skip cleanly when the complete declared fixture set is absent; and
- perform no network access.

VXX is diagnostic unless the dispersion universe intentionally includes a vol
product. Its current 87-92% price-in-band result must not weaken equity admission
rules.

#### P3.5 Property/fuzz corpus

Add a fixed-seed generator varying:

- spot/price level;
- strike density and missing sides;
- spread and size;
- skew/term/event jump;
- rate/yield sign and term shape within supported pricing regimes;
- dividend schedule and HTB context; and
- corrupt/non-finite source fields.

Fast gate: at least 250 boards across the profile buckets. Long/overnight gate:
at least 10,000. Required properties:

- no crash/terminate;
- exactly one disposition per cell;
- admitted surfaces are finite and calendar-clean under the declared grid;
- non-admitted cells have a reason;
- archive round trip preserves admitted query results; and
- worker count does not change results or bytes in strict deterministic mode.

#### P3.6 End-to-end existing-strategy smoke

Load the admitted 3-date synthetic archives into `MarketSnapshot`, resolve the
symbol-authored universe, run the existing ATM-straddle dispersion strategy, and
assert:

- completion under quarantined/missing cells;
- drop/renormalize diagnostics name the correct reason;
- no unpriced Greeks are silently treated as zero;
- P&L attribution closure remains at its existing tolerance; and
- results are bit-identical across worker counts/pricing mode where the mode
  promises identity.

This is a corpus/lifecycle gate, not the final QIS methodology claim.

**P3 acceptance:** the S1 acceptance gap is closed on index + at least 12 names x
3 dates; the complete quality scoreboard is deterministic; peak live fitted
surfaces is O(max symbols per date), verified by a counter; and the admitted
corpus reloads into an end-to-end dispersion smoke.

---

### P4 - Archive materialization and mapped-open performance

**Estimate:** 4 engineer-days. **Risk:** medium. **Depends on:** P0; integrate
after archive correctness changes from P2/P3 are stable.

#### P4.1 Deterministic parallel blob materialization

Refactor `write_surface_archive` into explicit stages:

1. scalar validate/canonicalize/sort/plan;
2. assign final offsets and lookup ownership;
3. allocate the final zero-initialized byte buffer once;
4. materialize each blob and compute its CRC in parallel into disjoint spans;
5. join;
6. populate lookup/directory CRC fields in canonical order; and
7. compute header/metadata CRC and finalize.

Use the landed persistent executor. Workers must not patch shared lookup slots or
resize shared containers. The scalar writer remains available as the reference
and small-archive crossover path.

Gate byte equality against the reference writer at worker counts 1/2/4/8 for all
curve kinds, optional cache payloads, alignment settings, and an injected worker
error. No partial output escapes on failure.

#### P4.2 Crossover dispatch

Measure 16/64/256/1024 surfaces. Select parallel execution only above the measured
crossover (surface count or planned payload bytes). Retain parallel materialization
only if:

- p50 improves >=20% on at least the 256/1024 production-stress cases;
- p95 does not regress >10%; and
- the below-crossover scalar case stays within 5%.

If current serialization already consumes <10% of date-build time and fan-out
does not meet the threshold, keep scalar and ship the honest negative. The
regression gate still closes the northstar performance contract.

#### P4.3 Mapped archive storage

Introduce an internal immutable byte-storage abstraction with two owners:

- owned buffer (existing `open(vector<byte>)` and buffered `open_file`); and
- read-only file mapping on Windows, with a platform fallback.

Public options:

```cpp
enum class ArchiveOpenMode { Auto, Buffered, Mapped };
struct SurfaceArchiveOpenOpts {
  ArchiveOpenMode mode{ArchiveOpenMode::Auto};
  ArchiveVerifyMode verify{ArchiveVerifyMode::Full};
};
```

`directory`, lookup, find, and CRC validation read immutable spans. Mapping
handles and views use RAII and are safe under move. The archive owns the mapping
until all reconstruction/query calls finish.

Mapped and buffered modes must return identical errors for truncation, bad bounds,
bad CRC, unknown kind, and schema mismatch.

#### P4.4 Reconstruction allocation audit

Instrument logical reconstruction allocations/copies. Separate unavoidable final
ownership (curve objects/nodes) from transient work. Remove only proven transient
churn. `map_all_into` must not call `map_all` and then move a temporary vector if
the landed code still does so (`surface_archive.cpp:1019-1027` pre-sprint).

Do not add a general arena that outlives or aliases reconstructed surfaces without
a clear ownership contract.

#### P4.5 Archive-backed view spike with a kill gate

Time-box to one engineer-day, charged within P4 only if mapped-open plus normal
reconstruction fails the system budget. Sketch an archive-backed read-only curve
view compatible with `PricedSurface`/portfolio queries. Ship it only if all hold:

- >=2x bulk-load improvement on 64 and 256 surface dates;
- no query-throughput regression >5%;
- CRC/schema lifetime is explicit and safe;
- no vtable/type explosion per curve kind; and
- archive-close-while-borrowed is impossible by type.

Otherwise record the spike and stop. It is not required merely to use the word
"zero-copy."

**P4 acceptance:** reference and production writers are byte-identical;
buffered/mapped readers are result-identical; strict corruption tests pass; the
selected crossover is measured; peak transient bytes and throughput are printed;
and archive time meets the system budgets in Section 8.

---

### P5 - Committed performance gate and release evidence

**Estimate:** 2 engineer-days. **Risk:** low-medium. **Depends on:** P3, P4.

#### P5.1 Final benchmark matrix

Run the P0 matrix against both reference and selected production routes. Add a
corpus-scale case representing 64 symbols x 21 dates, streamed one date at a time;
do not fabricate one 1,344-surface date if production archives are per date.

Report fit, quality, serialize, persist, open, and reconstruct separately. A
single end-to-end cells/s number without phase shares is rejected.

#### P5.2 Checked-in baseline

Extend the performance sprint's runner-specific baseline with archive/corpus
cases. Baseline metadata includes:

- runner id and CPU;
- OS/power plan;
- compiler/version/configuration;
- ISA/math/pricing mode;
- worker topology;
- archive schema/cache fingerprint;
- fixture fingerprint; and
- benchmark command.

Commit the canonical optimized JSON plus a small human-readable decision report.
Raw exploratory files remain outside git.

#### P5.3 Asserted comparison

On a matching pinned runner, fail when a required case has:

- median throughput regression >10% with both baseline/current CV <=5%;
- p95 latency regression >15%;
- peak transient bytes regression >10%;
- archive bytes/surface regression >10% without an approved schema payload; or
- missing/renamed benchmark cases.

File-I/O cases with CV 5-10% require two independent runs before failure. CV >10%
is `NOISY` and fails the benchmark job as inconclusive; it never passes silently.

On an unmatched runner, print `SKIP_BASELINE_MISMATCH` and still emit JSON. The
normal correctness suite never depends on timing.

#### P5.4 System budgets

The sprint does not close on a ratio gate alone. On the pinned runner and canonical
64-name date:

1. in-memory serialization + CRC is <=10% of fit+quality wall time;
2. atomic persistence is <=15% of fit+quality wall time on the declared local SSD;
3. buffered or mapped open + `map_all` is <=10% of one post-performance backtest
   step wall time, or the selected archive-backed-view task must ship;
4. streaming peak fitted-surface count is <=2 x max admitted symbols in one date
   at the default one-date setting; and
5. output is byte-identical across worker counts in strict mode.

These budgets define "high performance" relative to the producer/consumer that
matters. Absolute MB/s and surfaces/s are still reported and baselined.

#### P5.5 Documentation and operator artifact

Update `atx-vol/README.md` with:

- qualified corpus lifecycle;
- quality metric/NA semantics;
- admission versus fit failure;
- source identity scope;
- buffered versus mapped open (no false zero-copy claim);
- pinned baseline table generated from JSON; and
- exact commands to rebuild/compare the baseline.

Emit one example quality scoreboard and phase-timing report from the synthetic
corpus. Do not commit licensed OPRA payloads.

**P5 acceptance:** the comparison tool demonstrably fails on an injected 2x
slowdown and missing-case fixture; passes after removal; full correctness suite is
green; system budgets hold; baseline and decision report are committed.

---

## 7. Task ledger and dependency order

| ID | Deliverable | Est. | Depends on | Proof |
|---|---|---:|---|---|
| P0-1 | post-performance API/archive audit | 0.25 d | perf sprint | recorded SHA + map |
| P0-2 | clean correctness baseline | 0.25 d | P0-1 | test log/fingerprints |
| P0-3 | canonical mixed-profile archive fixture | 0.25 d | P0-1 | deterministic fixture hash |
| P0-4 | before benchmark JSON | 0.25 d | P0-3 | median/p95/CV JSON |
| P1-1 | pure final-surface quality evaluator | 0.75 d | P0 | unit/reference tests |
| P1-2 | selected-family OOS scorer | 0.75 d | P1-1 | heldout oracle tests |
| P1-3 | admission rules/reasons | 0.5 d | P1-1 | predicate battery |
| P1-4 | corpus FitContext/fallback provenance | 0.5 d | P1-1 | route/fallback tests |
| P1-5 | quality report round trip | 0.5 d | P1-3 | parse/serialize identity |
| P2-1 | OPRA instrument-id provenance | 0.75 d | P0 | daily mapping tests |
| P2-2 | per-cell market-input table | 0.5 d | P2-1 | lookup/as-of tests |
| P2-3 | dividend/context wiring | 0.5 d | P2-2 | zero-div identity + event/HTB |
| P2-4 | per-expiry rate fit/query/archive | 1.0 d | P2-2 | non-flat oracle + flat identity |
| P2-5 | source/config fingerprint | 0.25 d | P2-1..4 | mutation battery |
| P3-1 | shared date-streaming build engine | 1.25 d | P1, P2 | wrapper parity + peak-state counter |
| P3-2 | crash-safe/resumable date commit | 0.5 d | P3-1 | interruption/fingerprint tests |
| P3-3 | 12-name x 3-date synthetic corpus | 0.75 d | P3-1 | S1 acceptance test |
| P3-4 | cached-real breadth qualification | 0.5 d | P3-1 | skip-clean real gate |
| P3-5 | property/fuzz corpus | 0.5 d | P3-1 | 250 fast / 10k long |
| P3-6 | existing dispersion e2e smoke | 0.5 d | P3-3 | deterministic P&L/drop report |
| P4-1 | parallel byte-identical writer | 1.25 d | P0 | worker matrix + before/after |
| P4-2 | measured crossover dispatch | 0.25 d | P4-1 | crossover JSON |
| P4-3 | mapped archive storage | 1.0 d | P0 | buffered/mapped parity |
| P4-4 | transient allocation cleanup | 0.5 d | P4-3 | counter/memory report |
| P4-5 | archive-backed view spike (conditional) | <=1.0 d | P4-3/4 | ship/kill report |
| P5-1 | final canonical benchmark | 0.5 d | P3, P4 | optimized JSON |
| P5-2 | asserted baseline comparison | 0.5 d | P5-1 | injected-failure test |
| P5-3 | docs/artifacts/full closeout | 1.0 d | all | DoD checklist |

Nominal scope: 17 engineer-days, plus at most one conditional spike day. P1/P2
can overlap after P0. P4 can develop against the canonical fixture while P3 lands,
but final baseline waits for all archive/schema changes.

---

## 8. Acceptance matrix

### 8.1 Corpus correctness

| Case | Expected |
|---|---|
| good direct-profile board, parity scoring off | measured quality from explicit evaluator; OOS `NA` unless requested |
| good direct-profile board, OOS required | one-family heldout score, not all-family selector |
| primary fails, fallback succeeds | score/archive fallback; `used_fallback=true` |
| successful fit below floor | `Quarantined`, exact reason/metrics |
| source file missing | `SourceFailed/MissingSource`, no omitted row |
| empty board | `Empty`, not fit failure |
| bad source schema/id mapping | source failure or quarantine per strictness |
| sparse board with two heldouts | cannot pass a rule requiring more evidence even at 100% |
| one date all names unavailable | no-trade/hold semantics from S1-3 preserved |
| quarantine disappears next date | held-lot unpriced accounting remains explicit |

### 8.2 Market inputs

- flat rates and zero dividends: exact historical bits;
- two-pillar rates: per-expiry `r_i`, `df_i`, `q_eff_i`, price, and Greeks match a
  hand-calculated reference;
- archive round trip preserves term-rate queries;
- cash dividends and HTB/event context reach fit policy and provenance;
- future/as-of market inputs are rejected;
- daily Databento instrument-id reuse is accepted; same-day ambiguous mapping is
  rejected.

### 8.3 Archive correctness

- every curve kind and optional cache payload;
- 0/1/max surfaces and alignment/load-factor corners;
- scalar/parallel byte identity;
- buffered/mapped result and error identity;
- truncated header/lookup/directory/blob/slice;
- bad header/metadata/blob/payload CRC;
- unknown kind/schema mismatch;
- map one/all/repeated/concurrent const;
- mapping lifetime/move/close behavior; and
- v3/landed-version backward compatibility.

### 8.4 Determinism

For 1/2/4/8 workers compare:

- legacy manifest;
- quality report;
- archive bytes with fixed stamp;
- admitted/quarantined ordering and reasons;
- sampled and full reloaded surface queries; and
- end-to-end backtest columns.

Cross-ISA numerical tolerance follows the performance sprint. Same ISA/mode/build
must meet its declared bit contract.

### 8.5 Performance

- benchmark fixture construction excluded;
- logical and padded bytes both reported;
- unique surfaces/s and slices/s reported together;
- file-cache state labeled;
- CV/noisy behavior enforced;
- phase shares sum to end-to-end wall time within measurement overhead;
- Section 6 P5.4 system budgets hold; and
- injected regression/missing benchmark causes nonzero comparison exit.

---

## 9. File-level implementation map

Expected files after the P0 re-audit:

### Core corpus/quality

- `include/atx/vol/corpus.hpp`
- `src/corpus.cpp`
- new `include/atx/vol/fit_quality.hpp`
- new `src/fit_quality.cpp`
- `include/atx/vol/curve_selector.hpp`
- `src/curve_selector.cpp`
- `include/atx/vol/pricer_fitter.hpp`
- `src/pricer_fitter.cpp`
- `include/atx/vol/fit_policy.hpp`

### Market inputs and rate truth

- `atx-core/include/atx/external/databento.hpp`
- `atx-core/src/external/databento_pull.cpp`
- `include/atx/vol/opra_panel.hpp`
- `src/opra_panel.cpp`
- `include/atx/vol/opra_batch.hpp`
- `src/opra_batch.cpp`
- `include/atx/vol/data.hpp`
- `src/data.cpp`
- `include/atx/vol/surface_parity.hpp`
- `src/surface_parity.cpp`
- `src/curve_fit.cpp`
- `include/atx/vol/priced_surface.hpp`
- `src/priced_surface.cpp`

### Archive/performance

- `include/atx/vol/surface_archive.hpp`
- `src/surface_archive.cpp`
- `include/atx/vol/backtest.hpp` / `src/backtest.cpp` only if the landed stateful
  loader needs an open-mode/workspace seam
- `bench/CMakeLists.txt`
- new `bench/surface_archive_bench.cpp`
- new `bench/corpus_pipeline_bench.cpp`
- new/extended `bench/support/synth_corpus.hpp`
- performance sprint baseline/comparison files (extend, do not duplicate)

### Tests

- new `tests/fit_quality_test.cpp`
- new `tests/corpus_admission_test.cpp`
- new `tests/corpus_streaming_test.cpp`
- new `tests/corpus_robustness_test.cpp`
- new `tests/corpus_fuzz_test.cpp`
- `tests/multiname_pipeline_test.cpp`
- `tests/opra_breadth_corpus_test.cpp`
- `tests/opra_panel_test.cpp`
- `tests/opra_batch_test.cpp`
- `tests/priced_surface_test.cpp`
- `tests/surface_archive_test.cpp`
- new `tests/surface_archive_mapped_test.cpp` if platform guards stay clearer in a
  separate TU

Avoid adding all cases to the already-large `multiname_pipeline_test.cpp`; keep
ownership boundaries visible.

---

## 10. Build and verification commands

Use the repository presets/target names after P0 confirms the performance-sprint
merge. Expected commands:

```powershell
cmake --preset rel -DATX_BUILD_TESTS=ON -DATX_BUILD_BENCH=ON -DATX_VOL_COUNTERS=ON
cmake --build --preset rel --target atx-vol-tests atx-vol-surface-archive-bench atx-vol-corpus-bench -j

$env:ATX_VOL_FIT_WORKERS='1'
ctest --test-dir build-rel -L atx_vol -j16 --output-on-failure --timeout 900

build-rel\bin\atx-vol-surface-archive-bench.exe `
  --benchmark_out=atx-vol\bench\results\surface-archive-after.json `
  --benchmark_out_format=json

build-rel\bin\atx-vol-corpus-bench.exe `
  --benchmark_out=atx-vol\bench\results\corpus-after.json `
  --benchmark_out_format=json

python atx-vol\bench\compare_baseline.py `
  --baseline atx-vol\bench\baselines\i7-1260p-clang18-archive.json `
  --candidate atx-vol\bench\results\surface-archive-after.json `
  --runner-id i7-1260p-win11
```

Leave `ATX_VOL_FIT_WORKERS` unset for a single corpus-fit benchmark so across-board
fan-out is measured. Keep it set to 1 under parallel ctest to avoid process x fit
oversubscription, consistent with the pf2 S0 finding.

Add sanitizer/static-analysis commands from the performance sprint closeout if
they landed. No paid data command belongs in this sprint.

---

## 11. Risks and mitigations

| Risk | Mitigation |
|---|---|
| admission overfits the same board it judges | explicit one-family heldout scorer; denominators persisted |
| direct route reports fake zero metrics | presence bits/`NA`; tests pin semantics |
| universal floor rejects valid sparse boards | profile-specific rules + min sample counts |
| fallback provenance names the primary | score/archive final curve; cross-check slice kinds |
| quality scoring doubles corpus time | only selected family; benchmark phase separately; configurable OOS requirement |
| term-rate change breaks flat archives | derive from existing per-slice df; exact flat identity gate; old-r fallback |
| per-cell market facts introduce lookahead | as-of tags, fingerprint, future-input rejection |
| daily instrument id treated as permanent | date-scoped source key; canonical surface uid remains separate |
| date streaming changes deterministic order | strictly ascending append + canonical within-date commit |
| resume reuses stale archive | input/policy/archive fingerprint match required |
| parallel writer races lookup/CRC | disjoint blob spans; scalar final metadata patch |
| mmap marketed as zero-copy | name it mapped open; report reconstruction allocations |
| mapped lifetime use-after-close | RAII owner inside `SurfaceArchive`; move/lifetime tests |
| perf CI flakes | pinned runner matching, CV rules, two-run I/O confirmation |
| post-performance cache payload dominates size | benchmark payload mix and bytes/surface; approved schema-size exception only |
| existing build is already fast enough | retain regression gate; keep optimization only at measured crossover |
| synthetic robustness is not market robustness | cached-real breadth gate now; paid diversified pilot remains later |

---

## 12. Definition of done

- [ ] implementation is based on the completed performance-sprint merge and its
      archive/benchmark APIs;
- [ ] existing `build_corpus` behavior remains compatible with admission off;
- [ ] every planned cell has exactly one terminal disposition and quality row;
- [ ] unmeasured metrics are `NA`, never zero;
- [ ] direct routes can obtain independent selected-family OOS evidence;
- [ ] the final fallback curve, not the primary, is scored and archived;
- [ ] profile-specific admission rules quarantine an engineered bad fit;
- [ ] `FitContext` reaches corpus routing for event/HTB/session facts;
- [ ] per-cell rates/dividends/as-of provenance are fingerprinted;
- [ ] each expiry fits and reprices with its own `r(T)`; flat-rate bits are held;
- [ ] OPRA Parquet preserves date-scoped instrument-id provenance;
- [ ] the builder streams dates and bounds peak live surfaces by date width;
- [ ] index + at least 12 names x at least 3 dates completes the qualified corpus
      and existing dispersion smoke;
- [ ] cached real breadth fixtures produce a per-profile scoreboard when present;
- [ ] 250-board deterministic fuzz/property gate is clean;
- [ ] reference and parallel archive writers are byte-identical;
- [ ] buffered and mapped archive reads are result/error identical;
- [ ] archive throughput, p95, CV, transient bytes, and phase shares are measured;
- [ ] Section 6 P5.4 system budgets hold on the pinned runner;
- [ ] checked-in runner-qualified baseline and comparison gate exist;
- [ ] an injected 2x slowdown and missing benchmark case fail the gate;
- [ ] full tests, warnings-as-errors, archive compatibility, and sanitizer gates
      are green;
- [ ] README and example quality/performance artifacts are committed without
      licensed OPRA data; and
- [ ] the pf2 progress ledger marks S1 fully closed and identifies the traditional
      SPY listed-options vega-flat backtest as the next sprint.

At close, the project has an auditable answer to two questions before it changes
strategy math: "Which real/synthetic boards were safe enough to trade, and why?"
and "Can the exact fitted artifact be persisted and reloaded at corpus scale
without becoming the bottleneck?" The next sprint can then run a traditional
vega-flat SPY/component ATM-straddle book on qualified, reloaded surfaces.

