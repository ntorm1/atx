#pragma once

// Corpus — a whole-panel archive builder: fit MANY (date, symbol) boards in
// parallel and lay the fitted surfaces out as ONE `SurfaceArchive` per date,
// indexed by a single deterministic manifest.
//
// A research/production universe is not one board — it is a grid of (date,
// symbol) volatility surfaces (a "corpus"). Each board is fit through the ONE
// blessed atx-vol path (OptionChain::from_frame -> PricerFitter::fit ->
// VolaSession::to_priced_surface), the curve family AUTO-SELECTED per board when
// the fit template leaves `PricerConfig::curve` unset (a penny-dense index board
// picks ConvexDense; a sparse single-name board picks the parsimonious eSSVI
// backbone — see curve_selector.hpp). The fits fan out ACROSS boards (each board
// itself fit single-threaded); the surfaces of one date are packed into that
// date's `SurfaceArchive` file, and a manifest indexes the whole corpus.
//
// ## Determinism
//
// The build is deterministic by construction, independent of the worker count:
// a single-threaded pre-pass fixes a stable board order, each worker owns its
// own fit scratch and writes a DISJOINT result slot (never a shared index), and
// the manifest entries are sorted (date asc, symbol asc) before any output. Two
// runs — at any thread count — produce the same manifest and the same per-date
// surfaces (fitting is a pure function of the board), so `map_symbol` reloads a
// surface that reprices bit-for-bit.
//
// ## Ownership / thread-safety
//
// `build_corpus` owns every intermediate (the move-only `PricedSurface`s live in
// a stable container for the duration of each per-date archive write). It is a
// self-contained call: pass boards + an output directory, receive the in-memory
// manifest and the on-disk archives. The manifest value types are plain
// aggregates (Rule of Zero); serialize / parse are pure functions of their input.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "atx/vol/data.hpp"            // QuoteFrame
#include "atx/vol/fit_policy.hpp"      // FitDecisionSource, FitPreset, ProfileKind
#include "atx/vol/market_env.hpp"      // MarketEnv
#include "atx/vol/pricer_fitter.hpp"   // PricerConfig
#include "atx/vol/surface_archive.hpp" // ArchiveV2WriteOpts
#include "atx/vol/types.hpp"           // Result, Status, ErrorCode
#include "atx/vol/vol_curve.hpp"       // VolCurveKind

namespace atx::vol {

// ── One board to fit ────────────────────────────────────────────────────────
//
// A single underlier's quote board tagged by valuation date + symbol. `date`
// groups boards into per-date archives (its exact string is the archive-file
// stem); `symbol` is the archive key (canonicalized inside the archive:
// ASCII-upper-cased, truncated to 32). `frame` + `env` are exactly what
// `OptionChain::from_frame` consumes.
struct CorpusBoard {
  std::string date;   // e.g. "2026-06-19"; groups boards into per-date archives
  std::string symbol; // archive key (canonicalized inside the archive)
  QuoteFrame frame;   // the board's quotes
  MarketEnv env;      // spot / rate-curve / divs / valuation time for from_frame
  // Per-board curve override. std::nullopt (the default) => this board uses the
  // config's `fit_template.curve` policy (auto-select when that is also unset).
  // Set it to PIN a specific curve family for this board (e.g. the ConvexDense
  // index recipe), independent of the rest of the corpus.
  std::optional<CurveConfig> curve{};
  FitContext fit_context{}; // event/session/HTB facts for unified routing
  bool source_provenance_complete{false};
  std::uint32_t source_schema_version{0};
  std::uint64_t source_fingerprint{0};
  std::uint64_t market_input_fingerprint{0};
};

enum class CorpusDividendTreatment : std::uint8_t {
  EscrowedForward = 0,
};

// Per-board fit outcome, mirroring calib_pool.hpp's FitStatus but kept local so
// this header carries no calibrator-pool dependency (the corpus never drives
// calibrate_pool — see corpus.cpp for why).
enum class CorpusFitStatus : std::uint8_t {
  Ok = 0,      // fit + snapshot succeeded; the surface is in its date's archive
  Failed = 1,  // chain build / fit / snapshot failed (see `error_code`)
  Skipped = 2, // the board had nothing fittable (empty frame)
};

[[nodiscard]] const char *to_string(CorpusFitStatus status) noexcept;

// A qualified corpus distinguishes a successful, admitted fit from a
// successful fit rejected by the configured quality policy. Source and fit
// failures stay distinct so no planned (date, symbol) cell silently disappears.
enum class CorpusDisposition : std::uint8_t {
  Admitted = 0,
  Quarantined = 1,
  SourceFailed = 2,
  FitFailed = 3,
  Empty = 4,
};

[[nodiscard]] const char *to_string(CorpusDisposition disposition) noexcept;

// Stable reason vocabulary for qualified-corpus reports. Numeric values are
// persisted and also index `CorpusAdmissionDecision::failed_checks`; append new
// values before Count and never reorder existing ones.
enum class CorpusAdmissionReason : std::uint8_t {
  None = 0,
  MissingSource = 1,
  InvalidSourceSchema = 2,
  AmbiguousSourceIdentity = 3,
  EmptyBoard = 4,
  FitError = 5,
  SourceProvenanceUnavailable = 6,
  QualityUnavailable = 7,
  NonFiniteMetric = 8,
  InvalidRule = 9,
  TooFewQuotes = 10,
  TooFewSlices = 11,
  TooFewHoldouts = 12,
  CalendarArbitrage = 13,
  InBandBelowFloor = 14,
  OosInBandBelowFloor = 15,
  OosVegaWeightedBelowFloor = 16,
  VolRmseAboveCeiling = 17,
  ReducedChi2AboveCeiling = 18,
  RoundTripMismatch = 19,
  MetricOutOfRange = 20,
  Count = 21,
};

[[nodiscard]] const char *to_string(CorpusAdmissionReason reason) noexcept;

// One quality record for the final curve that would be archived. Optional
// measurements are deliberate: `nullopt` means "not measured" and must never be
// serialized or interpreted as a clean zero.
struct CorpusQualityMetrics {
  ProfileKind profile{ProfileKind::OrdinarySingleName};
  FitDecisionSource decision_source{FitDecisionSource::BoardFeatures};
  FitPreset preset{FitPreset::Robust};
  VolCurveKind primary_kind{VolCurveKind::ConvexDense};
  VolCurveKind final_kind{VolCurveKind::ConvexDense};
  bool used_fallback{false};
  bool curve_pinned{false};
  bool final_kind_consistent{true};
  bool provenance_complete{false};
  std::uint32_t source_schema_version{0};
  std::uint64_t source_fingerprint{0};
  std::uint64_t market_input_fingerprint{0};
  CorpusDividendTreatment dividend_treatment{CorpusDividendTreatment::EscrowedForward};
  std::uint32_t n_cash_dividends{0};
  std::uint32_t n_raw_quotes{0};
  std::uint32_t n_two_sided{0};
  std::uint32_t n_slices{0};
  std::uint32_t n_holdout{0};
  std::uint32_t n_fit_scorable{0};
  std::uint32_t n_fit_in_band{0};
  std::uint32_t n_oos_in_band{0};
  std::optional<double> fit_in_band{};
  std::optional<double> oos_in_band{};
  std::optional<double> oos_vega_weighted{};
  std::optional<double> oos_vega_weight_in_band{};
  std::optional<double> oos_vega_weight_total{};
  std::optional<double> mean_vol_rmse{};
  std::optional<double> mean_reduced_chi2{};
  std::optional<std::uint32_t> calendar_violations{};

  [[nodiscard]] bool operator==(const CorpusQualityMetrics &) const = default;
};

// Profile-specific admission thresholds. `min_quotes` applies to two-sided
// quote observations, the population on which price-in-band is meaningful.
// An unset optional disables that numerical predicate; when set, the matching
// quality metric is required to be present and finite.
struct CorpusAdmissionRule {
  std::uint32_t min_quotes{0};
  std::uint32_t min_slices{0};
  std::uint32_t min_holdout{0};
  std::optional<double> min_fit_in_band{};
  std::optional<double> min_oos_in_band{};
  std::optional<double> min_oos_vega_weighted{};
  std::optional<double> max_mean_vol_rmse{};
  std::optional<double> max_mean_reduced_chi2{};
  bool require_calendar_arb_free{true};
  // Symmetric log-moneyness domain used for the calendar-arbitrage check.
  // 3.0 preserves the historical strict wing/extrapolation gate; ATM strategies
  // may explicitly require the documented MonotoneFit domain (0.7).
  double calendar_abs_k{3.0};
  bool require_source_provenance{false};

  [[nodiscard]] bool operator==(const CorpusAdmissionRule &) const = default;
};

struct CorpusAdmissionPolicy {
  bool enabled{false};
  std::array<CorpusAdmissionRule, kProfileKindCount> by_profile{};

  [[nodiscard]] bool operator==(const CorpusAdmissionPolicy &) const = default;
};

using CorpusAdmissionFailureMask = std::uint32_t;

// Admission returns both the deterministic primary reason and every failed
// predicate. The primary reason is stable report vocabulary; the mask prevents
// the first failure from hiding other evidence useful for remediation.
struct CorpusAdmissionDecision {
  CorpusDisposition disposition{CorpusDisposition::Quarantined};
  CorpusAdmissionReason primary_reason{CorpusAdmissionReason::QualityUnavailable};
  CorpusAdmissionFailureMask failed_checks{0};

  [[nodiscard]] bool failed(CorpusAdmissionReason reason) const noexcept;
};

// Evaluate a final fitted surface against one already-selected profile rule.
// Pure, deterministic, allocation-free, and noexcept. Invalid rule values are
// quarantined with `InvalidRule`; missing required measurements with
// `QualityUnavailable`.
[[nodiscard]] CorpusAdmissionDecision
evaluate_corpus_admission(const CorpusQualityMetrics &metrics,
                          const CorpusAdmissionRule &rule) noexcept;

// One row in the qualified-corpus sidecar. The legacy `CorpusManifest` remains
// the backtest index; this record carries the admission evidence for every
// planned cell, including cells that never reached a successful fit.
struct QualifiedCorpusEntry {
  std::string date{};
  std::string symbol{};
  CorpusDisposition disposition{CorpusDisposition::Empty};
  CorpusAdmissionReason primary_reason{CorpusAdmissionReason::EmptyBoard};
  CorpusAdmissionFailureMask failed_checks{0};
  ErrorCode source_or_fit_error{ErrorCode::Unknown};
  CorpusQualityMetrics quality{};
  std::string archive_path{};

  [[nodiscard]] bool operator==(const QualifiedCorpusEntry &) const = default;
};

// Deterministic quality sidecar. Fingerprints exclude absolute paths and timing
// values; their construction is wired by the qualified builder in a later task.
struct CorpusQualityReport {
  std::uint64_t input_fingerprint{0};
  std::uint64_t policy_fingerprint{0};
  std::vector<QualifiedCorpusEntry> entries{}; // date/symbol ascending
  std::uint32_t n_planned{0};
  std::uint32_t n_admitted{0};
  std::uint32_t n_quarantined{0};
  std::uint32_t n_source_failed{0};
  std::uint32_t n_fit_failed{0};
  std::uint32_t n_empty{0};

  [[nodiscard]] bool operator==(const CorpusQualityReport &) const = default;
};

// Versioned deterministic TSV. Optional measurements serialize as `NA`; the
// parser verifies enum domains and that aggregate counts exactly partition the
// entry rows.
[[nodiscard]] std::string serialize_quality_report(const CorpusQualityReport &report);
[[nodiscard]] Result<CorpusQualityReport> parse_quality_report(std::string_view tsv);
[[nodiscard]] Status write_quality_report_file(std::string_view path,
                                               const CorpusQualityReport &report);
[[nodiscard]] Result<CorpusQualityReport> read_quality_report_file(std::string_view path);

// ── Per-board manifest record ───────────────────────────────────────────────
//
// One row of the corpus index. `chosen_kind` / `n_slices` / `oos_in_band` are
// meaningful iff `status == Ok`; `error_code` iff `status == Failed`.
// `archive_path` names the per-date archive this surface was written to (empty
// for a non-Ok board that was not archived).
struct CorpusEntry {
  std::string date{};
  std::string symbol{};
  CorpusFitStatus status{CorpusFitStatus::Skipped};
  VolCurveKind chosen_kind{VolCurveKind::ConvexDense}; // iff Ok
  std::uint32_t n_slices{0};
  double oos_in_band{0.0};                  // chosen candidate's OOS in-band (0 if curve pinned)
  ErrorCode error_code{ErrorCode::Unknown}; // iff Failed
  std::string archive_path{};               // the per-date archive (iff archived)

  [[nodiscard]] bool operator==(const CorpusEntry &) const = default;
};

// ── Whole-corpus index ──────────────────────────────────────────────────────
//
// `dates` are unique + ascending; `entries` are sorted (date asc, symbol asc) —
// fully deterministic. The aggregate counts sum over `entries`.
struct CorpusManifest {
  std::vector<std::string> dates{};   // ascending, unique
  std::vector<CorpusEntry> entries{}; // sorted (date asc, symbol asc)
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_skipped{0};

  [[nodiscard]] bool operator==(const CorpusManifest &) const = default;
};

// ── Build policy ────────────────────────────────────────────────────────────
struct CorpusConfig {
  // The per-board fit template. `fit_template.curve` left std::nullopt (the
  // default) => the CurveSelector picks the family per board. `n_threads` on the
  // template is IGNORED — each board is fit single-threaded; parallelism is
  // across boards via `CorpusConfig::n_threads`.
  PricerConfig fit_template{};
  // Worker count for the ACROSS-board fan-out. 0 => hardware_concurrency (>= 1),
  // clamped to the board count.
  unsigned n_threads{0};
  // Options forwarded verbatim to every per-date archive write (ATXVSA2, S4).
  ArchiveV2WriteOpts write_opts{};
};

struct QualifiedCorpusConfig {
  CorpusConfig build{};
  CorpusAdmissionPolicy admission{};
  std::uint64_t input_fingerprint{0};
  std::uint64_t policy_fingerprint{0};
};

struct QualifiedCorpusManifest {
  CorpusManifest manifest{};
  CorpusQualityReport quality{};
  std::uint32_t peak_live_fitted_surfaces{0};
};

struct CorpusSourceFailure {
  std::string date{};
  std::string symbol{};
  CorpusAdmissionReason reason{CorpusAdmissionReason::MissingSource};
  ErrorCode error_code{ErrorCode::NotFound};
  std::uint32_t source_schema_version{0};
  std::uint64_t source_fingerprint{0};
  std::uint64_t market_input_fingerprint{0};
};

using CorpusCellInput = std::variant<CorpusBoard, CorpusSourceFailure>;

class CorpusBuildSession {
public:
  CorpusBuildSession(CorpusBuildSession &&) noexcept = default;
  CorpusBuildSession &operator=(CorpusBuildSession &&) noexcept = default;
  CorpusBuildSession(const CorpusBuildSession &) = delete;
  CorpusBuildSession &operator=(const CorpusBuildSession &) = delete;

  [[nodiscard]] static Result<CorpusBuildSession> create(std::string_view out_dir,
                                                         const QualifiedCorpusConfig &cfg);

  // Dates must be strictly ascending. Every cell key must match `date`, and
  // canonical symbols must be unique within the batch.
  [[nodiscard]] Status append_date(std::string_view date, std::span<const CorpusCellInput> cells);

  [[nodiscard]] Result<QualifiedCorpusManifest> finish();

private:
  CorpusBuildSession(std::string out_dir, QualifiedCorpusConfig cfg);

  std::string out_dir_{};
  QualifiedCorpusConfig cfg_{};
  std::string last_date_{};
  std::string input_fingerprint_material_{};
  CorpusManifest manifest_{};
  CorpusQualityReport quality_{};
  std::uint32_t peak_live_fitted_surfaces_{0};
  bool finished_{false};
};

// ── Driver ──────────────────────────────────────────────────────────────────

// Fit every board (parallel across boards, each board single-threaded), group
// by `date`, write ONE `SurfaceArchive` file per date into `out_dir`
// (`out_dir/<date>.atxvsa`), write the manifest (`out_dir/manifest.tsv`), and
// return the in-memory manifest. A date with zero Ok boards writes no archive
// (the archive writer rejects an empty item list); those boards are still
// recorded (Failed / Skipped). One failing board never sinks the corpus.
//
// Deterministic: identical manifest + surfaces across runs and thread counts.
//
// @return InvalidArgument if `boards` is empty or `out_dir` is empty; IoError
//         propagated from a per-date archive write or the manifest write; an
//         archive AlreadyExists (a duplicate canonical symbol within one date)
//         propagated.
[[nodiscard]] Result<CorpusManifest> build_corpus(std::span<const CorpusBoard> boards,
                                                  std::string_view out_dir,
                                                  const CorpusConfig &cfg = {});

// Fit through the same engine as `build_corpus`, but evaluate the final fitted
// surface against its profile rule before archive admission and write a complete
// `quality.tsv` sidecar. Successful quarantined fits are excluded from archives
// and appear as Failed/Unavailable in the legacy manifest; their distinct
// quality reason remains in the sidecar.
[[nodiscard]] Result<QualifiedCorpusManifest>
build_qualified_corpus(std::span<const CorpusBoard> boards, std::string_view out_dir,
                       const QualifiedCorpusConfig &cfg);

// ── Manifest serialization (deterministic TSV) ──────────────────────────────

// Serialize `m` as deterministic TSV: a magic line, an aggregate-counts line, a
// dates line, a column-header line, then one tab-separated row per entry (in
// `entries` order). `oos_in_band` is written at full round-trip precision;
// enums as their integer value. `parse(serialize(m)) == m`.
[[nodiscard]] std::string serialize_manifest(const CorpusManifest &m);

// Parse the TSV produced by `serialize_manifest`.
//
// @return ParseError on a malformed document (bad magic, short line, a
//         non-numeric numeric field, or an unknown enum value).
[[nodiscard]] Result<CorpusManifest> parse_manifest(std::string_view tsv);

// Persist / load the manifest TSV. `write_manifest_file` creates the parent
// directory if missing (atomic temp-then-rename). Both add IoError on a
// filesystem failure; `read_manifest_file` adds NotFound for a missing file.
[[nodiscard]] Status write_manifest_file(std::string_view path, const CorpusManifest &m);
[[nodiscard]] Result<CorpusManifest> read_manifest_file(std::string_view path);

} // namespace atx::vol
