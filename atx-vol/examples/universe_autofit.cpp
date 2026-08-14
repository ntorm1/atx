// universe_autofit.cpp — fit an entire options universe from one OPRA snapshot,
// letting the library AUTO-SELECT the curve family per board (PricerConfig::curve
// left unset => unified fit policy + CurveSelector), then report exactly what the
// pipeline did per symbol: routing decision, chosen family, fit outcome, quality
// diagnostics, valuation NaN rates, and wall-clock timings.
//
// This is the "vola.dynamic claim" stress harness: point it at a {symbol}/{date}
// parquet hive holding one snapshot minute for N thousand underliers and it
// answers (a) does the pipeline survive the whole US universe, (b) where does it
// fail, (c) where does the CPU go.
//
//   universe_autofit --opra-root DIR --date YYYY-MM-DD --symbols-file FILE
//       [--snapshot-suffix T14:00:00Z] [--r 0.043] [--preset robust]
//       [--fit-workers N] [--limit N] [--out results.csv] [--no-value]
//       [--oos-max-expiries N] [--selector-budget-ms N] [--sparse-floor N]
//       [--min-direct-confidence X] [--path-template "{symbol}/{date}.parquet"]
//       [--no-fit] [--fit-path production|legacy]
//       [--v2-fields none|quality|outputs|both] [--outputs risk|mark|both]
//       [--risk-admission required|na] [--fallback lkg|none]
//       [--publish-floor on|off] [--symbol-knobs on|off]
//       [--attempts-out attempts.csv] [--expiries-out expiries.csv]
//       [--slices-out slices.csv]
//
// Output CSV: one row per symbol with load/fit/value status + diagnostics.
// `--attempts-out` additionally writes a SECOND csv, one row per (symbol,
// build attempt), joinable to the first on `symbol` — the fallback ladder's
// per-rung rejection evidence, which cannot fit one row per board.
// `--expiries-out` writes a THIRD csv, one row per (symbol, build attempt,
// chain) — the fit driver's own per-expiry census, which is the only place a
// DROPPED expiry names its own cause (T3c).
// `--slices-out` writes a FOURTH csv, one row per (symbol, SERVED slice) —
// per-slice tenor, quote density and re-Americanized quality, which is the only
// place the board aggregates' composition can be decomposed (T3d).
// Summary to stdout: status counts, curve-family histogram, profile histogram,
// error-code breakdown, timing percentiles, slowest boards.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/chain.hpp"         // OptionChain
#include "atx/vol/corpus.hpp"        // CorpusBoard
#include "atx/vol/fit_policy.hpp"    // FitDecision
#include "atx/vol/opra_batch.hpp"    // OpraBatchSpec, load_opra_daterange, corpus_board_from_opra
#include "atx/vol/detail/parallel_for.hpp"  // parallel_for, atx_auto_worker_count
#include "atx/vol/pricer_fitter.hpp" // PricerFitter, PricerConfig, OutputField
#include "atx/vol/profile.hpp"       // ProfileKind
#include "atx/vol/session.hpp"       // FitPreset, SessionDiagnostics
#include "atx/vol/surface_db.hpp"    // SymbolFitConfig, symbol_config_from_preset
#include "atx/vol/surface_policy.hpp" // SurfaceHealth, ValidationDigest, SurfaceState
#include "atx/vol/tools/surface_db_populate.hpp" // populate_admission_policy
#include "atx/vol/types.hpp"         // Result
#include "atx/vol/vol_curve.hpp"     // VolCurveKind, to_string
#include "atx/vol/vol_surface.hpp"   // EssviParams, VolSurface::essvi_slices

using namespace atx::vol;
using SteadyClock = std::chrono::steady_clock;

namespace {

double ms_since(SteadyClock::time_point t0) {
  return std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
}

const char *profile_name(ProfileKind k) {
  switch (k) {
  case ProfileKind::IndexEtfUltraLiquid: return "IndexEtfUltraLiquid";
  case ProfileKind::MegaCapEvent: return "MegaCapEvent";
  case ProfileKind::LiquidSingleName: return "LiquidSingleName";
  case ProfileKind::OrdinarySingleName: return "OrdinarySingleName";
  case ProfileKind::IlliquidSmallCap: return "IlliquidSmallCap";
  case ProfileKind::HtbDividendName: return "HtbDividendName";
  case ProfileKind::VolProduct: return "VolProduct";
  }
  return "?";
}

const char *source_name(FitDecisionSource s) {
  switch (s) {
  case FitDecisionSource::ProfileOverride: return "ProfileOverride";
  case FitDecisionSource::TickerPrior: return "TickerPrior";
  case FitDecisionSource::BoardFeatures: return "BoardFeatures";
  case FitDecisionSource::SparseGuard: return "SparseGuard";
  case FitDecisionSource::CrossValidation: return "CrossValidation";
  }
  return "?";
}

const char *preset_name(FitPreset p) {
  switch (p) {
  case FitPreset::Fast: return "fast";
  case FitPreset::Accurate: return "accurate";
  case FitPreset::Robust: return "robust";
  case FitPreset::Hft: return "hft";
  case FitPreset::Populate: return "populate";
  case FitPreset::Bulk: return "bulk";
  }
  return "?";
}

// `SurfaceAdmissionReason`, `SurfaceBuildStage` and `ParityDiagnosticState` have
// no library `to_string`; naming them here follows the same example-local
// convention as `profile_name`/`source_name` above rather than growing the API.
const char *admission_reason_name(SurfaceAdmissionReason reason) {
  switch (reason) {
  case SurfaceAdmissionReason::None: return "none";
  case SurfaceAdmissionReason::BuildFailed: return "BuildFailed";
  case SurfaceAdmissionReason::InsufficientFittedExpiries: return "InsufficientFittedExpiries";
  case SurfaceAdmissionReason::InsufficientExpiryCoverage: return "InsufficientExpiryCoverage";
  case SurfaceAdmissionReason::InsufficientQuoteCoverage: return "InsufficientQuoteCoverage";
  case SurfaceAdmissionReason::FrontExpiryMissing: return "FrontExpiryMissing";
  case SurfaceAdmissionReason::ConsecutiveExpiryGap: return "ConsecutiveExpiryGap";
  case SurfaceAdmissionReason::NonFiniteDiagnostics: return "NonFiniteDiagnostics";
  case SurfaceAdmissionReason::CalendarArbitrage: return "CalendarArbitrage";
  case SurfaceAdmissionReason::QualityBelowFloor: return "QualityBelowFloor";
  case SurfaceAdmissionReason::ImpossibleEvidence: return "ImpossibleEvidence";
  case SurfaceAdmissionReason::DuplicateMaturity: return "DuplicateMaturity";
  case SurfaceAdmissionReason::FiniteIvDomain: return "FiniteIvDomain";
  case SurfaceAdmissionReason::EuropeanPriceBounds: return "EuropeanPriceBounds";
  case SurfaceAdmissionReason::StrikeMonotonicity: return "StrikeMonotonicity";
  case SurfaceAdmissionReason::StrikeConvexity: return "StrikeConvexity";
  case SurfaceAdmissionReason::CalendarTotalVariance: return "CalendarTotalVariance";
  case SurfaceAdmissionReason::ForwardVariance: return "ForwardVariance";
  case SurfaceAdmissionReason::RequiredTenorBucket: return "RequiredTenorBucket";
  case SurfaceAdmissionReason::DiagnosticsUnavailable: return "DiagnosticsUnavailable";
  }
  return "?";
}

const char *build_stage_name(SurfaceBuildStage stage) {
  switch (stage) {
  case SurfaceBuildStage::Selection: return "Selection";
  case SurfaceBuildStage::InputValidation: return "InputValidation";
  case SurfaceBuildStage::Build: return "Build";
  case SurfaceBuildStage::Admission: return "Admission";
  case SurfaceBuildStage::Publication: return "Publication";
  }
  return "?";
}

const char *expiry_build_outcome_name(ExpiryBuildOutcome outcome) {
  switch (outcome) {
  case ExpiryBuildOutcome::Missing: return "Missing";
  case ExpiryBuildOutcome::Fitted: return "Fitted";
  case ExpiryBuildOutcome::DuplicateMaturity: return "DuplicateMaturity";
  }
  return "?";
}

const char *expiry_fit_outcome_name(ExpiryFitOutcome outcome) {
  switch (outcome) {
  case ExpiryFitOutcome::Fitted: return "Fitted";
  case ExpiryFitOutcome::FittedFallbackCurve: return "FittedFallbackCurve";
  case ExpiryFitOutcome::FittedLegacyPrep: return "FittedLegacyPrep";
  case ExpiryFitOutcome::CarryFailed: return "CarryFailed";
  case ExpiryFitOutcome::PrepStarved: return "PrepStarved";
  case ExpiryFitOutcome::PrepFailed: return "PrepFailed";
  case ExpiryFitOutcome::FitFailed: return "FitFailed";
  case ExpiryFitOutcome::Skipped: return "Skipped";
  case ExpiryFitOutcome::PrepUncovered: return "PrepUncovered";
  case ExpiryFitOutcome::FitRefusedCalendar: return "FitRefusedCalendar";
  }
  return "?";
}

const char *parity_state_name(ParityDiagnosticState state) {
  switch (state) {
  case ParityDiagnosticState::NotScored: return "NotScored";
  case ParityDiagnosticState::Disabled: return "Disabled";
  case ParityDiagnosticState::Failed: return "Failed";
  case ParityDiagnosticState::Valid: return "Valid";
  }
  return "?";
}

// ── The ValidationDigest CSV contract, in ONE place ─────────────────────────
//
// The served surface exports it under `oracle_`, every build attempt under
// `att_`. `digest_header` and `format_digest` are written adjacent and MUST be
// edited together: a second hand-written copy of this projection would diverge
// the first time `ValidationDigest` gains a counter, and the two column blocks
// would silently stop meaning the same thing.
[[nodiscard]] std::string digest_header(std::string_view prefix) {
  static constexpr std::string_view kFields[] = {"n_slices",
                                                 "n_strike_samples",
                                                 "n_calendar_samples",
                                                 "n_non_finite",
                                                 "n_price_bound_violations",
                                                 "n_strike_monotonicity_violations",
                                                 "n_butterfly_violations",
                                                 "n_calendar_violations",
                                                 "n_wing_violations",
                                                 "max_calendar_slack",
                                                 "max_butterfly_slack",
                                                 "max_price_bound_slack",
                                                 "max_wing_slope_excess",
                                                 "first_calendar_k",
                                                 "first_butterfly_k",
                                                 // T3e. The butterfly slack is a difference of two
                                                 // ADJACENT-CELL price slopes on a forward-
                                                 // normalised grid, so on its own it is not a
                                                 // quantity anyone can judge. It becomes money
                                                 // only with the slice it sits on (that slice's
                                                 // forward, and its cell width). The slice index
                                                 // supplies that join; the two slopes let a reader
                                                 // reconstruct the slack rather than trust the
                                                 // reported maximum. All four already exist on the
                                                 // digest and were simply not being written out.
                                                 "first_butterfly_slice",
                                                 "first_butterfly_slope_left",
                                                 "first_butterfly_slope_right",
                                                 "first_calendar_long_slice"};
  std::string out;
  for (const std::string_view field : kFields) {
    if (!out.empty()) out += ',';
    out += prefix;
    out += field;
  }
  return out;
}

// ValidationDigest counters are std::uint32_t, not std::size_t: `%u`, never
// `%zu`. A mismatched conversion specifier is undefined behaviour, and the two
// types differ in width on this target.
// T3e widened the slack/slope columns from %.6g to %.9g. A butterfly slack is
// order 1e-4 and the economic question asked of it — what bid-ask width would
// this arbitrage have to beat to be harvestable — divides it by a cell width of
// order 1e-2, so six significant digits was discarding the answer's precision,
// not the noise.
[[nodiscard]] std::string format_digest(const ValidationDigest &v) {
  char buf[384];
  std::snprintf(buf, sizeof(buf),
                "%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%.9g,%u",
                v.n_slices, v.n_strike_samples, v.n_calendar_samples, v.n_non_finite,
                v.n_price_bound_violations, v.n_strike_monotonicity_violations,
                v.n_butterfly_violations, v.n_calendar_violations, v.n_wing_violations,
                v.max_calendar_slack, v.max_butterfly_slack, v.max_price_bound_slack,
                v.max_wing_slope_excess, v.first_calendar_k, v.first_butterfly_k,
                v.first_butterfly_slice, v.first_butterfly_slope_left,
                v.first_butterfly_slope_right, v.first_calendar_long_slice);
  return buf;
}

// An absent optional is written as an EMPTY cell, never as 0 — same reason
// `oracle_ran` exists: a number that was never measured must not read as a
// measurement that came out zero.
[[nodiscard]] std::string format_optional(const std::optional<double> &v) {
  if (!v.has_value()) return {};
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", *v);
  return buf;
}

FitPreset parse_preset(std::string_view name) {
  if (name == "accurate") return FitPreset::Accurate;
  if (name == "robust") return FitPreset::Robust;
  if (name == "hft") return FitPreset::Hft;
  if (name == "populate") return FitPreset::Populate;
  if (name == "bulk") return FitPreset::Bulk;
  return FitPreset::Fast;
}

// ── T1b: which fit CONTRACT the harness exercises ───────────────────────────
//
// Until T1b this example named neither `PricerConfig::quality_mode` nor
// `::outputs`, so `PricerFitter::is_v2_request()` was false on every board and
// every number the sprint published came from the LEGACY single-surface branch
// (`pricer_fitter.cpp:621`) with the independent risk oracle never run. The
// path that actually persists surfaces — `populate_universe_streaming` ->
// `pricer_config_for_symbol` (`surface_db_populate.cpp:52`) — is a v2 request.
// The benchmark was measuring a configuration nobody serves.
//
// `Production` reproduces that translation; `Legacy` leaves `PricerConfig`
// exactly as it was, so one run pair is an A/B of the two contracts.
enum class FitPath : std::uint8_t { Legacy, Production };

// The five `PricerConfig` fields `pricer_config_for_symbol` sets beyond the
// preset, plus the four optional<bool> knobs it forwards, each independently
// switchable so the breadth delta can be attributed one field at a time. The
// enum VALUES are never hardcoded here: they are read from
// `symbol_config_from_preset(preset)`, the same seed
// `populate_universe_streaming` writes into the manifest.
struct FitPathSpec {
  FitPath path{FitPath::Production};
  bool name_quality_mode{false};
  bool name_outputs{false};
  bool publish_floor{false};  // admission = populate_admission_policy()
  bool symbol_knobs{false};   // the four optional<bool> forwards
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfaceOutputs outputs{SurfaceOutputs::Risk};
  RiskAdmission risk_admission{RiskAdmission::Required};
  SurfaceFallback fallback{SurfaceFallback::LastKnownGood};
  // Only assigned when the path (or an override) asks for them; the legacy
  // request must stay byte-identical to the pre-T1b config, and both fields
  // already default to these values on PricerConfig.
  bool set_risk_admission{false};
  bool set_fallback{false};
};

[[nodiscard]] const char *outputs_name(SurfaceOutputs o) {
  switch (o) {
  case SurfaceOutputs::MarketMark: return "mark";
  case SurfaceOutputs::Risk: return "risk";
  case SurfaceOutputs::MarketMarkAndRisk: return "mark+risk";
  }
  return "?";
}

[[nodiscard]] const char *risk_admission_name(RiskAdmission a) {
  switch (a) {
  case RiskAdmission::NotApplicable: return "na";
  case RiskAdmission::Required: return "required";
  }
  return "?";
}

[[nodiscard]] const char *fallback_name(SurfaceFallback f) {
  switch (f) {
  case SurfaceFallback::None: return "none";
  case SurfaceFallback::LastKnownGood: return "lkg";
  }
  return "?";
}

// A compact, machine-readable record of the EFFECTIVE contract, emitted per row
// so a CSV can never be mistaken for one produced under different overrides.
// `-` marks a field the request leaves unnamed, which for quality_mode/outputs
// is exactly the legacy signal.
[[nodiscard]] std::string fit_config_label(const FitPathSpec &s) {
  std::string out = "qm=";
  out += s.name_quality_mode ? std::string(to_string(s.quality_mode)) : std::string("-");
  out += ",out=";
  out += s.name_outputs ? outputs_name(s.outputs) : "-";
  out += ",ra=";
  out += s.set_risk_admission ? risk_admission_name(s.risk_admission) : "-";
  out += ",fb=";
  out += s.set_fallback ? fallback_name(s.fallback) : "-";
  out += ",floor=";
  out += s.publish_floor ? "populate" : "default";
  out += ",knobs=";
  out += s.symbol_knobs ? "on" : "off";
  return out;
}

// Mirror of `surface_db_populate.cpp`'s TU-private `pricer_config_for_symbol`
// (the precedent for reproducing it in an example is `spy_fit_rca.cpp:55`).
//
// Deliberate divergences, all of them because an EXAMPLE cannot be the populate
// driver, not because the fidelity is optional:
//   * `pin_curve` is left false. Populate pins only the designated index leg
//     (`seed_symbol_config`); this harness has no index leg and its whole
//     purpose is to measure the auto-selector, so pinning would change what is
//     being measured rather than make it more production-like.
//   * The fields `PricerConfig` cannot carry (band_k, al_override/al,
//     calendar_repair) reach the real fit through `fit_board`'s
//     `session_overlay` hook, which is not on `PricerConfig`. `PricerFitter::fit`
//     does accept an overlay, but the risk pipeline deliberately runs it LAST
//     (pricer_fitter.cpp, MERGE note) and populate's own overlay is
//     `apply_symbol_config`; reproducing it here would be reproducing the
//     driver, not the config. Flagged rather than papered over.
void apply_fit_path(PricerConfig &cfg, const SymbolFitConfig &sym, const FitPathSpec &spec) {
  if (spec.name_quality_mode) {
    cfg.quality_mode = spec.quality_mode;
  }
  if (spec.name_outputs) {
    cfg.outputs = spec.outputs;
  }
  if (spec.set_risk_admission) {
    cfg.risk_admission = spec.risk_admission;
  }
  if (spec.set_fallback) {
    cfg.fallback = spec.fallback;
  }
  if (spec.symbol_knobs) {
    // Plain bools on SymbolFitConfig, optional<bool> on PricerConfig: the
    // populate translation ENGAGES all four, and two of them (score_parity,
    // enforce_calendar_floor) are hard preconditions of a v2 risk request —
    // pricer_fitter.cpp rejects the whole request as "invalid correctness
    // policy" if either is explicitly false.
    cfg.use_correction_cache = sym.use_correction_cache;
    cfg.score_parity = sym.score_parity;
    cfg.enforce_calendar_floor = sym.enforce_calendar_floor;
    cfg.use_deam_cache_for_fit = sym.use_deam_cache_for_fit;
  }
  if (spec.publish_floor) {
    cfg.admission = populate_admission_policy();
  }
}

// One symbol's full outcome. Plain data; workers write disjoint slots.
struct Row {
  std::string symbol;
  std::string status{"skipped"}; // load_missing|load_error|chain_error|fit_error|fit_exception|ok
  std::string error;             // error to_string (or exception what)
  // board shape
  std::size_t n_rows{0};      // parquet quote rows
  std::size_t n_options{0};   // chain option ids (post-build)
  double spot{0.0};
  // routing decision
  std::string profile;
  double profile_conf{0.0};
  std::string decision_source;
  std::string effective_preset;
  std::string chosen_kind;
  std::string primary_kind;
  bool used_fallback{false};
  bool selector_fallback{false}; // selector refused; profile's direct route served
  std::string selector_error;    // the refusal text, when the selector produced one
  bool selector_ran{false};
  double selector_oos_vw{0.0};
  // Raw classifier features behind the routing decision. Emitted so a
  // reproducibility study can attribute a cross-session routing flip to the
  // board observable that moved, instead of inferring it from the verdict.
  std::uint32_t f_live_quotes{0};
  std::uint32_t f_live_expiries{0};
  std::uint32_t f_quoted_expiries{0};
  std::uint32_t f_atm_quotes{0};
  std::uint32_t f_ident_expiries{0};
  std::uint32_t f_max_nm_strikes{0};
  double f_median_spread{0.0};
  std::uint32_t f_front_expiries{0};
  bool f_weeklies{false};
  // fit diagnostics
  double worst_in_band{0.0};
  double mean_in_band{0.0};
  double mean_chi2{0.0};
  double mean_rmse_vol{0.0};
  bool calendar_arb_free{false};
  // The boolean conflates "the check ran and found violations" with "the check
  // itself failed" — a failed check is stamped with the sentinel count 1 so the
  // `calendar_arb_free == (n_calendar_viol_pre == 0)` invariant holds. Exporting
  // the raw count is what separates the two, and a count above the sentinel is
  // unambiguously real arbitrage.
  std::size_t n_calendar_viol{0};
  std::size_t n_price_bound_viol{0};
  std::size_t n_slices{0};
  std::size_t n_quotes_used{0};
  // ── Independent risk oracle (SurfaceHealth / ValidationDigest) ──────────────
  // Deliberately `oracle_`-prefixed and kept apart from the SessionDiagnostics
  // fields above: the two measure different things. The legacy booleans are
  // written per-lane over lane-specific bands (eSSVI |k| <= 3.0, polymorphic
  // |k| <= 0.6); the oracle certifies one band, |k| <= 0.5 at 1e-8, for every
  // board that reaches the risk stage. Confusing the two is what produced the
  // sprint plan's two withdrawn conclusions.
  //
  // `oracle_ran` is NOT redundant with the counters. A default-constructed
  // SurfaceHealth is {state=Rejected, reasons=InsufficientData, all counters 0},
  // so a board whose risk stage never executed is byte-identical to a board the
  // oracle inspected and found clean. Only the generation stamp separates them.
  bool oracle_ran{false};
  std::string oracle_state;       // to_string(SurfaceState)
  std::uint32_t oracle_reasons{0}; // ValidationFailure bitmask, as an integer
  std::uint64_t oracle_candidate_generation{0};
  std::uint64_t oracle_served_generation{0};
  // Held whole rather than unpacked into scalars so the served surface and each
  // build attempt are formatted by the same `format_digest`.
  ValidationDigest oracle_digest{};
  // The market-mark surface's state, exported alongside the risk state so a
  // reader can tell WHICH surface `PricerFitter::surface()` handed back rather
  // than inferring it from the config.
  std::string mm_state;
  // T4 escalation (T10c): banded parity-evidence counters (SessionDiagnostics
  // n_parity_*). Appended as the LAST CSV columns so the shared-column prefix
  // of a pre-counter baseline stays byte-comparable.
  std::size_t n_parity_scored{0};
  std::size_t n_parity_in_band{0};
  std::size_t n_parity_out_of_band{0};
  // valuation
  std::size_t n_valued{0};
  std::size_t n_price_nan{0};
  std::size_t n_bidiv_nan{0};
  std::size_t n_askiv_nan{0};
  // timings
  double load_ms{0.0}; // per-board share, measured around corpus_board_from_opra
  double chain_ms{0.0};
  double fit_ms{0.0};
  double value_ms{0.0};
};

void record_decision(Row &row, const FitDecision &d) {
  row.profile = profile_name(d.profile.kind);
  row.profile_conf = d.profile.confidence;
  row.decision_source = source_name(d.source);
  row.effective_preset = preset_name(d.preset);
  row.chosen_kind = to_string(d.curve.kind);
  row.primary_kind = to_string(d.primary_curve.kind);
  row.used_fallback = d.used_fallback;
  row.selector_fallback = d.selector_fallback;
  row.f_live_quotes = d.features.n_live_quotes;
  row.f_live_expiries = d.features.n_live_expiries;
  row.f_quoted_expiries = d.features.n_quoted_expiries;
  row.f_atm_quotes = d.features.n_atm_quotes;
  row.f_ident_expiries = d.features.n_identifiable_expiries;
  row.f_max_nm_strikes = d.features.max_near_money_strikes;
  row.f_median_spread = d.features.median_spread_pct;
  row.f_front_expiries = d.features.n_front_expiries;
  row.f_weeklies = d.features.has_weeklies;
}

// Copy the independent risk oracle's verdict out of the publication snapshot.
//
// `oracle_ran` is derived from `candidate_generation`, not from the counters,
// because the counters cannot distinguish "inspected, clean" from "never
// inspected" — see the Row comment. `PricerFitter` pre-increments its monotone
// generation counter on entry to `fit` and stamps it into every admission
// decision it takes, so a non-zero `candidate_generation` on `risk_health` is
// exactly the condition "a risk admission decision was reached for this board".
// A board whose policy omitted Risk from `outputs`, or whose build refused
// before the risk stage, leaves the default-constructed value 0.
void record_oracle(Row &row, const SurfaceBundle &bundle) {
  const SurfaceHealth &health = bundle.risk_health;
  row.oracle_ran = health.candidate_generation != 0;
  row.oracle_state = std::string(to_string(health.state));
  row.oracle_reasons = static_cast<std::uint32_t>(health.reasons);
  row.oracle_candidate_generation = health.candidate_generation;
  row.oracle_served_generation = health.served_generation;
  row.oracle_digest = health.validation;
  row.mm_state = std::string(to_string(bundle.market_mark_health.state));
}

// ── One build attempt (T1c) ─────────────────────────────────────────────────
//
// The served digest describes the surface that SURVIVED the fallback ladder. On
// the production path eSSVI is the primary on nearly every board and is rejected
// on most of them, so `oracle_*` measures the SUBSTITUTE and is silent about why
// the intended fit was refused. That evidence is per attempt, and attempts are
// variable in number — hence a second CSV keyed on `symbol`.
struct AttemptRow {
  std::string symbol;
  std::uint32_t attempt_index{0};
  std::uint32_t n_attempts{0};
  const char *report_source{"none"}; // published | last_attempt
  bool report_published{false};
  bool report_used_fallback{false};
  std::string primary_kind;
  std::string published_kind;
  std::string curve_kind;
  const char *stage{"?"};
  bool build_succeeded{false};
  // The FitAdmissionPolicy verdict for THIS attempt. It is one of the two gates
  // a candidate must pass; `admission_failed_checks` is the complete mask over
  // SurfaceAdmissionReason, not just the primary reason.
  bool admitted{false};
  const char *admission_reason{"none"};
  std::uint32_t admission_failed_checks{0};
  std::string failure;
  // Admission evidence that names WHERE the invariant broke, when it did.
  std::size_t ev_attempted_expiries{0};
  std::size_t ev_fitted_expiries{0};
  std::size_t ev_attempted_quotes{0};
  std::size_t ev_fitted_quotes{0};
  double ev_worst_in_band{0.0};
  bool ev_calendar_arb_free{false};
  const char *ev_first_invariant_failure{"none"};
  std::optional<double> ev_first_failure_maturity{};
  std::optional<double> ev_first_failure_k{};
  std::optional<double> ev_first_failure_value{};
  const char *ev_parity_state{"?"};
  double ev_grid_k_min{0.0};
  double ev_grid_k_max{0.0};
  std::size_t ev_grid_points{0};
  // The independent oracle's verdict on THIS candidate — see `probe_health`.
  // `probe_ran` is the per-attempt equivalent of `oracle_ran`: a digest that was
  // never computed is all-zero and byte-identical to a validated-clean one, so
  // the zeros are meaningless unless this is 1.
  bool probe_ran{false};
  const char *probe_source{"none"}; // probe | not_built | pin_refused
  std::string probe_state;
  std::uint32_t probe_reasons{0};
  std::uint32_t probe_failures{0};
  // 1/0 when this attempt's family is the one that was published (the probe must
  // then reproduce the served digest exactly); empty otherwise.
  std::optional<double> probe_id_matches_served{};
  // The probe's OWN admission mask for its first (pinned) attempt, and 1/0 for
  // whether it equals `admission_failed_checks` — the mask production recorded
  // for this same candidate. `probe_id_matches_served` can only certify the
  // PUBLISHED family; this certifies every probed attempt, including the
  // rejected primary whose rejection reason is the whole point of this CSV.
  std::optional<double> probe_admission_matches{};
  ValidationDigest probe_digest{};
  // ── What the served surface cost, relative to THIS attempt (T3e) ──────────
  //
  // The reject-and-substitute path never asks whether the substitute is better
  // than the candidate it replaced: adoption is "the first rung whose
  // publish_candidate is true" (pricer_fitter.cpp:1601, :1689), and by the time
  // a rung is adopted the rejected candidate's session has already been dropped
  // (:1716-1725), so nothing downstream CAN score it. The only quality scalar
  // that survives on the attempt is SurfaceAdmissionEvidence::
  // worst_frac_within_bidask -- and "worst" is a MIN, the one statistic a
  // substitute improves by declining to fit the slice that made it hard.
  //
  // These three deltas are that missing comparison, at the sink. On the
  // rejected primary's row they read directly as "what serving the substitute
  // instead of this cost the board". Empty on the published attempt's own row
  // (comparing it to itself says nothing) and whenever no attempt was published.
  //
  // They are a MEASUREMENT, not a gate: nothing here feeds the decision. The
  // decision seam is pricer_fitter.cpp:1601-1606, where both records are live
  // simultaneously and where a real comparison would have to be made.
  std::optional<double> served_d_worst_in_band{};
  std::optional<double> served_d_fitted_quotes{};
  std::optional<double> served_d_fitted_expiries{};
};

// ── One expiry of one build attempt (T3c) ───────────────────────────────────
//
// `SurfaceBuildAttemptReport::expiries` is the per-chain census the FIT DRIVER
// built (`run_surface_parity` / `fit_curve_surface` -> `ExpiryFitReport`, then
// `completed_attempt_report`). It is the only record that names why a chain the
// board offered never became a slice. Nothing exported it, so "the eSSVI lane
// prepares half as many expiries" was measurable only as a ratio with no cause
// attached.
//
// `fit_outcome_known` is the guard column, and it is not decoration: this sprint
// has twice shipped a metric whose "not populated" was byte-identical to a
// clean zero (a default-constructed `ValidationDigest` reading as 0 violations;
// the `n_calendar_viol` sentinel). `ExpiryBuildReport::fit_outcome` has exactly
// that shape — it defaults to `ExpiryFitOutcome::Fitted` (the enum's 0) and is
// populated ONLY on a `Missing` expiry of an attempt that reached
// `completed_attempt_report` (pricer_fitter.hpp:300-318). A `Fitted` in this
// column is therefore either a real fit or an unpopulated default, and only the
// guard separates them. Read `fit_outcome` where, and only where, it is 1.
struct ExpiryRow {
  std::string symbol;
  std::uint32_t attempt_index{0};
  std::string curve_kind;
  bool build_succeeded{false};
  std::size_t chain_index{0};
  double maturity{0.0};
  const char *build_outcome{"?"};
  std::size_t n_used{0};
  const char *fit_outcome{"?"};
  bool fit_outcome_known{false};
};

// Flatten every attempt's per-chain census. Independent of `collect_attempts`:
// this reads only what the build already recorded and never refits, so it costs
// nothing beyond the walk and can be requested without the probe.
void collect_expiries(std::vector<ExpiryRow> &out, const std::string &symbol,
                      const PricerFitter &fitter) {
  const std::optional<SurfaceBuildReport> &maybe = fitter.published_report().has_value()
                                                       ? fitter.published_report()
                                                       : fitter.last_attempt_report();
  if (!maybe.has_value()) {
    return;
  }
  const SurfaceBuildReport &report = *maybe;
  for (std::size_t i = 0; i < report.attempts.size(); ++i) {
    const SurfaceBuildAttemptReport &a = report.attempts[i];
    for (const ExpiryBuildReport &e : a.expiries) {
      ExpiryRow r;
      r.symbol = symbol;
      r.attempt_index = static_cast<std::uint32_t>(i);
      r.curve_kind = std::string(to_string(a.curve.kind));
      r.build_succeeded = a.build_succeeded;
      r.chain_index = e.expiry_index;
      r.maturity = e.maturity;
      r.build_outcome = expiry_build_outcome_name(e.outcome);
      r.n_used = e.n_used;
      // The exact population contract from pricer_fitter.hpp:300-318 — a rich
      // outcome exists only for a Missing expiry of an attempt that BUILT.
      r.fit_outcome_known = a.build_succeeded && e.outcome == ExpiryBuildOutcome::Missing;
      r.fit_outcome = expiry_fit_outcome_name(e.fit_outcome);
      out.push_back(std::move(r));
    }
  }
}

// ── One SERVED slice of the published surface (T3d) ─────────────────────────
//
// `--expiries-out` (T3c) is the ATTEMPT census: it names why a chain the board
// offered never became a slice. It carries no quality column, so the one
// question it cannot answer is what the slices that DID get served are worth.
// Board-level `mean_in_band`/`worst_in_band` cannot answer it either -- they
// are a mean and a min over exactly this population, and a coverage change
// moves the population, so the aggregate confounds "the surface got worse" with
// "we started serving harder expiries". Separating those two is the whole of
// this task's Part 1, and it needs the per-slice terms.
//
// `VolaSession::parity()` (‖ `expiries()`, both ascending T) is that population,
// already computed and retained by the build. Reading it costs no refit.
//
// GUARD COLUMN, for the reason T3c wrote one: `SessionInputs::score_parity`
// exists and zeroes per-expiry parity intentionally (session.hpp:138-143), so a
// `frac_in_band` of 0.0 is either a genuinely terrible slice or an opt-out, and
// nothing in the value separates them. `parity_scored` does: it is 1 only when
// the session's own `parity_state` is `Valid` AND the parity span is ‖ the
// context span. Read `frac_in_band`/`n_scored`/`rmse_vol`/`chi2` where, and only
// where, it is 1.
struct SliceRow {
  std::string symbol;
  std::string curve_kind; // the family actually SERVED for this board
  std::size_t slice_index{0};
  double T{0.0};
  double forward{0.0};
  std::size_t n_used{0};    // strikes that survived to the fit
  std::size_t n_dropped{0}; // strikes skipped (bad quote / failed invert)
  double frac_in_band{0.0};
  std::size_t n_scored{0};
  std::size_t n_within{0};
  double rmse_vol{0.0};
  double chi2{0.0};
  bool parity_scored{false};
  // eSSVI BACKBONE PARAMETERS, and the guard that says they mean anything (T3e).
  //
  // WHY: (N1)/(N2)/(S1) are not post-hoc projections — they are bounds on the
  // fit's own search domain (`essvi_calib.cpp`: (N1) raises the theta band's
  // floor to the previous slice's theta, (N2) is a lower bound on phi, (S1) an
  // upper cap on phi). So "was the OLD parameter vector still admissible once a
  // new neighbour was inserted ahead of it?" is decidable as arithmetic on
  // (theta, phi, rho) pairs — no refit, no solver — PROVIDED both runs' vectors
  // are on record. That is the only thing this block adds.
  //
  // The plan-space the sprint reasons in is (theta, psi = theta*phi,
  // chi = rho*psi); it is recoverable from these three, so store the natural
  // triple the surface actually holds rather than a derived form.
  //
  // GUARD, in T3d's sense: `essvi_params` is 1 only when the served surface
  // carries one `EssviParams` per `SliceContext`. A ConvexDense or SVI board
  // has no eSSVI backbone at all, and a zeroed theta is not distinguishable
  // from a genuinely tiny one in the value alone.
  bool essvi_params{false};
  double essvi_theta{0.0};
  double essvi_phi{0.0};
  double essvi_rho{0.0};
  // The backbone is what the constraints bind; the wing residual rides on top
  // and moves w(k) without moving (N1)/(N2)/(S1). Recorded so a reader can see
  // when SSE and feasibility are being read off different functions.
  double essvi_resid_scale{0.0};
};

// Flatten the published session's per-slice context + parity. Reads only what
// the build already retained; never refits.
void collect_slices(std::vector<SliceRow> &out, const std::string &symbol,
                    const std::string &kind, const PricerFitter &fitter) {
  const FittedSurface *surf = fitter.surface();
  if (surf == nullptr) {
    return;
  }
  const VolaSession &sess = surf->session();
  const std::span<const SliceContext> ctx = sess.expiries();
  const std::span<const ParityReport> par = sess.parity();
  // The exact condition under which a parity term means what it says.
  const bool scored =
      sess.diagnostics().parity_state == ParityDiagnosticState::Valid && par.size() == ctx.size();
  // Parallel to `scored`: the eSSVI backbone is present only for an eSSVI
  // board, and only usable when it is index-parallel to the context span.
  const std::span<const EssviParams> ess = sess.surface().essvi_slices();
  const bool has_essvi = ess.size() == ctx.size();
  for (std::size_t i = 0; i < ctx.size(); ++i) {
    SliceRow r;
    r.symbol = symbol;
    r.curve_kind = kind;
    r.slice_index = i;
    r.T = ctx[i].T;
    r.forward = ctx[i].forward;
    r.n_used = ctx[i].n_used;
    r.n_dropped = ctx[i].n_dropped;
    r.parity_scored = scored;
    if (scored) {
      r.frac_in_band = par[i].frac_fv_within_bidask;
      r.n_scored = par[i].n;
      r.n_within = par[i].n_within;
      r.rmse_vol = par[i].rmse_mid_vol;
      r.chi2 = par[i].chi2_reduced;
    }
    r.essvi_params = has_essvi;
    if (has_essvi) {
      r.essvi_theta = ess[i].theta;
      r.essvi_phi = ess[i].phi;
      r.essvi_rho = ess[i].rho;
      r.essvi_resid_scale = ess[i].resid_scale;
    }
    out.push_back(std::move(r));
  }
}

// What one pinned refit yields: the oracle's verdict plus the probe's own
// admission mask for the pinned candidate, kept together so the caller cannot
// read one without the other's provenance.
struct ProbeResult {
  SurfaceHealth health{};
  bool has_attempt{false};
  std::uint32_t admission_failed_checks{0};
};

// Re-measure one attempt's candidate with the independent oracle.
//
// LIBRARY LIMIT, read not assumed: `SurfaceBuildAttemptReport` carries the
// admission verdict and its evidence but NOT a `ValidationDigest` — the digest
// is a local in the risk build (`pricer_fitter.cpp:1548/1596/1684`) and only the
// PUBLISHED one survives, on `SurfaceHealth`. Rather than widen the library,
// refit with this attempt's own `CurveConfig` pinned: a pin clears `auto_routed`
// (`:1451`), so neither fallback ladder can fire and the candidate the oracle
// judges is exactly this one. The refusal IS the measurement — `risk_health_` is
// stamped at `:1715` before the Err returns.
//
// The pin costs fidelity in exactly one place, and it is compensated: a pinned
// request skips the profile-calib override at `:1355-1363`. The overlay restores
// it from `curve.parametric`, which is that override's own result (`:1369`,
// `:1394`). `att_probe_id_matches_served` and `att_probe_admission_matches` are
// the empirical checks on all of this.
//
// ONE FAMILY IS NOT MEASURED BARE, and a reader must know which. Strict recovery
// (`:1645`) is gated on `auto_routed || rejected_primary_curve.kind ==
// ConvexDense`. A pin clears `auto_routed`, so for every family EXCEPT
// ConvexDense the probe measures this candidate alone. For a ConvexDense pin the
// second disjunct still fires, so `att_probe_*` describes the ConvexDense
// candidate AFTER up to three convex-repair rounds — the family's outcome, not
// this attempt's raw geometry. Read ConvexDense probe counters as a floor on the
// violations that candidate had, never as its measured total. eSSVI and SVI, the
// families this CSV exists to explain, are unaffected.
[[nodiscard]] ProbeResult probe_health(const PricerConfig &base, const OptionChain &chain,
                                       const CurveConfig &curve) {
  PricerConfig cfg = base;
  cfg.curve = curve;
  PricerFitter fitter{cfg};
  const CalibOpts calib = curve.parametric;
  // SAFETY: the Status is deliberately discarded. A rejected candidate is the
  // expected outcome here and returns Err; the verdict wanted is the health it
  // stamped on the way out, read below.
  static_cast<void>(fitter.fit(chain, [calib](SessionInputs &in) { in.calib = calib; }));
  ProbeResult out;
  out.health = fitter.bundle().risk_health;
  // Attempt 0 is the pinned candidate on every route: the pin is resolved before
  // the first build, and a pinned request cannot reach the auto-routed ladder.
  const std::optional<SurfaceBuildReport> &report = fitter.published_report().has_value()
                                                        ? fitter.published_report()
                                                        : fitter.last_attempt_report();
  if (report.has_value() && !report->attempts.empty()) {
    out.has_attempt = true;
    out.admission_failed_checks = report->attempts.front().admission.failed_checks;
  }
  return out;
}

void collect_attempts(std::vector<AttemptRow> &out, const std::string &symbol,
                      const PricerFitter &fitter, const PricerConfig &cfg,
                      const OptionChain &chain) {
  const bool from_published = fitter.published_report().has_value();
  const std::optional<SurfaceBuildReport> &maybe =
      from_published ? fitter.published_report() : fitter.last_attempt_report();
  if (!maybe.has_value()) {
    return;
  }
  const SurfaceBuildReport &report = *maybe;
  const ValidationDigest &served = fitter.bundle().risk_health.validation;
  const std::size_t n = report.attempts.size();

  // The attempt that actually got served, found the same way the existing
  // `probe_id_matches_served` column finds it (family match against
  // `published_curve`), taking the LAST such attempt because the strict-recovery
  // rung can re-enter the same family after an earlier one was refused. Absent
  // when nothing was published — and then the deltas below stay empty rather
  // than silently comparing against attempt 0.
  std::optional<std::size_t> served_idx;
  if (report.published) {
    for (std::size_t i = 0; i < n; ++i) {
      if (report.attempts[i].curve.kind == report.published_curve.kind &&
          report.attempts[i].build_succeeded) {
        served_idx = i;
      }
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    const SurfaceBuildAttemptReport &a = report.attempts[i];
    AttemptRow r;
    r.symbol = symbol;
    r.attempt_index = static_cast<std::uint32_t>(i);
    r.n_attempts = static_cast<std::uint32_t>(n);
    r.report_source = from_published ? "published" : "last_attempt";
    r.report_published = report.published;
    r.report_used_fallback = report.used_fallback;
    r.primary_kind = std::string(to_string(report.primary_curve.kind));
    r.published_kind = std::string(to_string(report.published_curve.kind));
    r.curve_kind = std::string(to_string(a.curve.kind));
    r.stage = build_stage_name(a.stage);
    r.build_succeeded = a.build_succeeded;
    r.admitted = a.admission.admitted;
    r.admission_reason = admission_reason_name(a.admission.primary_reason);
    r.admission_failed_checks = a.admission.failed_checks;
    if (a.failure.has_value())
      r.failure = a.failure->to_string();
    const SurfaceAdmissionEvidence &e = a.evidence;
    r.ev_attempted_expiries = e.attempted_expiries;
    r.ev_fitted_expiries = e.fitted_expiries;
    r.ev_attempted_quotes = e.attempted_quotes;
    r.ev_fitted_quotes = e.fitted_quotes;
    r.ev_worst_in_band = e.worst_frac_within_bidask;
    r.ev_calendar_arb_free = e.calendar_arb_free;
    r.ev_first_invariant_failure = admission_reason_name(e.first_invariant_failure);
    r.ev_first_failure_maturity = e.first_failure_maturity;
    r.ev_first_failure_k = e.first_failure_log_moneyness;
    r.ev_first_failure_value = e.first_failure_value;
    r.ev_parity_state = parity_state_name(e.parity_state);
    r.ev_grid_k_min = e.invariant_grid_k_min;
    r.ev_grid_k_max = e.invariant_grid_k_max;
    r.ev_grid_points = e.invariant_grid_points;

    // What serving the published surface cost, measured against THIS attempt.
    // Skipped on the served attempt's own row: a self-comparison is always zero
    // and would dilute any aggregate taken over the column.
    if (served_idx.has_value() && *served_idx != i && a.build_succeeded) {
      const SurfaceAdmissionEvidence &s = report.attempts[*served_idx].evidence;
      r.served_d_worst_in_band = s.worst_frac_within_bidask - e.worst_frac_within_bidask;
      r.served_d_fitted_quotes = static_cast<double>(s.fitted_quotes) -
                                 static_cast<double>(e.fitted_quotes);
      r.served_d_fitted_expiries = static_cast<double>(s.fitted_expiries) -
                                   static_cast<double>(e.fitted_expiries);
    }

    // A candidate that never built has no geometry to certify; a LinearVariance
    // PIN is hard-refused as an invalid risk request (`pricer_fitter.cpp:1211`),
    // so probing one would measure the pin instead of the candidate. Both keep
    // `probe_ran = false` and are named, not silently zeroed.
    if (!a.build_succeeded) {
      r.probe_source = "not_built";
    } else if (a.curve.kind == VolCurveKind::LinearVariance) {
      r.probe_source = "pin_refused";
    } else {
      const ProbeResult probe = probe_health(cfg, chain, a.curve);
      const SurfaceHealth &health = probe.health;
      r.probe_ran = health.candidate_generation != 0;
      r.probe_source = "probe";
      r.probe_state = std::string(to_string(health.state));
      r.probe_reasons = static_cast<std::uint32_t>(health.reasons);
      r.probe_failures = static_cast<std::uint32_t>(health.validation.failures);
      r.probe_digest = health.validation;
      if (probe.has_attempt) {
        r.probe_admission_matches =
            probe.admission_failed_checks == a.admission.failed_checks ? 1.0 : 0.0;
      }
      if (report.published && a.curve.kind == report.published_curve.kind) {
        r.probe_id_matches_served =
            health.validation.validation_id == served.validation_id ? 1.0 : 0.0;
      }
    }
    out.push_back(std::move(r));
  }
}

std::vector<std::string> read_symbols_file(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

std::string csv_escape(const std::string &s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string q = "\"";
  for (char c : s) { if (c == '"') q += "\"\""; else q += c; }
  q += "\"";
  return q;
}

// The selector's refusal, when the fit fell back to the profile's own family.
// It survives in the build report as a Selection-stage attempt on BOTH the
// published and the last-attempt report, so an operator can see why a board was
// not cross-validated without re-running anything.
std::string selection_refusal(const PricerFitter &fitter) {
  for (const auto *report : {&fitter.published_report(), &fitter.last_attempt_report()}) {
    if (!report->has_value()) {
      continue;
    }
    for (const SurfaceBuildAttemptReport &attempt : (*report)->attempts) {
      if (attempt.stage == SurfaceBuildStage::Selection && attempt.failure.has_value()) {
        return attempt.failure->to_string();
      }
    }
  }
  return {};
}

} // namespace

int main(int argc, char **argv) {
  std::string opra_root, date, symbols_file, out_csv = "universe_autofit_results.csv";
  std::string snapshot_suffix = "T14:00:00Z";
  // Default matches the per-symbol OPRA v1 hive; a single-file-per-date hive is
  // reachable with --path-template "date={date}/data.parquet".
  std::string path_template = "{symbol}/{date}.parquet";
  std::string preset_name_arg = "robust";
  std::string pin_kind; // empty => auto-select; else pin this family for every board
  double r = 0.043;
  unsigned fit_workers = atx_auto_worker_count();
  std::size_t limit = 0;
  bool do_value = true;
  bool do_fit = true;
  std::optional<unsigned> oos_max_expiries;
  std::optional<double> selector_budget_ms;
  std::optional<std::uint32_t> sparse_floor;
  std::optional<double> min_direct_confidence;
  // T1b. Production is the DEFAULT: the legacy contract is reachable only by
  // asking for it, because a benchmark whose default measures a configuration
  // nothing serves is how this sprint published a wrong headline metric.
  std::string fit_path_arg = "production";
  std::optional<std::string> v2_fields_arg, outputs_arg, risk_admission_arg, fallback_arg,
      publish_floor_arg, symbol_knobs_arg;
  // T1c. Unset => no second CSV and no probe refits: the default run's cost and
  // output stay exactly what T1b measured.
  std::string attempts_csv;
  // T3c. Unset => no third CSV. Costs no refit either way — it only flattens the
  // census the build already recorded.
  std::string expiries_csv;
  // T3d. Unset => no fourth CSV. Same deal: reads the published session's
  // retained per-slice parity, never refits.
  std::string slices_csv;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--opra-root") opra_root = nv();
    else if (a == "--date") date = nv();
    else if (a == "--symbols-file") symbols_file = nv();
    else if (a == "--snapshot-suffix") snapshot_suffix = nv();
    else if (a == "--path-template") path_template = nv();
    else if (a == "--r") r = std::strtod(nv(), nullptr);
    else if (a == "--preset") preset_name_arg = nv();
    else if (a == "--fit-workers") fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--limit") limit = static_cast<std::size_t>(std::strtoull(nv(), nullptr, 10));
    else if (a == "--out") out_csv = nv();
    else if (a == "--no-value") do_value = false;
    else if (a == "--no-fit") do_fit = do_value = false;
    else if (a == "--pin") pin_kind = nv();
    else if (a == "--oos-max-expiries")
      oos_max_expiries = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--selector-budget-ms") selector_budget_ms = std::strtod(nv(), nullptr);
    else if (a == "--sparse-floor")
      sparse_floor = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--min-direct-confidence")
      min_direct_confidence = std::strtod(nv(), nullptr);
    else if (a == "--fit-path") fit_path_arg = nv();
    else if (a == "--v2-fields") v2_fields_arg = nv();
    else if (a == "--outputs") outputs_arg = nv();
    else if (a == "--risk-admission") risk_admission_arg = nv();
    else if (a == "--fallback") fallback_arg = nv();
    else if (a == "--publish-floor") publish_floor_arg = nv();
    else if (a == "--symbol-knobs") symbol_knobs_arg = nv();
    else if (a == "--attempts-out") attempts_csv = nv();
    else if (a == "--expiries-out") expiries_csv = nv();
    else if (a == "--slices-out") slices_csv = nv();
    else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }
  if (opra_root.empty() || date.empty() || symbols_file.empty()) {
    std::fprintf(stderr,
                 "usage: universe_autofit --opra-root DIR --date YYYY-MM-DD --symbols-file FILE "
                 "[--snapshot-suffix T14:00:00Z] [--r 0.043] [--preset robust] [--fit-workers N] "
                 "[--limit N] [--out FILE] [--no-value] [--no-fit] "
                 "[--oos-max-expiries N] [--selector-budget-ms N] [--sparse-floor N] "
                 "[--min-direct-confidence X] [--path-template T] "
                 "[--fit-path production|legacy] [--v2-fields none|quality|outputs|both] "
                 "[--outputs risk|mark|both] [--risk-admission required|na] "
                 "[--fallback lkg|none] [--publish-floor on|off] [--symbol-knobs on|off] "
                 "[--attempts-out FILE] [--expiries-out FILE] [--slices-out FILE]\n");
    return 2;
  }

  std::vector<std::string> symbols = read_symbols_file(symbols_file);
  if (symbols.empty()) {
    std::fprintf(stderr, "no symbols in %s\n", symbols_file.c_str());
    return 2;
  }
  if (limit > 0 && symbols.size() > limit) symbols.resize(limit);
  const FitPreset preset = parse_preset(preset_name_arg);

  // ── Resolve the fit contract ────────────────────────────────────────────
  // The production VALUES come from the seed `populate_universe_streaming`
  // actually writes (`seed_symbol_config` -> `symbol_config_from_preset`), not
  // from a default-constructed `SurfacePolicy`. The two disagree: the seed maps
  // a Risk-purpose preset to `SurfaceOutputs::Risk`, while `SurfacePolicy{}` is
  // `MarketMarkAndRisk`. Reading the seed is what makes this the served config
  // rather than a plausible-looking neighbour of it.
  const SymbolFitConfig production_symbol_cfg = symbol_config_from_preset(preset);
  FitPathSpec spec_build;
  if (fit_path_arg == "legacy") {
    spec_build.path = FitPath::Legacy;
  } else if (fit_path_arg == "production") {
    spec_build.path = FitPath::Production;
  } else {
    std::fprintf(stderr, "--fit-path must be production or legacy (got %s)\n",
                 fit_path_arg.c_str());
    return 2;
  }
  spec_build.quality_mode = production_symbol_cfg.surface_policy.quality_mode;
  spec_build.outputs = production_symbol_cfg.surface_policy.outputs;
  spec_build.risk_admission = production_symbol_cfg.surface_policy.risk_admission;
  spec_build.fallback = production_symbol_cfg.surface_policy.fallback;
  if (spec_build.path == FitPath::Production) {
    spec_build.name_quality_mode = true;
    spec_build.name_outputs = true;
    spec_build.set_risk_admission = true;
    spec_build.set_fallback = true;
    spec_build.publish_floor = true;
    spec_build.symbol_knobs = true;
  }
  // Per-field overrides. Their ONLY purpose is attribution: run production with
  // one field pushed back to its legacy value and the breadth delta that moves
  // is that field's cost. Not a production shape — the CSV's `fit_config`
  // column records what was actually requested.
  const auto parse_on_off = [](const std::string &v, const char *flag, bool &out) -> bool {
    if (v == "on") { out = true; return true; }
    if (v == "off") { out = false; return true; }
    std::fprintf(stderr, "%s must be on or off (got %s)\n", flag, v.c_str());
    return false;
  };
  if (v2_fields_arg.has_value()) {
    const std::string &v = *v2_fields_arg;
    if (v != "none" && v != "quality" && v != "outputs" && v != "both") {
      std::fprintf(stderr, "--v2-fields must be none|quality|outputs|both (got %s)\n", v.c_str());
      return 2;
    }
    spec_build.name_quality_mode = (v == "quality" || v == "both");
    spec_build.name_outputs = (v == "outputs" || v == "both");
  }
  if (outputs_arg.has_value()) {
    const std::string &v = *outputs_arg;
    if (v == "risk") spec_build.outputs = SurfaceOutputs::Risk;
    else if (v == "mark") spec_build.outputs = SurfaceOutputs::MarketMark;
    else if (v == "both") spec_build.outputs = SurfaceOutputs::MarketMarkAndRisk;
    else {
      std::fprintf(stderr, "--outputs must be risk|mark|both (got %s)\n", v.c_str());
      return 2;
    }
  }
  if (risk_admission_arg.has_value()) {
    const std::string &v = *risk_admission_arg;
    if (v == "required") spec_build.risk_admission = RiskAdmission::Required;
    else if (v == "na") spec_build.risk_admission = RiskAdmission::NotApplicable;
    else {
      std::fprintf(stderr, "--risk-admission must be required|na (got %s)\n", v.c_str());
      return 2;
    }
    spec_build.set_risk_admission = true;
  }
  if (fallback_arg.has_value()) {
    const std::string &v = *fallback_arg;
    if (v == "lkg") spec_build.fallback = SurfaceFallback::LastKnownGood;
    else if (v == "none") spec_build.fallback = SurfaceFallback::None;
    else {
      std::fprintf(stderr, "--fallback must be lkg|none (got %s)\n", v.c_str());
      return 2;
    }
    spec_build.set_fallback = true;
  }
  if (publish_floor_arg.has_value() &&
      !parse_on_off(*publish_floor_arg, "--publish-floor", spec_build.publish_floor)) {
    return 2;
  }
  if (symbol_knobs_arg.has_value() &&
      !parse_on_off(*symbol_knobs_arg, "--symbol-knobs", spec_build.symbol_knobs)) {
    return 2;
  }
  // Frozen before any worker starts: the contract must be one immutable value
  // for the whole run, and every fit thread reads it concurrently.
  const FitPathSpec fit_spec = spec_build;
  const std::string fit_config = fit_config_label(fit_spec);
  const char *const fit_path_name = fit_spec.path == FitPath::Production ? "production" : "legacy";

  // Progress must be visible under output redirection (Windows stdio is fully
  // buffered when stdout is not a console).
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("[universe_autofit] symbols=%zu date=%s snapshot=%s preset=%s fit-workers=%u\n",
              symbols.size(), date.c_str(), snapshot_suffix.c_str(), preset_name_arg.c_str(),
              fit_workers);
  std::printf("[fit-path] %s  %s  (v2_request=%d)\n", fit_path_name, fit_config.c_str(),
              (fit_spec.name_quality_mode || fit_spec.name_outputs) ? 1 : 0);
#if defined(NDEBUG)
  std::printf("[build] Release (NDEBUG)\n");
#else
  std::printf("[build] *** DEBUG BUILD — timings not representative ***\n");
#endif

  // ── Load the snapshot hive (single date) ──────────────────────────────────
  const auto t_load0 = SteadyClock::now();
  OpraBatchSpec spec;
  spec.symbols = symbols;
  spec.date_lo = date;
  spec.date_hi = date;
  spec.root_dir = opra_root;
  spec.snapshot_suffix = snapshot_suffix;
  spec.path_template = path_template;
  spec.r = r;
  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  const double load_total_ms = ms_since(t_load0);
  std::printf("[load] loaded=%zu missing=%zu error=%zu of %zu in %.1fs\n", batch->n_loaded,
              batch->n_missing, batch->n_error, batch->n_total, load_total_ms / 1e3);

  // ── Boards + rows (parallel array; entry i <-> rows[i]) ───────────────────
  std::vector<Row> rows(batch->entries.size());
  // Per board, so workers write disjoint slots exactly as they do for `rows`;
  // flattened in board order at write time.
  std::vector<std::vector<AttemptRow>> attempt_rows(batch->entries.size());
  std::vector<std::vector<ExpiryRow>> expiry_rows(batch->entries.size());
  std::vector<std::vector<SliceRow>> slice_rows(batch->entries.size());
  const bool want_attempts = !attempts_csv.empty();
  const bool want_expiries = !expiries_csv.empty();
  const bool want_slices = !slices_csv.empty();
  std::vector<const OpraBatchEntry *> entries(batch->entries.size());
  for (std::size_t i = 0; i < batch->entries.size(); ++i) entries[i] = &batch->entries[i];

  const auto t_fit0 = SteadyClock::now();
  std::atomic<std::size_t> n_done{0};
  parallel_for(entries.size(), fit_workers, [&](std::size_t i) {
    const OpraBatchEntry &e = *entries[i];
    Row &row = rows[i];
    row.symbol = e.symbol;
    // Progress line on every exit path (stderr is unbuffered, safe under redirect).
    struct Progress {
      const Row &r;
      std::atomic<std::size_t> &done;
      std::size_t total;
      SteadyClock::time_point t0;
      ~Progress() {
        const std::size_t k = ++done;
        const double el = std::chrono::duration<double>(SteadyClock::now() - t0).count();
        std::fprintf(stderr, "[%zu/%zu] %-8s %-14s fit=%.0fms eta=%.0fs\n", k, total,
                     r.symbol.c_str(), r.status.c_str(), r.fit_ms,
                     k ? el / static_cast<double>(k) * static_cast<double>(total - k) : 0.0);
      }
    } progress{row, n_done, entries.size(), t_fit0};
    if (!e.panel) {
      const bool missing = e.panel.error().code() == ErrorCode::NotFound;
      row.status = missing ? "load_missing" : "load_error";
      row.error = e.panel.error().to_string();
      return;
    }

    try {
      const auto t0 = SteadyClock::now();
      CorpusBoard board = corpus_board_from_opra(e.date, e.symbol, *e.panel);
      row.load_ms = ms_since(t0);
      row.n_rows = board.frame.rows.size();
      row.spot = board.frame.spot;

      const auto t1 = SteadyClock::now();
      Result<OptionChain> chain = OptionChain::from_frame(board.frame, board.env);
      row.chain_ms = ms_since(t1);
      if (!chain) {
        row.status = "chain_error";
        row.error = chain.error().to_string();
        return;
      }
      row.n_options = chain->ids().size();

      PricerConfig cfg;
      cfg.preset = preset;
      cfg.context = board.fit_context;
      cfg.n_threads = 1; // board-level parallelism only; keep each fit serial
      cfg.fit_workers = 1; // prevent board fan-out from nesting expiry fan-out
      if (oos_max_expiries.has_value()) cfg.selector.oos_max_expiries = *oos_max_expiries;
      if (selector_budget_ms.has_value()) cfg.selector.time_budget_ms = *selector_budget_ms;
      if (sparse_floor.has_value()) cfg.policy.sparse_validation_floor = *sparse_floor;
      if (min_direct_confidence.has_value())
        cfg.policy.min_direct_confidence = *min_direct_confidence;
      if (!pin_kind.empty()) {
        CurveConfig cc;
        if (pin_kind == "linear-variance") cc.kind = VolCurveKind::LinearVariance;
        else if (pin_kind == "essvi") cc.kind = VolCurveKind::Essvi;
        else if (pin_kind == "svi") cc.kind = VolCurveKind::Svi;
        else if (pin_kind == "c8") cc.kind = VolCurveKind::C8;
        else cc.kind = VolCurveKind::ConvexDense;
        cfg.curve = cc;
      }
      // Last, so the contract under measurement is never silently undone by a
      // knob above it.
      apply_fit_path(cfg, production_symbol_cfg, fit_spec);

      // Routing-only mode: resolve the policy exactly as PricerFitter would and
      // stop. Used to study classifier reproducibility over a whole universe
      // without paying for the fit, which dominates the wall clock.
      if (!do_fit) {
        record_decision(row, select_fit_policy(chain->underlying(), chain->underlying().ticker,
                                               cfg.context, cfg.policy));
        row.status = "ok";
        return;
      }
      PricerFitter fitter{cfg};

      const auto t2 = SteadyClock::now();
      const Status st = fitter.fit(chain.value());
      row.fit_ms = ms_since(t2);
      if (!st) {
        row.status = "fit_error";
        row.error = st.error().to_string();
        // decision may still explain what the policy attempted
        if (fitter.decision()) {
          record_decision(row, *fitter.decision());
        }
        row.selector_error = selection_refusal(fitter);
        // A board that served nothing is exactly where the ladder history is
        // most informative, so it is collected on this path too.
        if (want_attempts) {
          collect_attempts(attempt_rows[i], row.symbol, fitter, cfg, chain.value());
        }
        if (want_expiries) {
          collect_expiries(expiry_rows[i], row.symbol, fitter);
        }
        return;
      }

      if (fitter.decision()) {
        record_decision(row, *fitter.decision());
      }
      row.selector_error = selection_refusal(fitter);
      if (fitter.selection()) {
        row.selector_ran = true;
        const SelectorResult &sel = *fitter.selection();
        if (sel.chosen_index < sel.scores.size()) {
          row.selector_oos_vw = sel.scores[sel.chosen_index].oos_vw;
        }
      }
      const SessionDiagnostics &dg = fitter.surface()->diagnostics();
      row.worst_in_band = dg.worst_frac_within_bidask;
      row.mean_in_band = dg.mean_frac_within_bidask;
      row.mean_chi2 = dg.mean_chi2_reduced;
      row.mean_rmse_vol = dg.mean_rmse_vol;
      row.calendar_arb_free = dg.calendar_arb_free;
      row.n_calendar_viol = dg.n_calendar_viol_pre;
      row.n_price_bound_viol = dg.n_price_bound_violations;
      row.n_slices = dg.n_slices;
      row.n_quotes_used = dg.n_quotes;
      row.n_parity_scored = dg.n_parity_scored;
      row.n_parity_in_band = dg.n_parity_in_band;
      row.n_parity_out_of_band = dg.n_parity_out_of_band;
      record_oracle(row, fitter.bundle());
      if (want_attempts) {
        collect_attempts(attempt_rows[i], row.symbol, fitter, cfg, chain.value());
      }
      if (want_expiries) {
        collect_expiries(expiry_rows[i], row.symbol, fitter);
      }
      if (want_slices) {
        collect_slices(slice_rows[i], row.symbol, row.chosen_kind, fitter);
      }

      if (do_value) {
        const auto t3 = SteadyClock::now();
        const Result<ChainValuation> val =
            fitter.value_chain(chain.value(), OutputField::Prices | OutputField::Bands, 1);
        row.value_ms = ms_since(t3);
        if (val) {
          row.n_valued = val->size();
          for (std::size_t k = 0; k < val->size(); ++k) {
            if (!(val->model_price[k] == val->model_price[k])) ++row.n_price_nan;
            if (!(val->bid_iv[k] == val->bid_iv[k])) ++row.n_bidiv_nan;
            if (!(val->ask_iv[k] == val->ask_iv[k])) ++row.n_askiv_nan;
          }
        }
      }
      row.status = "ok";
    } catch (const std::exception &ex) {
      row.status = "fit_exception";
      row.error = ex.what();
    } catch (...) {
      row.status = "fit_exception";
      row.error = "unknown exception";
    }
  });
  const double fit_total_ms = ms_since(t_fit0);

  // ── CSV ────────────────────────────────────────────────────────────────────
  {
    std::ofstream out(out_csv, std::ios::trunc);
    out << "symbol,status,error,n_rows,n_options,spot,profile,profile_conf,decision_source,"
           "effective_preset,chosen_kind,primary_kind,used_fallback,selector_ran,selector_oos_vw,"
           "worst_in_band,mean_in_band,mean_chi2,mean_rmse_vol,calendar_arb_free,"
           "n_calendar_viol,n_price_bound_viol,n_slices,"
           "n_quotes_used,n_valued,n_price_nan,n_bidiv_nan,n_askiv_nan,load_ms,chain_ms,fit_ms,"
           "value_ms,selector_fallback,f_live_quotes,f_live_expiries,f_quoted_expiries,"
           "f_atm_quotes,f_ident_expiries,f_max_nm_strikes,f_median_spread,f_front_expiries,"
           "f_weeklies,selector_error,"
           // Appended block: independent risk oracle. Existing columns and their
           // order are frozen so previously-written analysis scripts keep working
           // — `digest_header` reproduces exactly the names T1 wrote, and is used
           // here rather than a second literal so the projection cannot drift
           // from `format_digest`, which emits this row's values.
           "oracle_ran,oracle_state,oracle_reasons,oracle_candidate_generation,"
           "oracle_served_generation,"
        << digest_header("oracle_")
        << ",mm_state,"
           // T1b: which fit contract produced this row. Constant within a run;
           // per-row so a concatenation of two runs stays self-describing.
           "fit_path,fit_config,"
           // T4 escalation (T10c): banded parity-evidence counters. Kept as the
           // LAST CSV columns so the shared-column prefix of a pre-counter
           // baseline stays byte-comparable.
           "n_parity_scored,n_parity_in_band,n_parity_out_of_band\n";
    for (const Row &w : rows) {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    ",%zu,%zu,%.6f,%s,%.6f,%s,%s,%s,%s,%d,%d,%.4f,%.4f,%.4f,%.4f,%.6f,%d,%zu,%zu,"
                    "%zu,%zu,"
                    "%zu,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%d,",
                    w.n_rows, w.n_options, w.spot, w.profile.c_str(), w.profile_conf,
                    w.decision_source.c_str(), w.effective_preset.c_str(), w.chosen_kind.c_str(),
                    w.primary_kind.c_str(), w.used_fallback ? 1 : 0, w.selector_ran ? 1 : 0,
                    w.selector_oos_vw, w.worst_in_band, w.mean_in_band, w.mean_chi2,
                    w.mean_rmse_vol, w.calendar_arb_free ? 1 : 0, w.n_calendar_viol,
                    w.n_price_bound_viol, w.n_slices, w.n_quotes_used, w.n_valued, w.n_price_nan,
                    w.n_bidiv_nan, w.n_askiv_nan, w.load_ms, w.chain_ms, w.fit_ms, w.value_ms,
                    w.selector_fallback ? 1 : 0);
      char fbuf[256];
      std::snprintf(fbuf, sizeof(fbuf), "%u,%u,%u,%u,%u,%u,%.6g,%u,%d,", w.f_live_quotes,
                    w.f_live_expiries, w.f_quoted_expiries, w.f_atm_quotes, w.f_ident_expiries,
                    w.f_max_nm_strikes, w.f_median_spread, w.f_front_expiries,
                    w.f_weeklies ? 1 : 0);
      // The 64-bit generation stamps are cast to `unsigned long long` so `%llu`
      // is exact by construction rather than by assuming what std::uint64_t maps
      // to. The digest itself goes through `format_digest`, the one projection
      // shared with the per-attempt CSV.
      char obuf[128];
      std::snprintf(obuf, sizeof(obuf), ",%d,%s,%u,%llu,%llu,", w.oracle_ran ? 1 : 0,
                    w.oracle_state.c_str(), w.oracle_reasons,
                    static_cast<unsigned long long>(w.oracle_candidate_generation),
                    static_cast<unsigned long long>(w.oracle_served_generation));
      out << csv_escape(w.symbol) << ',' << w.status << ',' << csv_escape(w.error) << buf << fbuf
          << csv_escape(w.selector_error) << obuf << format_digest(w.oracle_digest) << ','
          << w.mm_state << ',' << fit_path_name << ',' << csv_escape(fit_config) << ','
          << w.n_parity_scored << ',' << w.n_parity_in_band << ',' << w.n_parity_out_of_band
          << '\n';
    }
  }

  // ── Per-attempt CSV (T1c) ──────────────────────────────────────────────────
  //
  // Written only on request. One row per (symbol, attempt), keyed on `symbol` so
  // it joins to the main CSV — board shape, routing and the served digest stay
  // there and are not duplicated here.
  std::size_t n_attempt_rows = 0;
  if (want_attempts) {
    std::ofstream out(attempts_csv, std::ios::trunc);
    out << "symbol,attempt_index,n_attempts,report_source,report_published,report_used_fallback,"
           "primary_kind,published_kind,curve_kind,stage,build_succeeded,admitted,"
           "admission_reason,admission_failed_checks,failure,"
           "ev_attempted_expiries,ev_fitted_expiries,ev_attempted_quotes,ev_fitted_quotes,"
           "ev_worst_in_band,ev_calendar_arb_free,ev_first_invariant_failure,"
           "ev_first_failure_maturity,ev_first_failure_k,ev_first_failure_value,ev_parity_state,"
           "ev_grid_k_min,ev_grid_k_max,ev_grid_points,"
        // `att_probe_ran` is this CSV's `oracle_ran`: every counter below is 0 on
        // a digest that was never computed, which is byte-identical to a
        // validated-clean one. Read the counters only where it is 1.
        << "att_probe_ran,att_probe_source,att_probe_state,att_probe_reasons,att_probe_failures,"
           "att_probe_id_matches_served,att_probe_admission_matches,"
        << digest_header("att_")
        // T3e. Empty on the served attempt's own row and when nothing was
        // published — an absent comparison is not a zero one.
        << ",att_served_d_worst_in_band,att_served_d_fitted_quotes,"
           "att_served_d_fitted_expiries\n";
    for (const std::vector<AttemptRow> &board : attempt_rows) {
      for (const AttemptRow &a : board) {
        char head[512];
        std::snprintf(head, sizeof(head), ",%u,%u,%s,%d,%d,%s,%s,%s,%s,%d,%d,%s,%u,",
                      a.attempt_index, a.n_attempts, a.report_source, a.report_published ? 1 : 0,
                      a.report_used_fallback ? 1 : 0, a.primary_kind.c_str(),
                      a.published_kind.c_str(), a.curve_kind.c_str(), a.stage,
                      a.build_succeeded ? 1 : 0, a.admitted ? 1 : 0, a.admission_reason,
                      a.admission_failed_checks);
        char ev[256];
        std::snprintf(ev, sizeof(ev), ",%zu,%zu,%zu,%zu,%.6f,%d,%s,", a.ev_attempted_expiries,
                      a.ev_fitted_expiries, a.ev_attempted_quotes, a.ev_fitted_quotes,
                      a.ev_worst_in_band, a.ev_calendar_arb_free ? 1 : 0,
                      a.ev_first_invariant_failure);
        char ev2[192];
        std::snprintf(ev2, sizeof(ev2), ",%s,%.6g,%.6g,%zu,", a.ev_parity_state, a.ev_grid_k_min,
                      a.ev_grid_k_max, a.ev_grid_points);
        char probe[192];
        std::snprintf(probe, sizeof(probe), "%d,%s,%s,%u,%u,", a.probe_ran ? 1 : 0, a.probe_source,
                      a.probe_state.c_str(), a.probe_reasons, a.probe_failures);
        out << csv_escape(a.symbol) << head << csv_escape(a.failure) << ev
            << format_optional(a.ev_first_failure_maturity) << ','
            << format_optional(a.ev_first_failure_k) << ','
            << format_optional(a.ev_first_failure_value) << ev2 << probe
            << format_optional(a.probe_id_matches_served) << ','
            << format_optional(a.probe_admission_matches) << ',' << format_digest(a.probe_digest)
            << ',' << format_optional(a.served_d_worst_in_band) << ','
            << format_optional(a.served_d_fitted_quotes) << ','
            << format_optional(a.served_d_fitted_expiries) << '\n';
        ++n_attempt_rows;
      }
    }
  }

  // ── Per-expiry census CSV (T3c) ────────────────────────────────────────────
  //
  // One row per (symbol, attempt, chain), keyed on `symbol` + `attempt_index` so
  // it joins to the attempts CSV. `fit_outcome` is meaningful only where
  // `fit_outcome_known` is 1 — see `ExpiryRow`.
  std::size_t n_expiry_rows = 0;
  if (want_expiries) {
    std::ofstream out(expiries_csv, std::ios::trunc);
    out << "symbol,attempt_index,curve_kind,build_succeeded,chain_index,maturity,"
           "build_outcome,n_used,fit_outcome,fit_outcome_known\n";
    for (const std::vector<ExpiryRow> &board : expiry_rows) {
      for (const ExpiryRow &e : board) {
        char buf[320];
        std::snprintf(buf, sizeof(buf), ",%u,%s,%d,%zu,%.9g,%s,%zu,%s,%d\n", e.attempt_index,
                      e.curve_kind.c_str(), e.build_succeeded ? 1 : 0, e.chain_index, e.maturity,
                      e.build_outcome, e.n_used, e.fit_outcome, e.fit_outcome_known ? 1 : 0);
        out << csv_escape(e.symbol) << buf;
        ++n_expiry_rows;
      }
    }
  }

  // ── Per-served-slice census CSV (T3d) ──────────────────────────────────────
  //
  // One row per (symbol, served slice), keyed on `symbol` + `slice_index`. This
  // is the SERVED population, so a coverage change is visible here as rows
  // appearing — which is exactly what makes a board's aggregate movable without
  // any individual slice moving. `parity_scored` gates the quality columns.
  std::size_t n_slice_rows = 0;
  if (want_slices) {
    std::ofstream out(slices_csv, std::ios::trunc);
    out << "symbol,curve_kind,slice_index,T,forward,n_used,n_dropped,"
           "frac_in_band,n_scored,n_within,rmse_vol,chi2,parity_scored,"
           "essvi_params,essvi_theta,essvi_phi,essvi_rho,essvi_resid_scale\n";
    for (const std::vector<SliceRow> &board : slice_rows) {
      for (const SliceRow &s : board) {
        char buf[448];
        std::snprintf(buf, sizeof(buf),
                      ",%s,%zu,%.9g,%.9g,%zu,%zu,%.9g,%zu,%zu,%.9g,%.9g,%d,%d,%.17g,%.17g,%.17g,"
                      "%.17g\n",
                      s.curve_kind.c_str(), s.slice_index, s.T, s.forward, s.n_used, s.n_dropped,
                      s.frac_in_band, s.n_scored, s.n_within, s.rmse_vol, s.chi2,
                      s.parity_scored ? 1 : 0, s.essvi_params ? 1 : 0, s.essvi_theta, s.essvi_phi,
                      s.essvi_rho, s.essvi_resid_scale);
        out << csv_escape(s.symbol) << buf;
        ++n_slice_rows;
      }
    }
  }

  // ── Summary ────────────────────────────────────────────────────────────────
  std::map<std::string, std::size_t> by_status, by_kind, by_profile, by_error;
  std::vector<double> fit_times;
  double fit_ms_sum = 0.0;
  for (const Row &w : rows) {
    ++by_status[w.status];
    if (w.status == "ok") {
      ++by_kind[w.chosen_kind.empty() ? "(none)" : w.chosen_kind];
      ++by_profile[w.profile.empty() ? "(none)" : w.profile];
      fit_times.push_back(w.fit_ms);
      fit_ms_sum += w.fit_ms;
    } else if (!w.error.empty()) {
      ++by_error[w.error.substr(0, 90)];
    }
  }

  std::printf("\n=== universe_autofit summary ===\n");
  std::printf("fit-path: %s (%s)\n", fit_path_name, fit_config.c_str());
  std::printf("wall: load=%.1fs fit+value=%.1fs (workers=%u) | serial fit cpu=%.1fs\n",
              load_total_ms / 1e3, fit_total_ms / 1e3, fit_workers, fit_ms_sum / 1e3);
  std::printf("-- status --\n");
  for (const auto &[k, n] : by_status) std::printf("  %-16s %6zu\n", k.c_str(), n);
  std::printf("-- chosen curve family (ok boards) --\n");
  for (const auto &[k, n] : by_kind) std::printf("  %-16s %6zu\n", k.c_str(), n);
  std::printf("-- profile (ok boards) --\n");
  for (const auto &[k, n] : by_profile) std::printf("  %-22s %6zu\n", k.c_str(), n);
  std::printf("-- top errors --\n");
  {
    std::vector<std::pair<std::size_t, std::string>> errs;
    for (const auto &[msg, n] : by_error) errs.emplace_back(n, msg);
    std::sort(errs.rbegin(), errs.rend());
    const std::size_t show = errs.size() < 15 ? errs.size() : 15;
    for (std::size_t i = 0; i < show; ++i)
      std::printf("  %5zu  %s\n", errs[i].first, errs[i].second.c_str());
  }
  if (!fit_times.empty()) {
    std::sort(fit_times.begin(), fit_times.end());
    const auto pct = [&](double p) {
      return fit_times[static_cast<std::size_t>(p * (fit_times.size() - 1))];
    };
    std::printf("-- fit_ms percentiles (ok boards, n=%zu) --\n", fit_times.size());
    std::printf("  p50=%.1f p90=%.1f p99=%.1f max=%.1f mean=%.1f\n", pct(0.50), pct(0.90),
                pct(0.99), fit_times.back(), fit_ms_sum / static_cast<double>(fit_times.size()));
  }
  {
    std::vector<const Row *> slow;
    for (const Row &w : rows)
      if (w.status == "ok") slow.push_back(&w);
    std::sort(slow.begin(), slow.end(),
              [](const Row *a, const Row *b) { return a->fit_ms > b->fit_ms; });
    const std::size_t show = slow.size() < 15 ? slow.size() : 15;
    std::printf("-- slowest fits --\n");
    for (std::size_t i = 0; i < show; ++i)
      std::printf("  %-8s fit=%8.1fms rows=%6zu kind=%-14s profile=%s\n", slow[i]->symbol.c_str(),
                  slow[i]->fit_ms, slow[i]->n_rows, slow[i]->chosen_kind.c_str(),
                  slow[i]->profile.c_str());
  }
  std::printf("\nresults: %s\n", out_csv.c_str());
  if (want_attempts) {
    std::printf("attempts: %s (%zu rows)\n", attempts_csv.c_str(), n_attempt_rows);
  }
  if (want_expiries) {
    std::printf("expiries: %s (%zu rows)\n", expiries_csv.c_str(), n_expiry_rows);
  }
  if (want_slices) {
    std::printf("slices: %s (%zu rows)\n", slices_csv.c_str(), n_slice_rows);
  }
  return 0;
}
