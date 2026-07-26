#pragma once

// Library seam for the traditional SPY listed-options dispersion proxy.
//
// The command bodies that used to live inside examples/spy_dispersion_backtest.cpp
// (~620 LOC of library workflow trapped in an example main) live here so the
// example is thin CLI glue and every stage is unit-testable off the filesystem.
//
// SEAM CONTRACT (RECONCILE 1, 2026-07-25) — READ THIS BEFORE DELETING ANYTHING.
// This is a PARTIALLY-DISPATCHED seam, on purpose, and the split is stated in full
// at the "File-oriented workflow entry points" block below.
//
// Background: the main -> feat/pipeline-m merge took main's version of the example
// wholesale, because main had hard-cut the CLI over to the `run.atxrun` RunArchive
// result container and the shared Python layer is written against that container.
// The side effect was that NO shipped binary called into this header, leaving a
// tested library with no production caller — the shape that rots quietly and is
// then deleted by someone who assumes it was always dead. It was not, and this
// note exists so nobody has to reconstruct that.
//
// The resolution is a UNION where the two designs are compatible, not a blanket
// restore and not a blanket retreat to library-only:
//
//   * Where BOTH designs write only loose TSVs and neither touches `run.atxrun`
//     (build-corpus, run-surface-backtest, run-projected-var), the CLI dispatches
//     here. Those three are exact supersets of the bodies they replaced, so the
//     union costs nothing and recovers X1/X2/X3/X4/X5/X6, C1-ACTIVATE and F4's
//     `persist_typed_spec_keys`.
//   * Where the two designs disagree about the RESULT ENVELOPE — the library
//     writes loose result TSVs and main's design uses `run.atxrun` (build-schedule,
//     run-backtest, verify) — the CLI keeps its own body, because dispatching here
//     would write files the reporting layer cannot read. It still takes the CONFIG
//     CONSTRUCTION from here, so F4/F5 stay reachable from the command line.
//     (REV-TAIL M-1: this rule used to read "where main's design PUBLISHES ARCHIVE
//     SECTIONS". That is wrong for `verify`, which publishes nothing — it READS
//     (`RunDir(run_dir).verify()`), and it is library-only because its twin reads
//     LOOSE artifacts the cutover no longer writes. It also omitted
//     `project-schedule` and `run-projected-backtest`, which do publish sections
//     and are CLI-bodied. The membership list was right; the rule was not.)
//
// Consequence for readers of the tests: WHERE `dispersion_run_test.cpp` covers a
// DISPATCHED entry point, that coverage is also CLI coverage — but today it
// covers none of the three directly (measured at this revision: 0 calls to
// `dispersion_build_corpus`, 0 to `dispersion_run_projected_var`, and for
// `dispersion_run_surface_backtest` a single mention in a comment). REV-FIXTAIL
// Minor 4: the previous wording stated the implication as though the coverage
// existed, which is the same class of claim REV-TAIL I-1 deleted below, surviving
// in a neighbouring sentence. For the other three — the library-only ones, which
// that file DOES drive — the coverage is not coverage of a CLI path and, until
// REV-TAIL I-1, was not coverage at all; see the per-entry-point block further
// down for exactly what now backs them.
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
#include "atx/vol/dispersion_backtest.hpp"// DispersionBacktestConfig, run_dispersion_backtest
#include "atx/vol/dispersion_workflow.hpp"// RunSpec
#include "atx/vol/listed_dispersion.hpp"  // ListedQuoteQualityConfig (F6)
#include "atx/vol/listed_dispersion_pipeline.hpp" // ListedScheduleSpec (REV-MTIDY I-1)
#include "atx/vol/listed_dispersion_strategy.hpp" // ScheduleFillPolicy (F2)
#include "atx/vol/session.hpp"            // FitPreset
#include "atx/vol/tearsheet.hpp"          // TearSheet
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
};

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
// It is a named function rather than four lines inside `dispersion_run_backtest`
// because the first guard written for F5 rebuilt the construction inside the
// test, under the comment "verbatim the construction dispersion_run_backtest
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
// has no field for. Both `build_schedule_command` (the shipped route) and
// `dispersion_build_schedule` (the library twin) build their selection policy
// through this, so the "single construction point" claim on
// `listed_selection_config_from` is no longer route-local.
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

// Read a `date<TAB>pnl` benchmark series, returning the P&L column in file
// order. Header row optional (a first row whose second field does not parse as a
// number is treated as a header). `NotFound` if the file is absent, `ParseError`
// on a malformed row — a benchmark that silently half-loads would corrupt every
// statistic derived from it.
[[nodiscard]] Result<std::vector<double>>
read_dispersion_benchmark_series(const std::filesystem::path &path);

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

// Legacy view of the typed config for the corpus/batch plumbing that still
// speaks `RunSpec`. This is the ONE place the two representations meet.
[[nodiscard]] RunSpec run_spec_from(const DispersionRunConfig &config);

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

struct DispersionVerifyReport {
  std::size_t n_dates{0};
  std::uint32_t n_admitted{0};
  std::size_t n_rolls{0};
};

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
                                                   std::size_t date_batch);

// ── File-oriented workflow entry points ─────────────────────────────────────
//
// WHO CALLS THESE (RECONCILE 1, 2026-07-25). Read the contract at the top of this
// header first; this is the per-entry-point half of it. There are SIX file-oriented
// entry points in this block (the five declared immediately below plus
// `dispersion_verify`). Three DO back a shipped subcommand and three DELIBERATELY
// do not, and the difference is not an oversight in either direction.
// (REV-TAIL M-1: this said "three of the five", miscounting both halves.)
//
//   dispersion_build_corpus         <- `spy_dispersion_backtest build-corpus`
//   dispersion_run_surface_backtest <- `spy_dispersion_backtest run-surface-backtest`
//   dispersion_run_projected_var    <- `spy_dispersion_backtest run-projected-var`
//
// dispersion_build_schedule / dispersion_run_backtest — LIBRARY ONLY, and the
// shipped subcommands of the same name do NOT call them. Both write the loose
// `backtest.tsv` / `reconciliation.tsv` / `contract_marks.tsv` / `trade_schedule
// .tsv` result files that the RunArchive cutover REPLACED with `run.atxrun`
// sections; the Python reporting layer (`atxvol.report`, and the e2e module that
// executes this binary and reads `run.atxrun` back) is written against the
// archive. Dispatching them would publish a run directory the reporting layer
// cannot read, so the CLI keeps its own archive-publishing bodies.
//
// What the CLI DOES take from `dispersion_run_backtest` is its CONFIG
// CONSTRUCTION, which is the part that carries F4/F5: `run-backtest` calls
// `read_dispersion_run_config` + `make_listed_replay_run_config` +
// `write_dispersion_effective_config` exactly as this function does, so the typed
// spec governs the shipped listed replay and `run_config.tsv` is emitted on it.
// The two bodies therefore differ ONLY in how they persist results.
//
// F6's quote-quality admission (`DispersionRunConfig::quote_quality` ->
// `select_listed_dispersion`) USED TO reach selection only through
// `dispersion_build_schedule`, i.e. only on the library-only route. That was worse
// than a silently-ignored knob, because `write_dispersion_effective_config` echoed
// all three `quote_*` keys into `run_config.tsv` — an artifact this header
// documents as the EFFECTIVE value of every knob the run used — so a reader
// reconciling a shipped run against that record was told a filter applied that
// never ran. CLOSED (REV-FIXTAIL I-A): `ListedScheduleSpec` gained a `quality`
// member and `build_listed_dispersion_schedule` builds its selection config
// through the named `listed_selection_config_from`, so the shipped `build-schedule`
// honours the declared policy and the effective-config record is true on both
// routes. Golden-neutral by construction — the new default IS what the loop
// default-constructed (`ListedDispersionPipeline.DefaultScheduleSpecKeepsTheShipped
// SelectionDefaults`).
//
// GATED since REV-MTIDY I-1, and it was NOT gated when it was first called
// closed. The link between the strict typed config and the shipped selection was
// one assignment in the example's `main`; deleting it left the whole label gate
// at 2262/2262 passed, 0 failed. It is now `listed_schedule_spec_from` (declared
// above), which `DispersionScheduleSpecFrom.*` calls, so the same deletion turns
// a test red. Read that as the general rule this sprint keeps re-learning: a fix
// whose load-bearing line no test executes for a NON-DEFAULT value is not closed,
// it is asserted.
//
// WHAT REMAINS, stated so it is not rediscovered as the same defect: only
// `dispersion_build_schedule` writes the per-date `quote_rejects.tsv` tally. The
// shipped `build-schedule` never wrote it — not before this fix and not after —
// so on that route the policy now applies but its per-date accounting is not
// published. That is a missing REPORT, not an unhonoured knob, and no artifact
// claims otherwise.
//
// `dispersion_verify` (below) is likewise LIBRARY ONLY for the same reason: it
// verifies the loose artifact envelope and folds in the M1 native reference
// reconciliation, both of which read result files the cutover no longer writes.
//
// WHAT ACTUALLY COVERS THE THREE LIBRARY-ONLY ENTRY POINTS (REV-TAIL I-1,
// 2026-07-25). This block used to claim that "each is covered directly off the
// filesystem by `dispersion_run_test.cpp`". THAT WAS FALSE: that file called none
// of the six, and `dispersion_build_schedule`, `dispersion_run_backtest` and
// `dispersion_verify` had zero callers and zero tests — so the instruction not to
// delete them rested on coverage that did not exist. `run_spec_from` had neither
// a caller nor a test either. The determination, re-derived from the tree rather
// than inherited: they are NOT accidentally-unwired code that the CLI seam work
// (`347ad44`) failed to reach, and they are NOT dead code to delete. They are a
// deliberate, documented RESERVE — the reason is the cutover argument above, and
// it holds. What they lacked was any evidence they still work.
//
// They now have a floor rather than a claim:
//   `DispersionLibraryOnlyEntryPoints.EachIsReachableAndFailsClosedNamingTheMissing
//   Spec` drives all three off `tmp_path` and pins that each is LINKED, REACHABLE
//   and FAILS CLOSED naming its missing input.
// That is deliberately modest — it gates reachability and fail-closed behaviour,
// NOT the economics. Anyone relying on these for economics must build the coverage
// first. State it as reserve; do not restate it as coverage.
//
// CONSEQUENCE FOR M10 (`5d5af01`), stated so the next reader is not misled the way
// this block previously misled: M10's fail-closed `str()` change is real and is
// directly tested off `tmp_path` (`DispersionReferenceReconcile.M10_*`), but it
// reaches NO SHIPPED BINARY. Its only public reach is `reconcile_dispersion_
// reference` -> `dispersion_verify`, and the shipped `verify` subcommand calls
// `RunDir(run_dir).verify()` instead (spy_dispersion_backtest.cpp:329). The MINORS
// report's claim that it protects a production path was true at `5d5af01` and is
// stale now.

[[nodiscard]] Status dispersion_build_corpus(const std::filesystem::path &source_spec_path,
                                             const std::filesystem::path &run_dir,
                                             const DispersionCorpusPolicy &policy = {});
[[nodiscard]] Status dispersion_build_schedule(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_backtest(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_surface_backtest(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_projected_var(const std::filesystem::path &run_dir);

// Envelope gate for the OPTIONAL projected-VaR stage, folded into
// `dispersion_verify`. Absent artifacts are fine (the stage need not have run);
// a PRESENT summary must have its two companions, the contract header, and a
// scenario count matching the run's session count — so a truncated or stale
// projected-VaR run can no longer pass verify silently. `n_sessions == 0` skips
// the count check. Exposed separately so it is testable without a full run dir.
[[nodiscard]] Status verify_projected_var_artifacts(const std::filesystem::path &run_dir,
                                                    std::size_t n_sessions);
// Verifies the artifact envelope AND folds in the native reference reconciliation
// (M1): it recomputes + numerically compares the persisted arithmetic and writes
// run_dir/reference_reconciliation.tsv.
[[nodiscard]] Result<DispersionVerifyReport>
dispersion_verify(const std::filesystem::path &run_dir);

} // namespace atx::vol
