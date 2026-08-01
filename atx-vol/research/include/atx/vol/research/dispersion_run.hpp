#pragma once

// Library seam for the traditional SPY listed-options dispersion proxy.
//
// The command bodies that used to live inside examples/spy_dispersion_backtest.cpp
// (~620 LOC of library workflow trapped in an example main) live here so the
// example is thin CLI glue and every stage is unit-testable off the filesystem.
//
// SEAM CONTRACT (S3-T17, 2026-07-29) — READ THIS BEFORE DELETING ANYTHING.
// This is a FULLY-DISPATCHED seam: every file-oriented entry point declared here
// backs a shipped `spy_dispersion_backtest` subcommand, and no subcommand keeps a
// second implementation of one. The per-entry-point half is at the "File-oriented
// workflow entry points" block below.
//
// Background, kept because it explains why the block below is so emphatic: the
// main -> feat/pipeline-m merge took main's version of the example wholesale,
// because main had hard-cut the CLI over to the `run.atxrun` RunArchive result
// container and the shared Python layer is written against that container. The
// side effect was that NO shipped binary called into this header, leaving a
// tested library with no production caller — the shape that rots quietly and is
// then deleted by someone who assumes it was always dead. RECONCILE 1 resolved
// that as a UNION: the three entry points where both designs wrote only loose
// TSVs (build-corpus, run-surface-backtest, run-projected-var) became one-line
// CLI dispatches, recovering X1/X2/X3/X4/X5/X6, C1-ACTIVATE and F4's
// `persist_typed_spec_keys`. Those three are the three that remain.
//
// The other three (`dispersion_build_schedule`, `dispersion_run_backtest`,
// `dispersion_verify`) were held as a documented RESERVE on the argument that
// dispatching into them would publish or verify a run directory the reporting
// layer cannot read. S3-T17 closed that: the argument was sound and the
// conclusion was backwards. The CLI bodies are not duplicates of the reserve —
// they are compositions of the library's own seams (`build_listed_dispersion_
// schedule_audited`, `make_listed_replay_run_config`, `reconcile_listed_schedule`,
// `RunDir::write_run_archive`, `RunDir::verify`), and the reserve had drifted
// away from them. The reserve was deleted, not dispatched into. See the block
// below for the divergences, and for the two CLI-owned inputs (`--cache`, the
// `PhaseTimer`) that make a run-dir-only library twin lossy by construction.
//
// Consequence for readers of the tests: WHERE `dispersion_run_test.cpp` covers an
// entry point declared here, that coverage IS coverage of a shipped CLI path,
// because every entry point here is dispatched. It drives
// `dispersion_run_surface_backtest` and `dispersion_run_projected_var` directly
// (`DispersionDividends.*`, `DispersionProjectedVar.*`, `DispersionBenchmarkJoin
// .*`) and `dispersion_build_corpus` through `DispersionIndexRouting.CorpusBuild
// *`. What it does NOT cover is the CLI's own orchestration — the subcommand
// bodies in examples/spy_dispersion_backtest.cpp have no gtest surface at all;
// they are exercised by the Python e2e module (atx-vol/python/tests/
// test_dispersion_runarchive_e2e.py, fixture-gated) and by the sprint gate legs.
//
// REPRODUCIBILITY: the pinned admission thresholds / fingerprint material that
// determine which surfaces are admitted -- and therefore the dispersion golden
// (final_nav = 24740.624124981368, 82-session) -- are named library constants on
// DispersionCorpusPolicy below. No example-only literal is load-bearing.
//
// UNITS (E1, 2026-07-25): `DispersionConfig::target_vega` is dollars of index
// gross vega per VOL POINT, so the default book is 100x the size it was before
// E1 and every $-denominated golden figure in this header is 100x its former
// value. Measured, 3x-stable, at feat/pipeline-m tip 6b6aa7e.
//
// HOW TO CITE ANOTHER FILE FROM A COMMENT (REV-MTIDY M-1, 2026-07-25).
// NAME THE SYMBOL FIRST -- the function, test, struct or member -- and treat the
// line number as a convenience that is ALLOWED to rot:
//
//     ...instead (in `verify_command`, spy_dispersion_backtest.cpp:342)
//
// not `(spy_dispersion_backtest.cpp:342)` alone. A name survives an insertion
// above it; a bare line number is silently falsified by one, and then points a
// reader at unrelated code with full confidence. That is not hypothetical here:
// the same defect has now been found and re-fixed FOUR times in this sprint, and
// re-deriving the numbers buys exactly one commit's worth of correctness -- the
// batch that fixed four stale pointers broke six more in its sister commit. For
// a citation into a revision that no longer exists, SHA-pin instead
// (`dispersion_run.cpp` does this once, as
// `b0080fa:examples/spy_dispersion_backtest.cpp:950,981-982`), which is immune.
// This is a convention for comments you TOUCH, not a mandate to sweep the repo.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"           // Clock, BacktestResult
#include "atx/vol/corpus.hpp"             // QualifiedCorpusConfig
#include "atx/vol/dispersion.hpp"         // DispersionUniverse
#include "atx/vol/research/dispersion_backtest.hpp"// DispersionBacktestConfig, run_dispersion_backtest
#include "atx/vol/research/dispersion_workflow.hpp"// RunSpec
#include "atx/vol/listed_dispersion.hpp"  // ListedQuoteQualityConfig (F6)
#include "atx/vol/research/listed_dispersion_pipeline.hpp" // ListedScheduleSpec (REV-MTIDY I-1)
#include "atx/vol/listed_dispersion_strategy.hpp" // ScheduleFillPolicy (F2)
#include "atx/vol/session.hpp"            // FitPreset
#include "atx/vol/tools/tearsheet.hpp"          // TearSheet
#include "atx/vol/types.hpp"              // Result, Status
#include "atx/vol/vol_surface.hpp"        // VolCurveKind

namespace atx::vol {

// ── Pinned corpus/admission policy ──────────────────────────────────────────
//
// These defaults are the *only* load-bearing reproduction knobs for the listed
// SPY dispersion corpus. They were previously inline literals in
// build_corpus_command; changing any of them changes admission and therefore the
// golden NAV. dispersion_corpus_config() below assembles the QualifiedCorpusConfig
// exactly as the example driver did.
struct DispersionCorpusPolicy {
  // Per-profile admission thresholds (applied uniformly to every profile rule).
  std::uint32_t admission_min_quotes{20};
  std::uint32_t admission_min_slices{2};
  bool admission_require_calendar_arb_free{true};
  double admission_calendar_abs_k{0.7};
  bool admission_require_source_provenance{true};
  // Fit template pinned to the HFT market-mark preset with a direct linear-in-
  // variance curve and the calendar floor enforced.
  FitPreset fit_preset{FitPreset::Hft};
  VolCurveKind fit_curve_kind{VolCurveKind::LinearVariance};
  bool fit_enforce_calendar_floor{true};
  // Deterministic policy fingerprint material hashed into the quality sidecar.
  std::string_view policy_fingerprint_material{
      "spy-listed-dispersion-admission-v4-pinned-linear-calendar-floor-k0.7"};
};

// Nonzero 64-bit hash of `text` (0 folded to 1) — the corpus fingerprint hash.
[[nodiscard]] std::uint64_t dispersion_hash_text(std::string_view text);

// Pinned input-fingerprint over the corpus identity: "date_lo|date_hi|n_symbols".
[[nodiscard]] std::uint64_t dispersion_input_fingerprint(std::string_view date_lo,
                                                         std::string_view date_hi,
                                                         std::size_t n_symbols);

// Assemble the QualifiedCorpusConfig for a listed-dispersion corpus build from the
// pinned policy, the across-board worker count, and the input fingerprint. Byte-
// for-byte equivalent to the literals that used to live in build_corpus_command.
[[nodiscard]] QualifiedCorpusConfig dispersion_corpus_config(const DispersionCorpusPolicy &policy,
                                                             unsigned fit_workers,
                                                             std::uint64_t input_fingerprint);

// ── Surface-only backtest compute seam ──────────────────────────────────────

struct DispersionBacktestOutcome {
  BacktestResult track{}; // the rich SoA PnL track
  TearSheet sheet{};      // headline metrics derived from `track`
};

// Map the run spec into the canonical surface-only backtest config (the block that
// used to be inline in run_surface_backtest_command).
[[nodiscard]] DispersionBacktestConfig
dispersion_backtest_config_from_run_spec(const RunSpec &spec);

// Run the canonical surface-only dispersion backtest over an already-qualified
// Clock and bundle the track + tearsheet. Wraps the existing
// run_dispersion_backtest(); artifact persistence stays with the caller.
//
// FROZEN-UNIVERSE overload: the passed membership is held for the whole run. Kept
// for callers that genuinely own a single static basket; the flagship file-driven
// path uses the schedule overload below.
[[nodiscard]] Result<DispersionBacktestOutcome>
run_dispersion_surface_backtest(const Clock &clock, DispersionUniverse universe,
                                const DispersionBacktestConfig &config);

// C1-ACTIVATE point-in-time overload. Threads the raw constituent `schedule` down
// to WS-C's PIT resolver so the basket is re-resolved on every step and a
// mid-window reconstitution (add / reweight / REMOVE) is honored at the next roll.
// This is what `dispersion_run_surface_backtest` now calls: before activation the
// file-driven path resolved the universe once at the first session date and froze
// it, so a schedule with more than one `effective_date` block was silently ignored.
// With a single-block schedule this is byte-identical to the frozen overload.
[[nodiscard]] Result<DispersionBacktestOutcome>
run_dispersion_surface_backtest(const Clock &clock, std::vector<UniverseRow> schedule,
                                const DispersionBacktestConfig &config,
                                std::string_view index_symbol = "SPY");

// ── X1: one strict typed run config ─────────────────────────────────────────
//
// The run spec used to be TSV-soup at the seam: `read_run_spec` produced a
// `map<string,string>` that was hand-mapped into ~6 of the ~20 honored keys, and
// ANY unrecognized key was silently ignored — so a typo (`gross_vega` for
// `gross_index_vega`) or a knob the surface path never wired (frictions, limits)
// looked accepted and did nothing. `DispersionRunConfig` is the single typed
// destination and `read_dispersion_run_config` is a STRICT deserializer: every
// key is either bound to a typed field or rejected BY NAME.
//
// DEFAULTS ARE THE FRICTIONLESS GOLDEN. Every field below defaults to the value
// the pinned 82-session run (final_nav = 24740.624124981368) already used, so a
// spec that omits a knob reproduces byte-for-byte. Realism is opt-in.

struct DispersionDateRange {
  std::string lo{};
  std::string hi{};
};

struct DispersionUniverseSpec {
  std::filesystem::path schedule_path{};
  std::string index_symbol{"SPY"}; // never a constituent
  std::size_t min_names{10};
  double min_weight_coverage{0.8};
};

// The flat discount rate. NOTE (bug this struct makes visible): `flat_rate` was
// only ever routed into the OPRA fit batch — never into the cash/borrow ledger —
// so a run could declare r = 4.3% and still accrue zero carry. Setting
// `apply_to_financing` routes the same rate into FinancingConfig as well.
struct DispersionRateSource {
  double flat_rate{0.0};
  bool apply_to_financing{false};
};

struct DispersionDteBands {
  double target_days{30.0};
  double min_days{21.0};
  double max_days{60.0};
};

// X4. `WeightingScheme` / `StrikeRule` (dispersion.hpp) are the vocabulary; only
// the values the sizing path actually implements are nameable in a spec, and an
// unimplemented value is a parse error rather than a silently-ignored key.
//
// Spec keys, all defaulting to the shipped construction:
//   weighting            vega_neutral (default) | equal_vega | gamma_neutral | theta_neutral
//   strike               atm_forward_straddle (default) | fixed_moneyness | delta_strangle
//   strike_log_moneyness fixed_moneyness only; 0 (default) == ATM forward
//   strike_abs_delta     delta_strangle only; default 0.25

// `DispersionCostModel` (X6) is declared in dispersion_backtest.hpp and
// `DispersionRiskLimits` / `RiskBreachAction` (X3) in strategy.hpp — they live
// next to the code that enforces them, and reach this header transitively.

struct DispersionFitConfig {
  unsigned workers{0}; // 0 => all hardware cores
  bool core_mode{false};
};

// ── REVIEW C-6: dated benchmark rows, joined to the strategy BY DATE ─────────
//
// The benchmark reader used to return the P&L column alone and `benchmark_stats`
// aligned by vector POSITION over `min(strategy.size(), benchmark.size())`. A
// shifted, reversed, duplicated, missing-date or short benchmark therefore
// produced entirely plausible alpha/beta/IR/tracking-error values for the WRONG
// observations, and the short case silently dropped the strategy's tail. The date
// was in the file the whole time; it was parsed and discarded.
struct DispersionBenchmarkRow {
  std::string date; // the file's own date key, verbatim
  double pnl{0.0};  // validated finite
};

// How a benchmark series is joined to the strategy's own return dates. Spec key
// `benchmark_join`.
enum class DispersionBenchmarkJoin : std::uint8_t {
  // DEFAULT (`exact`), and the only policy that claims a COMPLETE comparison:
  // the benchmark's dates must equal the strategy's return dates, one for one,
  // in order. Anything else is an error naming the first disagreement.
  ExactDates = 0,
  // OPT-IN (`inner`): keep only the sessions present in BOTH series and compare
  // over those. This is a real statistic over a REDUCED sample, not a repair — it
  // is named in the spec and echoed in the published report (`benchmark_join`,
  // alongside `benchmark_n_obs`) so a reader can see that the block does not
  // cover every strategy observation. It is deliberately NOT the default: silently
  // restoring a partial comparison is the behaviour C-6 is about.
  InnerJoinOnDates = 1,
};

[[nodiscard]] std::string_view to_string(DispersionBenchmarkJoin join) noexcept;

// ── X5: the friction/impact regime, as a first-class reported dimension ─────
//
// THIS IS NOT COSMETIC. Measured on the pinned 82-session run, the SAME strategy
// over the SAME surfaces returns:
//
//   Frictionless           +24740.62   (the reproducibility pin)
//   Frictioned             + 1280.83   (cost 23459.79 — 95% of the gross result)
//   FrictionedWithImpact   - 6460.23   (cost 31200.85 — the SIGN FLIPS)
//
// A headline number is therefore meaningless without its regime, and a tearsheet
// that reports only the frictionless figure is actively misleading. Every
// artifact the reporting path emits carries the regime in its metadata, and the
// renderer refuses to draw a run without one.
enum class DispersionFrictionRegime : std::uint8_t {
  Frictionless = 0,        // mid fills, no commission, no impact — the pin
  Frictioned = 1,          // a spread and/or per-contract cost, but no impact term
  FrictionedWithImpact = 2 // the above plus an active square-root impact model
};

[[nodiscard]] std::string_view to_string(DispersionFrictionRegime regime) noexcept;

// A one-line, human-readable description of the regime's actual parameters
// (e.g. "frictionless (mid fills, no commission, no impact)"), so a report can
// state WHICH frictions produced a number, not merely that some were applied.
[[nodiscard]] std::string dispersion_regime_detail(const FrictionModel &frictions,
                                                   const DispersionCostModel &costs);

struct DispersionRunConfig {
  std::string label{"SPY listed-options dispersion proxy"};
  DispersionDateRange dates{};
  std::string snapshot_suffix{"T19:55:00Z"};
  std::filesystem::path opra_root{};
  std::string path_template{"{symbol}/{date}.parquet"};
  std::filesystem::path definitions{};
  std::filesystem::path occ_ess_root{};
  std::filesystem::path dividend_ledger{};
  DispersionUniverseSpec universe{};
  DispersionRateSource rate{};
  DispersionSide side{DispersionSide::ShortIndexLongNames};
  WeightingScheme weighting{WeightingScheme::VegaNeutral};
  StrikePolicy strike{};
  DispersionDteBands dte{};
  double roll_dte_days{7.0};
  double gross_index_vega{10'000.0};
  HedgeSpec hedge{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  unsigned entry_every_n{21};
  bool record_diagnostics{false};
  FrictionModel frictions{};   // X2; default frictionless mid
  FinancingConfig financing{}; // X2; default no carry
  DispersionCostModel costs{}; // X6; default zero impact
  DispersionRiskLimits limits{};
  DispersionFitConfig fit{};
  SurfaceProvenancePolicy provenance{SurfaceProvenancePolicy::Compatibility};
  double multiplier{100.0}; // was hardcoded at every construction site
  // X5. Optional benchmark P&L series for the benchmark-relative block, as a
  // `date<TAB>pnl` TSV in the SAME units as the track ($ P&L). EMPTY BY DEFAULT:
  // absent => the tearsheet reports absolute statistics only and claims no
  // alpha/beta/IR/TE. Spec key `benchmark_series`.
  std::filesystem::path benchmark_series{};
  // C-6. How that series is joined to the strategy's return dates. Spec key
  // `benchmark_join` (`exact` | `inner`); the default demands an exact date
  // match and refuses anything else.
  DispersionBenchmarkJoin benchmark_join{DispersionBenchmarkJoin::ExactDates};
  // Periods per year for annualizing. 252 is the shipped convention.
  double periods_per_year{252.0};

  // ── WS-F F4 (BT-W): knobs the LISTED route had no way to reach ─────────────
  //
  // X2/X6 wired frictions, financing, limits and costs into the SURFACE
  // backtest (`dispersion_backtest_config_from`). The listed `run-backtest` —
  // the headline artifact — still built a default-constructed engine RunConfig
  // and set only `unpriced`, so every published listed NAV was frictionless,
  // carry-free and provenance-permissive REGARDLESS of the spec. F4 routes the
  // same typed config into it via `dispersion_engine_run_config_from`.
  //
  // Every default below is the pre-F4 behaviour, so a spec that names none of
  // them reproduces the pinned run byte-for-byte.
  UnpricedLotPolicy unpriced{UnpricedLotPolicy::Error};        // spec key `unpriced`
  ScheduleFillPolicy fill_policy{ScheduleFillPolicy::ModelMark}; // `fill_policy`
  bool book_entry_fill_slippage{false};                        // `book_entry_fill_slippage`
  bool reconcile_nav{false};                                   // `reconcile_nav`
  // F6 quote-quality admission, consumed by `build-schedule`. Note its own
  // defaults are NOT the pre-F6 behaviour (a zero bid is now rejected and a
  // 10-minute staleness bound applies) — that is the BT-P2-8 fix, and the spec
  // line `quote_max_age_ns	0` restores the old contract. (FIX-F M1: this
  // named a key that does not exist, `quote_max_quote_age_ns`. It is the one
  // line an operator reads under re-pin pressure; see dispersion_run.cpp's
  // binder, which is the authority.)
  ListedQuoteQualityConfig quote_quality{};
  // S3-T16. Emit the LOOSE RESULT TSVs alongside the run's real output. Spec key
  // `emit_tsv_diagnostics`, DEFAULT OFF.
  //
  // The economic result envelope is `run.atxrun` (RunDir::write_run_archive),
  // which the Python reporting layer reads; the loose per-stage result tables
  // duplicate it in a human-readable form. They are a DIAGNOSTIC, not a
  // contract, and a default run leaves none of them behind.
  //
  // EXACTLY WHAT IT GATES (re-derived at S3-T17; four write sites, all on the
  // two library entry points that still exist, plus one CLI site):
  //   dispersion_run_surface_backtest -> `surface_backtest.tsv`, the X5 pair
  //       (`surface_tearsheet.tsv` + `surface_pnl_track.tsv`),
  //       `backtest_profile.tsv` (ATX_VOL_PROFILE), `backtest_counters.tsv`
  //       (ATX_VOL_COUNTERS)
  //   dispersion_run_projected_var    -> `projected_var.tsv`,
  //       `projected_risk_scenarios.tsv`, `projected_risk_legs.tsv`
  //   `verify_command`                -> `reference_reconciliation.tsv`, the M1
  //       schedule-only reference reconciliation sidecar
  //
  // What it NO LONGER gates, because S3-T17 deleted the library twins that were
  // the gated writers: `backtest.tsv`, `contract_marks.tsv` and
  // `reconciliation.tsv` are not written by any route now. `run_config.tsv` and
  // `quote_rejects.tsv` are written UNCONDITIONALLY by the shipped
  // `run_backtest_command` / `build_schedule_command`; that CLI-level
  // inconsistency is recorded, not fixed, by S3-T17 (the CLI's own TSV policy
  // was outside its byte-identity bar).
  //
  // What this flag does NOT govern: the run directory's retained TEXT INPUTS and
  // evidence — `run_spec.tsv`, `universe_schedule.tsv`, `definitions.tsv`,
  // `surface_manifest.tsv`, `quality.tsv`, `share_dividends.tsv`,
  // `input_inventory.tsv`, `methodology_map.tsv`, `occ_ess_inventory.tsv` and
  // `trade_schedule.tsv`. Later stages parse those back, `RunDir::run_identity_
  // hash` folds five of them, and the shipped `verify` requires them; they are
  // pipeline structure, not a report about it.
  //
  // Sits at the end of the struct because that is where it was added, NOT for
  // positional-initializer compatibility: the convention that once justified
  // that (see backtest.hpp) was retired in S4-T19 / plan item 4.2. This config
  // is bound BY KEY (`read_dispersion_run_config`) and has no aggregate-init
  // site at all, so it needed no migration then and needs no parking spot now.
  bool emit_tsv_diagnostics{false};
};

// One point-in-time observation of a discrete dividend schedule carried by an
// OPRA panel into the fitted corpus. Observations (rather than only deduplicated
// events) are persisted so the economic schedule retains its source/as-of and
// panel fingerprints. Replay validates stable amounts and reduces observations
// to FinancingConfig::share_dividends.
struct ShareDividendObservation {
  std::string observed_date{};
  std::string symbol{};
  std::uint32_t uid{0};
  std::int64_t ex_ts_ns{0};
  double amount{0.0};
  std::string source{};
  std::string as_of{};
  std::uint64_t source_fingerprint{0};
  std::uint64_t market_input_fingerprint{0};
};

[[nodiscard]] Result<CorpusMarketInputTable>
read_corpus_dividend_inputs(const std::filesystem::path &path);
[[nodiscard]] Status write_share_dividend_artifact(
    const std::filesystem::path &path,
    std::span<const ShareDividendObservation> observations);
[[nodiscard]] Result<std::vector<ShareDividend>>
read_share_dividend_artifact(const std::filesystem::path &path);

// F4: assemble the ENGINE run config the listed replay actually runs under.
// This is the single place the typed spec becomes engine behaviour, so a knob
// that is set here is provably reachable and one that is not is provably dead.
//
// REV-TAIL I-3 (2026-07-25): that claim was ASPIRATIONAL until this date. There
// were two places. `dispersion_backtest_config_from` — the builder the shipped
// `run-surface-backtest` goes through — hand-built its own `backtest.run`, set
// frictions/financing, hardcoded `unpriced = Error` over the spec's value, and
// never set `surface_provenance_policy` / `book_entry_fill_slippage` /
// `reconcile_nav` at all. Since that route reads the STRICT typed config, those
// four keys bound by name, passed `reject_unknown()`, and then did nothing.
// `dispersion_backtest_config_from` now delegates HERE, so the sentence above is
// literally true and the surface route cannot silently drift from the listed one.
// `DispersionBacktestConfigFrom.AgreesWithTheEngineRunConfigBuilderOnEveryKnob`
// is the guard that keeps it true.
[[nodiscard]] RunConfig dispersion_engine_run_config_from(const DispersionRunConfig &config);

// F5 (BT-T2) + FIX-F N2: the ONE construction of the engine RunConfig the listed
// `run-backtest` replay runs under — the F4 typed-spec config above PLUS the
// snapshot cache subsetted to the schedule's referenced uids, which the replay
// and the reconciliation pass share.
//
// It is a named function rather than four lines inside `run_backtest_command`
// because the first guard written for F5 rebuilt the construction inside the
// test, under the comment "verbatim the construction the listed replay
// performs". A comment cannot fail: reverting the production subsetting left the
// whole suite green, which is the same blind spot that let F5 ship inert on this
// path to begin with. The guard now calls THIS function, so "verbatim" becomes
// "the same code" and reverting the subsetting turns the guard red.
[[nodiscard]] RunConfig make_listed_replay_run_config(const DispersionRunConfig &config,
                                                      const Clock &clock,
                                                      const ListedDispersionStrategy &strategy);

// REV-MTIDY I-1: the ONE construction of the `ListedScheduleSpec` the shipped
// `build-schedule` hands `build_listed_dispersion_schedule`.
//
// It exists for the reason `make_listed_replay_run_config` above exists, except
// that here the reason is a MEASURED FACT rather than an argument. REV-FIXTAIL
// I-A closed "the three `quote_*` keys are published as EFFECTIVE and reach no
// shipped selection" with a single assignment in the example's `main` —
// `sched_spec.quality = run_config.quote_quality`. Deleting that assignment and
// running the whole gate produced 2262/2262 passed, 0 failed: byte-identical to
// the run with it. The two gtests written for the fix call
// `listed_selection_config_from`, one layer BELOW the assignment, and the e2e
// CLI chain drives only DEFAULT values, which equal the pre-fix behaviour by
// construction. So the repair for a knob that parsed and did nothing was itself
// protected only by a comment, and could have gone inert again under a green
// gate. That is the whole reason this function is a function.
//
// `DispersionScheduleSpecFrom.*` in dispersion_run_test.cpp calls THIS, so
// deleting the `quality` assignment in the body turns a test red.
//
// The two arguments are the two reads of the SAME `run_spec.tsv`: the loose
// `RunSpec` carries the eight swept schedule knobs, and the strict
// `DispersionRunConfig` carries F6's quote-quality admission, which `RunSpec`
// has no field for. `build_schedule_command` — since S3-T17 the ONLY schedule
// route — builds its selection policy through this, so the "single construction
// point" claim on `listed_selection_config_from` describes the shipped route
// itself and not a twin beside it.
[[nodiscard]] ListedScheduleSpec listed_schedule_spec_from(const RunSpec &spec,
                                                           const DispersionRunConfig &config);

// F4: emit `run_config.tsv` — the EFFECTIVE value of every execution knob the
// run actually used, REGIME FIRST (M4's reporting contract). A published NAV
// that does not say which frictions produced it is not a result, and until F4
// the listed route emitted no such record at all. Key/value TSV in the same
// key vocabulary as the spec; it is a RECORD of the run, not a re-runnable
// spec (it deliberately omits the corpus/date/path keys).
//
// "EFFECTIVE" IS A LOAD-BEARING WORD (REV-FIXTAIL I-A). Every key emitted here
// must be one some stage of the run directory actually honoured. It is written by
// `run-backtest`, but its scope is the RUN, so a key applied at an earlier stage
// of the same run directory belongs here — the three `quote_*` keys apply at
// `build-schedule` and are honoured on BOTH routes since I-A. A key that no stage
// reads does NOT belong here: publishing one as effective is strictly worse than
// ignoring it, because a reader can act on it. Before adding a row, name the code
// that consumes the value.
//
// TWO RESIDUALS ON THAT RULE, stated rather than left to be rediscovered.
//
// (1) REV-MTIDY M-4 — the claim now spans two CLI invocations. Every other key
// here is read and applied by the SAME invocation that publishes it. The three
// `quote_*` keys are not: they are applied by `build-schedule` and published by
// a later, separate `run-backtest` process from its own re-read of the same
// `run_spec.tsv`. Nothing binds the two, so the row is true only if the spec was
// unchanged between them. Narrow — a run directory is not normally edited
// mid-pipeline — but it is a real weakness in exactly the claim I-A exists to
// make true, and closing it would mean carrying the applied values forward in
// the run archive rather than re-deriving them from the spec.
//
// (2) REV-MTIDY M-5 — "name the code that consumes the value" does not
// distinguish CONSUMED from CONSUMED AND ABLE TO FIRE. `quote_max_age_ns` has a
// real consumer (`listed_dispersion.cpp`'s staleness gate) and is nonetheless
// structurally inert on the only production quote source: the OPRA panel is
// snapshot-stamped, so every quote's age is exactly 0. That is documented in
// full at `ListedQuoteQualityConfig::max_quote_age_ns`
// (listed_dispersion.hpp:74-93) and the tally reports it as unmeasurable rather
// than as a reassuring zero, so no number here is wrong. Read the rule as its
// stronger form: name the code that consumes the value, AND say so if that code
// cannot fire on the feed the run actually used.
[[nodiscard]] Status write_dispersion_effective_config(const std::filesystem::path &path,
                                                       const DispersionRunConfig &config);

// F4: carry every key the RunSpec writer does not emit from `source_spec` into
// the already-written `run_spec` in the run directory.
//
// `write_resolved_spec` only knows the RunSpec vocabulary, so build-corpus used
// to DROP every typed knob (frictions, financing, costs, limits, provenance,
// and F4/F6's own keys) when it rewrote the run dir's spec. Wiring a reader is
// necessary but not sufficient — the declared value has to survive the trip, or
// the knob is still unreachable in practice. Path-valued extras are rewritten
// absolute so they still resolve from the run directory.
[[nodiscard]] Status persist_typed_spec_keys(const std::filesystem::path &source_spec,
                                             const std::filesystem::path &run_spec);

// One row of `quote_rejects.tsv`.
struct QuoteRejectRow {
  std::string date{};
  // FIX-F m4. TRUE: selection produced a basket and `counts` describes the
  // expiry it CHOSE. FALSE: selection failed on this date and `counts`
  // describes the FIRST candidate expiry it inspected — all zeros if it failed
  // before inspecting any. The distinction is a column in the artifact rather
  // than an omission, because a row that silently means two different things is
  // worse than no row.
  bool selected{true};
  ListedQuoteRejectCounts counts{};
};

// F6: emit `quote_rejects.tsv` — the per-date quote-admission tally that
// `build-schedule` accumulated, one row per date selection RAN on, succeeded or
// not. Dates absent from the file held an unexpired cohort, so no roll was
// attempted and no quote was inspected.
//
// A counter that exists only in memory cannot answer "why did this schedule
// change" after the fact, which is precisely the question a moved golden asks.
//
// SCHEMA. The file leads with `# schema=quote_rejects/<version>` (FIX-F m5) —
// the `#`-metadata convention `write_backtest_pnl_tsv` already uses — so a
// positional reader can fail loudly instead of silently shifting when a column
// is inserted. Version 1 is:
//   date, selection, not_two_sided, zero_bid, stale, stale_unevaluable,
//   locked, locked_dropped, non_standard, total_dropped
// `locked` counts every locked market SEEN whether or not the policy dropped it,
// so `total_dropped` excludes it and counts `locked_dropped` instead;
// `stale_unevaluable` is a measurability report, not a rejection, and is
// excluded outright.
[[nodiscard]] Status write_quote_reject_report(const std::filesystem::path &path,
                                               std::span<const QuoteRejectRow> rows);

// Classify a run config's execution assumptions. Purely a function of the
// frictions + cost model that actually reach the engine.
[[nodiscard]] DispersionFrictionRegime
dispersion_friction_regime(const DispersionRunConfig &config) noexcept;

// Two series of EQUAL length, paired observation for observation.
struct DispersionBenchmarkPairing {
  std::vector<double> strategy;
  std::vector<double> benchmark;
  // Inner join only: strategy observations with no benchmark row. Always 0 under
  // ExactDates, which refuses rather than dropping.
  std::size_t n_unmatched{0};
};

// Join `benchmark` onto the strategy's dated returns under `policy`.
//
// Errors (InvalidArgument), never silent truncation: mismatched strategy
// date/value counts, non-ascending strategy dates, a length or date disagreement
// under `ExactDates`, or fewer than two paired observations under either policy —
// every ratio in `BenchmarkStats` needs a sample variance, so a one-observation
// join is not a benchmark comparison and must not be reported as one.
[[nodiscard]] Result<DispersionBenchmarkPairing>
pair_dispersion_benchmark(std::span<const std::string> strategy_dates,
                          std::span<const double> strategy_pnl,
                          std::span<const DispersionBenchmarkRow> benchmark,
                          DispersionBenchmarkJoin policy);

// Read a `date<TAB>pnl` benchmark series as DATED rows, in file order. Header row
// optional (a first row whose second field does not parse as a number is treated
// as a header). `NotFound` if the file is absent, `ParseError` on a malformed
// row — a benchmark that silently half-loads would corrupt every statistic
// derived from it.
//
// C-6 validation, all `ParseError`: the date field must be non-empty, the P&L
// must be FINITE (a NaN or an infinity used to flow straight into the published
// alpha/beta), and the dates must be STRICTLY INCREASING — which is what rejects
// duplicates, reversed files and unordered files at the source rather than
// letting them become a plausible-looking misalignment downstream.
[[nodiscard]] Result<std::vector<DispersionBenchmarkRow>>
read_dispersion_benchmark_series(const std::filesystem::path &path);

// THE one place a spec's `benchmark_series` becomes a benchmark-relative
// tearsheet block. Reads the file, derives the strategy's own return dates
// (`backtest_return_dates`), joins under `config.benchmark_join`, and folds the
// paired series with `benchmark_stats`. An empty `benchmark_series` returns
// exactly `tearsheet(track, config.periods_per_year)` and claims nothing.
[[nodiscard]] Result<TearSheet>
dispersion_tearsheet_with_benchmark(const BacktestResult &track,
                                    const DispersionRunConfig &config);

// Named friction presets selectable from the spec via `friction_preset`. `None`
// is the default and is exactly the frictionless golden; `RetailListedOptions`
// is a documented realistic setting (see the constant's definition) that a spec
// opts into. A preset is applied FIRST, then any explicit friction_* key
// overrides the corresponding field, so a spec can start from a preset and tune.
enum class DispersionFrictionPreset : std::uint8_t { None = 0, RetailListedOptions = 1 };

[[nodiscard]] FrictionModel dispersion_friction_preset(DispersionFrictionPreset preset);

// STRICT key/value TSV deserialization. Unknown keys, duplicate keys, malformed
// numbers, and out-of-contract combinations are all errors, and the message
// NAMES the offending key. Relative paths resolve against `path`'s directory.
[[nodiscard]] Result<DispersionRunConfig>
read_dispersion_run_config(const std::filesystem::path &path);

// Assemble the surface-only backtest config from the typed run config. This is
// where X2 frictions/financing, X3 limits, X6 costs and the previously-hardcoded
// multiplier actually reach the engine.
//
// Its `run` member is built by `dispersion_engine_run_config_from` (above) rather
// than hand-assembled, so the surface route and the listed route honour the SAME
// set of engine knobs. See REV-TAIL I-3 at that declaration for what this cost
// before it was true.
[[nodiscard]] DispersionBacktestConfig
dispersion_backtest_config_from(const DispersionRunConfig &config);

// ── X5: tearsheet emission ──────────────────────────────────────────────────
//
// The surface path emitted raw SoA and never produced headline statistics. These
// two writers add them WITHOUT touching `surface_backtest.tsv`: both are new
// files, so the pinned artifact is byte-for-byte unchanged and the reproducibility
// pin is measured on exactly the bytes it always was.
//
//   surface_tearsheet.tsv   `metric<TAB>value` table (%.10g), regime block FIRST
//   surface_pnl_track.tsv   the self-describing `# key=value` + series TSV the
//                           Python renderer consumes (write_backtest_pnl_tsv
//                           layout, matching examples/spy_dispersion_pnl.cpp)
//
// THE REGIME IS NOT OPTIONAL METADATA. Both files lead with `friction_regime`
// and `friction_detail`, and the renderer refuses a track that carries neither —
// see `DispersionFrictionRegime` for why a bare headline number is misleading.

// Ordered `# key=value` metadata describing the run's identity, its friction /
// impact regime, its X4 policies, and its headline + benchmark-relative stats.
// The regime keys are emitted FIRST so a truncated read still carries them.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
dispersion_report_metadata(const DispersionRunConfig &config, const TearSheet &sheet,
                           std::size_t n_sessions);

// Write both reporting artifacts into `run_dir`. Never writes
// `surface_backtest.tsv`.
[[nodiscard]] Status write_dispersion_tearsheet(const std::filesystem::path &run_dir,
                                                const DispersionRunConfig &config,
                                                const DispersionBacktestOutcome &outcome);

// ── Native reference reconciliation (M1) ────────────────────────────────────
//
// Independent arithmetic verifier for a persisted listed-dispersion run, ported
// from tools/reference_spy_dispersion.py. Re-derives the vega-flat schedule
// quantities and the model/quote P&L from the persisted TSV artifacts and
// numerically compares them against the recorded values (no market data needed).
struct ReferenceReconRecord {
  std::string record_type{}; // "roll" | "date"
  std::string date{};
  std::int64_t cohort{0};
  // Roll rows carry vega residuals; date rows carry P&L/NAV. Unused fields are
  // left NaN and serialize as "NA".
  double computed_net_vega{0.0};
  double computed_gross_vega{0.0};
  double relative_vega_residual{0.0};
  double computed_model_option_pnl{0.0};
  double computed_quote_mid_pnl{0.0};
  double computed_model_nav{0.0};
  double computed_quote_mid_nav{0.0};
  double quote_mid_coverage{0.0};
  bool is_roll{true};
};

// Recompute + numerically verify the persisted artifacts. `schedule_only` limits
// the check to trade_schedule.tsv (matches the Python tool's flag).
[[nodiscard]] Result<std::vector<ReferenceReconRecord>>
reconcile_dispersion_reference(const std::filesystem::path &run_dir, bool schedule_only = false);

// Serialize reference records to the canonical reference_reconciliation.tsv layout.
[[nodiscard]] Status write_reference_reconciliation_file(
    const std::filesystem::path &path, std::span<const ReferenceReconRecord> records);

// ── Corpus phase-split line (B1 / T1) ───────────────────────────────────────
//
// The single line `dispersion_build_corpus` prints under
// `ATX_VOL_CORPUS_PHASE_TIMING`. Factored out as a PURE function of its inputs
// so its contents can be gated directly, without driving an 82-date corpus
// build to find out what the probe would have reported: the caller prints
// exactly this string and nothing else.
//
// `other_s` is build-phase wall NOT attributed to a named phase (board
// construction from the OPRA panels, manifest/quality assembly, the session
// bookkeeping between dates). Printed rather than hidden so the parts sum to the
// whole and a large residual stays visible.
//
// The line's twelve fields and the two rules that govern them — NEW FIELDS
// APPEND, and every field is `name=value` — are documented in full at the
// definition (`src/dispersion_run.cpp`) and gated by `CorpusPhaseLine.
// FieldLayoutIsAppendOnlyAndEveryFieldIsSelfDescribing`. Out-of-tree operator
// scripts scrape this line positionally, so neither rule is cosmetic.
[[nodiscard]] std::string format_corpus_phase_line(double ingest_s, double build_s,
                                                   const CorpusPhaseTimings &phases,
                                                   std::size_t date_batch,
                                                   double ingest_process_cpu_s = 0.0);

// ── File-oriented workflow entry points ─────────────────────────────────────
//
// WHO CALLS THESE (S3-T17, 2026-07-29, plan item 3.7). Read the contract at the
// top of this header first; this is the per-entry-point half of it. There are
// now THREE file-oriented entry points, and every one of them backs a shipped
// subcommand:
//
//   dispersion_build_corpus         <- `spy_dispersion_backtest build-corpus`
//   dispersion_run_surface_backtest <- `spy_dispersion_backtest run-surface-backtest`
//   dispersion_run_projected_var    <- `spy_dispersion_backtest run-projected-var`
//
// The seam is FULLY DISPATCHED. No library-only reserve is left among these three
// WORKFLOW entry points, and no shipped subcommand carries a second implementation
// of a body declared here. The long "deliberate reserve" argument that used to
// stand here is gone with the code it defended; what follows is why, so it is not
// reconstructed. (The scope of that claim is the three workflow entries. The
// projected-VaR ENVELOPE GATE declared at the bottom of this header is a different
// thing and does not contradict it: it is a checkable predicate, not an
// orchestration, and its own REACH note states outright that it has no shipped
// caller since S3-T17 deleted `dispersion_verify`.)
//
// WHAT WAS DELETED. `dispersion_build_schedule`, `dispersion_run_backtest` and
// `dispersion_verify`. Re-derived from the tree rather than inherited:
//
//   * THE CLI WAS ALREADY THE COLLAPSED FORM. `build_schedule_command` composes
//     `listed_schedule_spec_from` + `build_listed_dispersion_schedule_audited`
//     (the library's own selection seam, with a `ListedQuoteRejectSink`) and
//     publishes through `RunDir::write_run_archive`; `run_backtest_command`
//     composes `read_dispersion_run_config` + `make_listed_replay_run_config` +
//     `reconcile_listed_schedule` and publishes the same way. The deleted twins
//     were SECOND implementations of those two orchestrations — not the seams
//     the CLI sits on. Deleting them is what makes it one implementation per
//     concern; dispatching into them would have been the reverse.
//   * THEY HAD DIVERGED, AND THIS BLOCK SAID OTHERWISE. It asserted the two
//     run-backtest bodies "differ ONLY in how they persist results". They did
//     not. The twin fed its FULL timeline straight to
//     `reconcile_listed_dispersion`; the CLI calls `reconcile_listed_schedule`,
//     which TRIMS the warm-up lead-in first and only then reconciles. Those are
//     different reconciliations, not two spellings of one: the untrimmed route
//     emits a flat, position-free row for every pre-entry date while the trimmed
//     route emits none, and `validate_listed_reconciliation_backtest` hard-requires
//     equal row counts (the still-open caution on
//     `assemble_reconciliation_snapshots`, listed_dispersion_pipeline.hpp). NB the
//     PREMISE this bullet used to argue from is itself now stale and is corrected
//     here rather than repeated: `reconcile_listed_dispersion` no longer
//     hard-requires the front session to BE the first roll date. Change C2 made it
//     tolerate a leading pre-roll session, emitting a flat row for it; the
//     surviving hard error is a timeline with NO snapshot on/after the first
//     scheduled roll date. Pinned by
//     `ListedDispersionPipeline.ReconcileClockCoupling_ToleratesWarmupLeadInButNotAMissingEntry`.
//     The divergence conclusion survives the correction; its reason is the trim,
//     not a precondition. The twin also joined the whole OPRA panel, where the CLI
//     narrows the join to the schedule's leg keys — a narrowing that also narrows
//     six of that join's seven fatal exits. That `wanted` contract is documented on
//     `listed_quotes_from_opra` in listed_opra.hpp, which is what
//     `listed_quotes_for_date` (listed_dispersion_pipeline.hpp) forwards it to.
//     Dispatching the CLI into the twin would have moved shipped ERROR PATHS, so
//     "collapse onto the twin" was never available.
//   * `dispersion_verify` VERIFIED AN ENVELOPE NO ROUTE PRODUCES. It required
//     the loose `backtest.tsv` / `contract_marks.tsv` / `reconciliation.tsv`
//     that the RunArchive cutover replaced. `RunDir::verify()` is the
//     archive-native gate, and the shipped `verify_command` already calls it.
//     Dispatching into the twin would have REGRESSED verify, not collapsed it.
//
// WHY v1 OFFERS NO LIBRARY-LEVEL LISTED-REPLAY ENTRY POINT. The two
// orchestration concerns the CLI owns are CLI inputs, not run-directory inputs:
// the `--cache` / `ATX_VOL_CACHE` pre-parsed definitions cache, and the
// `PhaseTimer` whose phases become the `diagnostics` archive section and the
// `diag` stderr line. An entry point taking only a run dir cannot carry either,
// so any such twin is a LOSSY copy of the shipped body by construction — which
// is exactly how the deleted pair drifted. `spy_dispersion_backtest` is the
// single listed-replay orchestration; every step inside it is a named library
// seam with its own tests.
//
// CONSEQUENCE FOR THE LOOSE RESULT TABLES, stated so it is not rediscovered as a
// defect: no SHIPPED ROUTE writes `backtest.tsv`, `contract_marks.tsv` or
// `reconciliation.tsv` into a run directory any more — the deleted twin was
// their last writer, and the shipped `run-backtest` has published its economics
// as `run.atxrun` sections since the cutover. In a directory this pipeline
// produced they therefore survive only from before that cutover. The qualifier is
// load-bearing and is not a hedge: the gtests DO still write all three, into
// synthetic run dirs they build themselves, and that is exactly what "off
// synthetic input" means below — a reader who takes the unqualified "nothing
// writes them" literally will read those fixtures as a contradiction.
// `reconcile_dispersion_reference` (declared above) reaches a shipped binary in
// its SCHEDULE-ONLY mode only, from `verify_command` under the run spec's
// `emit_tsv_diagnostics`; its full mode is covered by
// `DispersionReferenceReconcile.*` off synthetic input and by
// `DispersionReferenceReconcileRealData` off the aging published corpus.
//
// F6's quote-quality admission (`DispersionRunConfig::quote_quality` ->
// `select_listed_dispersion`) reaches selection through `listed_schedule_spec_
// from` -> `listed_selection_config_from`, which the shipped `build-schedule`
// composes — so the declared policy governs the shipped route and
// `write_dispersion_effective_config`'s `run_config.tsv` echo is true of the run
// that produced it. GATED by `DispersionScheduleSpecFrom.*`, and it was NOT
// gated when it was first called closed: the link was one assignment in the
// example's `main`, and deleting that assignment left the whole label gate green
// — 2262/2262 passed, 0 failed, 2262 being the gate's total AT THAT TIME and not
// a figure to compare against today's. Read it as the rule this sprint keeps
// re-learning — a fix whose load-bearing line no test executes for a NON-DEFAULT
// value is not closed, it is asserted.
//
// WHAT REMAINS: the per-date `quote_rejects.tsv` admission tally is written by
// the shipped `build_schedule_command` off the `ListedQuoteRejectSink` the
// audited builder drives — unconditionally, and BEFORE a failed acceptance
// propagates, since a rejected selection is when the counts are worth most. Since
// `build-schedule` is now the only route that builds a schedule at all, that is
// the whole story: on it the policy applies AND its per-date accounting is
// published. No shipped code parses the file back — it is a report ABOUT the run,
// not an input to it — and the only reader anywhere is
// `DispersionRunConfig`'s test of the `# schema=quote_rejects/1` header line,
// which is a pin on the format, not a consumer of the data.

[[nodiscard]] Status dispersion_build_corpus(const std::filesystem::path &source_spec_path,
                                             const std::filesystem::path &run_dir,
                                             const DispersionCorpusPolicy &policy = {});
[[nodiscard]] Status dispersion_run_surface_backtest(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_projected_var(const std::filesystem::path &run_dir);

// Envelope gate for the OPTIONAL projected-VaR stage. Absent artifacts are fine
// (the stage need not have run); a PRESENT summary must have its two companions,
// the contract header, and a scenario count matching the run's session count —
// so a truncated or stale projected-VaR run cannot pass silently. `n_sessions ==
// 0` skips the count check. Exposed separately so it is testable without a full
// run dir.
//
// REACH (S3-T17): its only production caller was `dispersion_verify`, which is
// deleted, so this is now a library API exercised by `DispersionProjectedVarGate
// .*` and `DispersionProjectedVar.*` and by no shipped binary. Stated rather
// than implied — the shipped `verify` gates the ARCHIVE envelope through
// `RunDir::verify()`, which does not fold this check in. Wiring it there is a
// behaviour change and belongs to whoever owns the verify contract, not to the
// task that deleted its old caller.
[[nodiscard]] Status verify_projected_var_artifacts(const std::filesystem::path &run_dir,
                                                    std::size_t n_sessions);

} // namespace atx::vol
