#include "atx/vol/var.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/detail/pricing_executor.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/universe.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool finite_positive(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Call || side == Side::Put;
}

[[nodiscard]] bool valid_query_execution(QueryExecution execution) noexcept {
  return execution == QueryExecution::Configured || execution == QueryExecution::ColdReference;
}

[[nodiscard]] bool valid_evaluation_config(const VarEvaluationConfig &config) noexcept {
  return std::isfinite(config.delta_tolerance) && config.delta_tolerance > 0.0 &&
         config.delta_tolerance <= 1.0e-3 && valid_query_execution(config.projection_execution) &&
         valid_query_execution(config.valuation_execution) &&
         (config.projection_solve_policy == OptionDeltaSolvePolicy::Direct ||
          config.projection_solve_policy == OptionDeltaSolvePolicy::FastScreenColdConfirm ||
          config.projection_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm) &&
         std::isfinite(config.max_restrike_abs_log_moneyness) &&
         config.max_restrike_abs_log_moneyness > 0.0 &&
         (config.base_mark_source == VarBaseMarkSource::DedicatedPricePass ||
          config.base_mark_source == VarBaseMarkSource::HarvestedFromSolver);
}

// Whether THIS config may reuse the cross-sectional solver's accepted-strike
// cold price as the base mark instead of running the dedicated EvalField::Price
// pass. Deliberately permissive rather than a config rejection: a caller on a
// Direct/FastScreen policy, or one that opted valuation into a configured marks
// accelerator, keeps today's numbers instead of failing to construct.
//
// The policy clause is load-bearing beyond mere admissibility: evaluate_scenario
// and evaluate_scenario_batched both downgrade a failing scenario by rewriting
// projection_solve_policy to FastScreenColdConfirm, so a downgraded scenario
// automatically stops harvesting and prices its own marks on the scalar route.
// The downgrade path needs no knob-awareness of its own.
[[nodiscard]] bool harvest_base_marks(const VarEvaluationConfig &config) noexcept {
  return config.base_mark_source == VarBaseMarkSource::HarvestedFromSolver &&
         config.projection_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm &&
         config.valuation_execution == QueryExecution::ColdReference;
}

// I4 diagnostic_flags bits (VarLegFrame): base/shifted tenor extrapolation and
// a restrike-root early-warning band. A root already beyond
// max_restrike_abs_log_moneyness never reaches this helper -- it is rejected
// (InvalidDelta) before any frame is populated, and poison_leg zeroes
// diagnostic_flags along with everything else.
constexpr std::uint8_t kDiagBaseTenorExtrapolated = 0x1u;
constexpr std::uint8_t kDiagShiftedTenorExtrapolated = 0x2u;
constexpr std::uint8_t kDiagRestrikeNearBound = 0x4u;

// Shared by every VarLegFrame-producing path (evaluate_option_leg's scalar
// resolve and evaluate_option_leg_resolved's cross-sectional resolve both
// terminate in finish_option_leg) so aggregate-vs-retained parity and
// thread-count bit-invariance hold trivially: there is exactly one place this
// is computed.
[[nodiscard]] std::uint8_t option_diagnostic_flags(const SurfaceRef &base,
                                                   const SurfaceRef &shifted, double base_time,
                                                   double shifted_time, double log_moneyness,
                                                   double max_abs_log_moneyness) noexcept {
  std::uint8_t flags = 0;
  if (base.extrapolates_tenor(base_time)) {
    flags |= kDiagBaseTenorExtrapolated;
  }
  if (shifted.extrapolates_tenor(shifted_time)) {
    flags |= kDiagShiftedTenorExtrapolated;
  }
  if (std::isfinite(log_moneyness) && std::fabs(log_moneyness) > 0.8 * max_abs_log_moneyness) {
    flags |= kDiagRestrikeNearBound;
  }
  return flags;
}

// SurfaceDb partition keys are canonical "YYYY-MM-DD" (Clock's own contract:
// backtest.hpp's from_surface_db). Parsed into a proleptic-Gregorian day
// ordinal purely for calendar-day gap arithmetic (I1) -- never for calendar
// validity, since these keys are produced upstream, not user input. A
// malformed key (should not occur) returns 0 so a gap computed against it
// stays finite and the guard fails toward NOT skipping.
[[nodiscard]] std::int64_t session_date_ordinal(std::string_view date) noexcept {
  if (date.size() != 10u || date[4] != '-' || date[7] != '-') {
    return 0;
  }
  std::int64_t year = 0;
  std::int64_t month = 0;
  std::int64_t day = 0;
  for (std::size_t i = 0; i < 4u; ++i) {
    if (date[i] < '0' || date[i] > '9')
      return 0;
    year = year * 10 + (date[i] - '0');
  }
  for (std::size_t i = 5; i < 7u; ++i) {
    if (date[i] < '0' || date[i] > '9')
      return 0;
    month = month * 10 + (date[i] - '0');
  }
  for (std::size_t i = 8; i < 10u; ++i) {
    if (date[i] < '0' || date[i] > '9')
      return 0;
    day = day * 10 + (date[i] - '0');
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return 0;
  }
  return atx::core::time::days_from_civil(static_cast<std::int32_t>(year),
                                          static_cast<std::uint32_t>(month),
                                          static_cast<std::uint32_t>(day));
}

[[nodiscard]] std::uint64_t nonzero_hash(const void *data, std::size_t size) noexcept {
  const std::uint64_t hash = atx::core::hash_bytes(data, size);
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] std::uint64_t stock_definition_fingerprint(std::uint32_t uid) noexcept {
  const std::uint64_t words[] = {0x5354'4f43'4b00'0001ull, uid};
  return nonzero_hash(words, sizeof words);
}

[[nodiscard]] std::uint32_t checked_u32(std::size_t value) noexcept {
  return value > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
             ? std::numeric_limits<std::uint32_t>::max()
             : static_cast<std::uint32_t>(value);
}

template <class F>
void run_balanced_ranges(std::size_t count, unsigned requested_threads, F &&body) {
  if (count == 0u) {
    return;
  }
  PricingExecutor &executor = pricing_executor();
  const unsigned capacity = executor.size() + 1u;
  unsigned ranges = requested_threads == 0u ? capacity : std::min(requested_threads, capacity);
  if (ranges == 0u) {
    ranges = 1u;
  }
  if (static_cast<std::size_t>(ranges) > count) {
    ranges = static_cast<unsigned>(count);
  }
  const std::size_t quotient = count / static_cast<std::size_t>(ranges);
  const std::size_t remainder = count % static_cast<std::size_t>(ranges);
  executor.run_blocks(ranges, ranges, [&](std::size_t range_index) {
    const std::size_t lo = range_index * quotient + std::min(range_index, remainder);
    const std::size_t hi = lo + quotient + (range_index < remainder ? 1u : 0u);
    body(lo, hi);
  });
}

[[nodiscard]] VarScenarioStatus scenario_status_for(VarLegStatus status) noexcept {
  switch (status) {
  case VarLegStatus::Ok:
    return VarScenarioStatus::Ok;
  case VarLegStatus::SurfaceUnavailable:
  case VarLegStatus::ProvenanceRejected:
    return VarScenarioStatus::MarketUnavailable;
  case VarLegStatus::TimestampMismatch:
    return VarScenarioStatus::TimestampMismatch;
  case VarLegStatus::ProjectionUnavailable:
  case VarLegStatus::PricingError:
  case VarLegStatus::InvalidDelta:
  case VarLegStatus::InvalidValue:
  case VarLegStatus::ExpiredBeforeShift:
    return VarScenarioStatus::LegFailure;
  }
  return VarScenarioStatus::LegFailure;
}

void poison_leg(VarLegFrame &frame, VarLegStatus status, VarLegKind kind,
                std::uint32_t uid) noexcept {
  frame = {};
  frame.kind = kind;
  frame.status = status;
  frame.uid = uid;
  frame.units = kNaN;
  frame.base_spot = kNaN;
  frame.shifted_spot = kNaN;
  frame.base_mark = kNaN;
  frame.shifted_mark = kNaN;
  frame.base_delta = kNaN;
  frame.dollar_delta = kNaN;
  frame.base_value = kNaN;
  frame.shifted_value = kNaN;
  frame.pnl = kNaN;
  frame.strike = kNaN;
  frame.base_time_to_expiry = kNaN;
  frame.shifted_time_to_expiry = kNaN;
}

void poison_scenario(VarScenarioFrame &frame, const VarScenario &scenario, VarScenarioStatus status,
                     std::size_t n_legs) noexcept {
  frame = {};
  frame.base_ts_ns = scenario.base_ts_ns;
  frame.shifted_ts_ns = scenario.shifted_ts_ns;
  frame.status = status;
  frame.base_value = kNaN;
  frame.shifted_value = kNaN;
  frame.pnl = kNaN;
  frame.dollar_delta = kNaN;
  frame.n_failed = checked_u32(n_legs);
}

[[nodiscard]] bool admitted_risk_surface(const SurfaceProvenance *provenance) noexcept {
  return provenance != nullptr && !provenance->legacy_format &&
         provenance->purpose == SurfacePurpose::Risk && provenance->served_generation != 0u &&
         (provenance->state == SurfaceState::Healthy ||
          provenance->state == SurfaceState::Degraded);
}

[[nodiscard]] Result<std::vector<std::uint32_t>>
required_uids(std::span<const VarPosition> positions) {
  if (positions.empty()) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: empty portfolio");
  }
  std::vector<std::pair<std::uint32_t, std::string>> keyed;
  keyed.reserve(positions.size());
  for (const VarPosition &position : positions) {
    const std::string &symbol =
        std::visit([](const auto &leg) -> const std::string & { return leg.underlier; }, position);
    const std::string canonical = canonical_symbol(symbol);
    if (canonical.empty()) {
      return Err(ErrorCode::InvalidArgument, "historical VaR: empty underlier");
    }
    keyed.emplace_back(uid_for_symbol(canonical), canonical);
  }
  std::sort(keyed.begin(), keyed.end());
  for (std::size_t index = 1; index < keyed.size(); ++index) {
    if (keyed[index - 1u].first == keyed[index].first &&
        keyed[index - 1u].second != keyed[index].second) {
      return Err(ErrorCode::InvalidArgument, "historical VaR: underlier uid collision");
    }
  }
  std::vector<std::uint32_t> result;
  result.reserve(keyed.size());
  for (const auto &[uid, symbol] : keyed) {
    static_cast<void>(symbol);
    if (result.empty() || result.back() != uid) {
      result.push_back(uid);
    }
  }
  return Ok(std::move(result));
}

[[nodiscard]] Status validate_snapshot(const MarketSnapshot &snapshot,
                                       std::span<const std::uint32_t> uids,
                                       SurfaceProvenancePolicy provenance_policy) {
  for (const std::uint32_t uid : uids) {
    if (snapshot.find(uid) == nullptr) {
      return Err(ErrorCode::NotFound, "historical VaR: required surface unavailable");
    }
    if (provenance_policy == SurfaceProvenancePolicy::RequireAdmittedRisk &&
        !admitted_risk_surface(snapshot.provenance(uid))) {
      return Err(ErrorCode::InvalidArgument,
                 "historical VaR: required surface is not admitted for risk");
    }
  }
  return Ok();
}

struct SnapshotLoad {
  std::optional<MarketSnapshot> snapshot{};
  VarLegStatus failure{VarLegStatus::SurfaceUnavailable};
  // Set when `failure` stems from a load error other than a genuinely absent
  // surface (corrupt/truncated archive, I/O error) -- a structural fault the
  // caller must classify as VarScenarioStatus::ArchiveError, not
  // MarketUnavailable, regardless of VarScenarioFailurePolicy ([solver] F4).
  bool archive_error{false};
};

struct NormalizedVarLeg {
  VarLegKind kind{VarLegKind::Option};
  std::uint32_t uid{0};
  std::string underlier{};
  ProjectedMaturitySpec maturity{};
  Side side{Side::Call};
  double multiplier{1.0};
  double target_dollar_delta{0.0};
  double target_abs_delta{0.0};
  // Constant for year-fraction/calendar-day maturities; zero selects the
  // calendar-aware projection fallback.
  std::int64_t expiry_offset_ns{0};
  std::uint64_t fingerprint{0};
  // Earliest leg with the same historical pricing definition. Quantity is
  // deliberately excluded: duplicate contracts share marks/Greeks, then scale
  // independently to their own reference dollar delta.
  std::size_t pricing_leader{0u};
};

struct VarOptionGroup {
  std::uint32_t uid{0u};
  std::size_t begin{0u};
  std::size_t end{0u};
};

struct OptionAnchorKey {
  std::uint32_t uid{0u};
  ProjectedMaturitySpec maturity{};
  double target_abs_delta{0.0};
  Side side{Side::Call};
  double multiplier{0.0};

  [[nodiscard]] bool operator==(const OptionAnchorKey &) const = default;
};

struct OptionAnchorKeyHash {
  [[nodiscard]] std::size_t operator()(const OptionAnchorKey &key) const noexcept {
    std::size_t hash = static_cast<std::size_t>(key.uid);
    hash = atx::core::hash_combine(hash, static_cast<std::size_t>(key.maturity.kind));
    hash = atx::core::hash_combine(
        hash, static_cast<std::size_t>(std::bit_cast<std::uint64_t>(key.maturity.year_fraction)));
    hash = atx::core::hash_combine(hash, static_cast<std::size_t>(key.maturity.calendar_count));
    hash = atx::core::hash_combine(hash, static_cast<std::size_t>(key.maturity.expiry_ts_ns));
    hash = atx::core::hash_combine(
        hash, static_cast<std::size_t>(std::bit_cast<std::uint64_t>(key.target_abs_delta)));
    hash = atx::core::hash_combine(hash, static_cast<std::size_t>(key.side));
    return atx::core::hash_combine(
        hash, static_cast<std::size_t>(std::bit_cast<std::uint64_t>(key.multiplier)));
  }
};

[[nodiscard]] OptionAnchorKey option_anchor_key(const VarOptionPosition &position) {
  return OptionAnchorKey{uid_for_symbol(canonical_symbol(position.underlier)),
                         position.time_to_expiry, position.target_abs_delta, position.side,
                         position.multiplier};
}

[[nodiscard]] SnapshotLoad load_snapshot(const SnapshotRef &ref,
                                         std::span<const std::uint32_t> uids,
                                         const VarRunConfig &config) {
  Result<MarketSnapshot> loaded = MarketSnapshot::load(ref.archive_path, config.query_pricing_tier,
                                                       uids, config.archive_backing);
  if (!loaded) {
    // NotFound is the "archive genuinely has nothing at this uid" shape
    // (matching validate_snapshot's own NotFound -> SurfaceUnavailable
    // mapping below); anything else here (IoError, a corrupt/truncated
    // archive's ParseError, ...) is an infrastructure fault, not an absent
    // market -- classify it distinctly so ExcludeFromDistribution cannot
    // silently absorb it.
    SnapshotLoad result;
    result.failure = VarLegStatus::SurfaceUnavailable;
    result.archive_error = loaded.error().code() != ErrorCode::NotFound;
    return result;
  }
  const Status validation = validate_snapshot(*loaded, uids, config.provenance_policy);
  if (!validation) {
    SnapshotLoad result;
    result.failure = validation.error().code() == ErrorCode::NotFound
                         ? VarLegStatus::SurfaceUnavailable
                         : VarLegStatus::ProvenanceRejected;
    return result;
  }
  SnapshotLoad result;
  result.snapshot.emplace(std::move(*loaded));
  result.failure = VarLegStatus::Ok;
  return result;
}

} // namespace

struct PreparedVarPortfolio::Impl {
  std::vector<NormalizedVarLeg> legs{};
  std::vector<VarReferenceLeg> reference_legs{};
  std::vector<std::size_t> grouped_option_indices{};
  std::vector<std::size_t> option_slot_by_leg{};
  std::vector<VarOptionGroup> option_groups{};
  double reference_value{0.0};
  double reference_dollar_delta{0.0};
  std::int64_t reference_ts_ns{0};
  std::uint64_t fingerprint{0};
};

namespace {

[[nodiscard]] Result<NormalizedVarLeg> prepare_option(const VarOptionPosition &position,
                                                      const SurfaceSet &reference_surfaces,
                                                      const VarEvaluationConfig &config,
                                                      VarReferenceLeg &reference) {
  const std::string symbol = canonical_symbol(position.underlier);
  if (symbol.empty() || !valid_side(position.side) || !std::isfinite(position.quantity) ||
      position.quantity == 0.0 || !finite_positive(position.multiplier) ||
      !(std::isfinite(position.target_abs_delta) && position.target_abs_delta > 0.0 &&
        position.target_abs_delta < 1.0) ||
      position.time_to_expiry.kind == ProjectedMaturityKind::AbsoluteExpiry) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid option position");
  }
  const std::uint32_t uid = uid_for_symbol(symbol);
  const SurfaceRef surface = reference_surfaces.find(uid);
  if (surface == nullptr) {
    return Err(ErrorCode::NotFound, "historical VaR: reference option surface unavailable");
  }
  const double spot = surface.pricing().S;
  if (!finite_positive(spot) || surface.pricing().now_ts_ns <= 0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid reference option market");
  }

  OptionProjectionSpec spec;
  spec.uid = uid;
  spec.maturity = position.time_to_expiry;
  spec.strike = ProjectedStrikeSpec::delta(position.target_abs_delta);
  spec.side = position.side;
  spec.multiplier = position.multiplier;
  OptionProjectionConfig projection_config;
  projection_config.output = OptionProjectionOutput::Mark;
  projection_config.delta_tolerance = config.delta_tolerance;
  projection_config.n_threads = 1u;
  projection_config.query_execution = config.projection_execution;
  projection_config.delta_solve_policy = config.projection_solve_policy;
  ATX_TRY(ProjectedOption projected, project_option_contract(surface, spec, projection_config));
  if (!finite_positive(projected.forward) || !finite_positive(projected.definition.contract.K) ||
      !std::isfinite(projected.model_mark) || !std::isfinite(projected.achieved_delta) ||
      std::fabs(projected.achieved_delta) <= config.delta_tolerance) {
    return Err(ErrorCode::Unavailable, "historical VaR: reference option projection invalid");
  }
  const double log_moneyness = std::log(projected.definition.contract.K / projected.forward);
  const double target_dollar_delta =
      position.quantity * position.multiplier * projected.achieved_delta * spot;
  const double reference_value = position.quantity * position.multiplier * projected.model_mark;
  if (!std::isfinite(log_moneyness) || !std::isfinite(target_dollar_delta) ||
      !std::isfinite(reference_value)) {
    return Err(ErrorCode::Unavailable, "historical VaR: reference option normalization invalid");
  }

  reference.kind = VarLegKind::Option;
  reference.uid = uid;
  reference.underlier = symbol;
  reference.reference_units = position.quantity;
  reference.reference_spot = spot;
  reference.reference_mark = projected.model_mark;
  reference.reference_delta = projected.achieved_delta;
  reference.target_dollar_delta = target_dollar_delta;
  reference.target_abs_delta = position.target_abs_delta;
  reference.log_moneyness = log_moneyness;

  NormalizedVarLeg leg;
  leg.kind = VarLegKind::Option;
  leg.uid = uid;
  leg.underlier = symbol;
  leg.maturity = position.time_to_expiry;
  leg.side = position.side;
  leg.multiplier = position.multiplier;
  leg.target_dollar_delta = target_dollar_delta;
  leg.target_abs_delta = position.target_abs_delta;
  if (position.time_to_expiry.kind == ProjectedMaturityKind::YearFraction ||
      position.time_to_expiry.kind == ProjectedMaturityKind::CalendarDays) {
    leg.expiry_offset_ns = projected.definition.expiry_ts_ns - surface.pricing().now_ts_ns;
  }
  leg.fingerprint = projected.definition.fingerprint;
  return Ok(std::move(leg));
}

[[nodiscard]] Result<NormalizedVarLeg> prepare_stock(const VarStockPosition &position,
                                                     const SurfaceSet &reference_surfaces,
                                                     VarReferenceLeg &reference) {
  const std::string symbol = canonical_symbol(position.underlier);
  if (symbol.empty() || !std::isfinite(position.shares) || position.shares == 0.0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid stock position");
  }
  const std::uint32_t uid = uid_for_symbol(symbol);
  const SurfaceRef surface = reference_surfaces.find(uid);
  if (surface == nullptr) {
    return Err(ErrorCode::NotFound, "historical VaR: reference stock surface unavailable");
  }
  const double spot = surface.pricing().S;
  const double target_dollar_delta = position.shares * spot;
  if (!finite_positive(spot) || surface.pricing().now_ts_ns <= 0 ||
      !std::isfinite(target_dollar_delta)) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid reference stock market");
  }

  reference.kind = VarLegKind::Stock;
  reference.uid = uid;
  reference.underlier = symbol;
  reference.reference_units = position.shares;
  reference.reference_spot = spot;
  reference.reference_mark = spot;
  reference.reference_delta = 1.0;
  reference.target_dollar_delta = target_dollar_delta;
  reference.target_abs_delta = 0.0;
  reference.log_moneyness = 0.0;

  NormalizedVarLeg leg;
  leg.kind = VarLegKind::Stock;
  leg.uid = uid;
  leg.underlier = symbol;
  leg.multiplier = 1.0;
  leg.target_dollar_delta = target_dollar_delta;
  leg.target_abs_delta = 0.0;
  leg.fingerprint = stock_definition_fingerprint(uid);
  return Ok(std::move(leg));
}

[[nodiscard]] Result<NormalizedVarLeg>
reuse_option_anchor(const VarOptionPosition &position, const NormalizedVarLeg &prototype,
                    const VarReferenceLeg &prototype_reference, VarReferenceLeg &reference) {
  if (!std::isfinite(position.quantity) || position.quantity == 0.0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid option position");
  }
  NormalizedVarLeg leg = prototype;
  const double target_dollar_delta = position.quantity * leg.multiplier *
                                     prototype_reference.reference_delta *
                                     prototype_reference.reference_spot;
  if (!std::isfinite(target_dollar_delta)) {
    return Err(ErrorCode::Unavailable, "historical VaR: duplicate option normalization invalid");
  }
  leg.target_dollar_delta = target_dollar_delta;
  reference = prototype_reference;
  reference.reference_units = position.quantity;
  reference.target_dollar_delta = target_dollar_delta;
  return Ok(std::move(leg));
}

struct ResolvedVarContract {
  OptionContract contract{};
  std::int64_t expiry_ts_ns{0};
  double base_delta{0.0};
  // log(strike / base-surface forward at contract.T); NaN if the forward was
  // unavailable. I4's restrike wing-bound check reads this.
  double log_moneyness{0.0};
  std::uint64_t fingerprint{0};
};

[[nodiscard]] bool resolve_var_contract(const NormalizedVarLeg &leg, const SurfaceRef &base,
                                        const VarScenario &scenario,
                                        const VarEvaluationConfig &config,
                                        ResolvedVarContract &resolved) {
  ProjectedMaturitySpec maturity = leg.maturity;
  if (leg.expiry_offset_ns > 0 &&
      leg.expiry_offset_ns <= std::numeric_limits<std::int64_t>::max() - scenario.base_ts_ns) {
    const std::int64_t expiry = scenario.base_ts_ns + leg.expiry_offset_ns;
    maturity = ProjectedMaturitySpec::absolute(expiry);
  }

  OptionProjectionSpec spec;
  spec.uid = leg.uid;
  spec.maturity = maturity;
  spec.strike = ProjectedStrikeSpec::delta(leg.target_abs_delta);
  spec.side = leg.side;
  spec.multiplier = leg.multiplier;
  OptionProjectionConfig projection_config;
  projection_config.output = OptionProjectionOutput::DefinitionOnly;
  projection_config.delta_tolerance = config.delta_tolerance;
  projection_config.n_threads = 1u;
  projection_config.query_execution = config.projection_execution;
  projection_config.delta_solve_policy = config.projection_solve_policy;
  const Result<ProjectedOption> projected = project_option_contract(base, spec, projection_config);
  if (!projected) {
    return false;
  }
  resolved.contract = projected->definition.contract;
  resolved.expiry_ts_ns = projected->definition.expiry_ts_ns;
  resolved.base_delta = projected->achieved_delta;
  resolved.fingerprint = projected->definition.fingerprint;
  resolved.log_moneyness = finite_positive(projected->forward)
                               ? std::log(resolved.contract.K / projected->forward)
                               : kNaN;
  return std::isfinite(resolved.base_delta) &&
         std::fabs(std::fabs(resolved.base_delta) - leg.target_abs_delta) <= config.delta_tolerance;
}

struct VarBatchScratch {
  std::vector<double> strike{};
  std::vector<double> base_time{};
  std::vector<double> shifted_time{};
  std::vector<Side> side{};
  std::vector<double> base_iv{};
  std::vector<double> shifted_iv{};
  std::vector<double> base_price{};
  std::vector<double> shifted_price{};
  // VarBaseMarkSource::HarvestedFromSolver only: the cold American mark
  // solve_american_delta_batch already computed at each leader slot's accepted
  // strike. Filled by resolve_group_contracts_cross_sectional and read by BOTH
  // valuation routes, which is what makes aggregate-vs-retained base-mark parity
  // structural rather than coincidental. Left untouched (and unread) under
  // DedicatedPricePass.
  std::vector<double> harvested_base_price{};
  std::vector<double> base_delta{};
  std::vector<double> log_moneyness{};
  // Either-side tenor extrapolation (I3), computed once per slot while
  // base/shifted SurfaceRefs are already in scope (the group-resolution
  // loops below) so the aggregate route's per-leg finalization can fold it
  // into VarScenarioFrame::n_tenor_extrapolated without a second surface
  // lookup or materializing a VarLegFrame.
  std::vector<std::uint8_t> tenor_extrapolated{};
  std::vector<double> base_spot{};
  std::vector<double> shifted_spot{};
  std::vector<std::uint64_t> fingerprint{};
  std::vector<Status> base_status{};
  std::vector<Status> shifted_status{};
  std::vector<VarLegFrame> fallback_frames{};
  // CrossSectionalColdConfirm-only columns (unused, but resized once, when
  // the policy is Direct/FastScreenColdConfirm).
  std::vector<double> solve_t{};
  std::vector<double> solve_target{};
  std::vector<std::int64_t> solve_expiry{};
  std::vector<std::uint16_t> solve_evaluations{};
  std::vector<Status> solve_status{};
  AmericanDeltaBatchScratch delta_scratch{};
};

template <class PreparedState>
[[nodiscard]] VarBatchScratch make_var_batch_scratch(const PreparedState &impl) {
  const std::size_t count = impl.grouped_option_indices.size();
  VarBatchScratch scratch;
  scratch.strike.resize(count);
  scratch.base_time.resize(count);
  scratch.shifted_time.resize(count);
  scratch.side.resize(count);
  scratch.base_iv.resize(count);
  scratch.shifted_iv.resize(count);
  scratch.base_price.resize(count);
  scratch.shifted_price.resize(count);
  scratch.harvested_base_price.resize(count);
  scratch.base_delta.resize(count);
  scratch.log_moneyness.resize(count);
  scratch.tenor_extrapolated.resize(count);
  scratch.base_spot.resize(count);
  scratch.shifted_spot.resize(count);
  scratch.fingerprint.resize(count);
  scratch.base_status.resize(count);
  scratch.shifted_status.resize(count);
  scratch.fallback_frames.resize(impl.legs.size());
  scratch.solve_t.resize(count);
  scratch.solve_target.resize(count);
  scratch.solve_expiry.resize(count);
  scratch.solve_evaluations.resize(count);
  scratch.solve_status.resize(count);
  for (std::size_t slot = 0u; slot < count; ++slot) {
    scratch.side[slot] = impl.legs[impl.grouped_option_indices[slot]].side;
  }
  return scratch;
}

// Fill scratch.strike/base_time/base_delta/fingerprint for every leader slot
// of `group` via ONE solve_american_delta_batch call on `base` (cold, per the
// solver's own contract); scratch.solve_expiry carries each slot's resolved
// absolute expiry back to the caller, which computes shifted_time itself
// (mirrors the scalar branch below, where resolve_var_contract's caller does
// the same). Returns false when any row fails to resolve/cold-confirm --
// caller preserves the existing fallback() semantics (the whole scenario
// re-runs on the scalar path; partial group results are never salvaged) --
// true when every row in the group is resolved.
template <class PreparedState>
[[nodiscard]] bool resolve_group_contracts_cross_sectional(
    const PreparedState &impl, const VarOptionGroup &group, const SurfaceRef &base,
    const VarScenario &scenario, const VarEvaluationConfig &config, VarBatchScratch &scratch) {
  const std::size_t count = group.end - group.begin;
  for (std::size_t slot = group.begin; slot < group.end; ++slot) {
    const NormalizedVarLeg &leg = impl.legs[impl.grouped_option_indices[slot]];
    std::int64_t expiry = 0;
    if (leg.expiry_offset_ns > 0 &&
        leg.expiry_offset_ns <= std::numeric_limits<std::int64_t>::max() - scenario.base_ts_ns) {
      expiry = scenario.base_ts_ns + leg.expiry_offset_ns;
    } else {
      const Result<std::int64_t> resolved_expiry =
          resolve_projected_expiry(scenario.base_ts_ns, leg.maturity);
      if (!resolved_expiry) {
        return false;
      }
      expiry = *resolved_expiry;
    }
    const double t = static_cast<double>(expiry - scenario.base_ts_ns) / kNsPerYear;
    if (!finite_positive(t)) {
      return false;
    }
    scratch.solve_t[slot] = t;
    scratch.solve_target[slot] = leg.target_abs_delta;
    scratch.solve_expiry[slot] = expiry;
  }

  const auto doubles = [begin = group.begin, count](std::vector<double> &values) {
    return std::span<double>{values}.subspan(begin, count);
  };
  const auto const_doubles = [begin = group.begin, count](const std::vector<double> &values) {
    return std::span<const double>{values}.subspan(begin, count);
  };
  const std::span<const Side> sides =
      std::span<const Side>{scratch.side}.subspan(group.begin, count);
  const std::span<std::uint16_t> evaluations =
      std::span<std::uint16_t>{scratch.solve_evaluations}.subspan(group.begin, count);
  const std::span<Status> row_status =
      std::span<Status>{scratch.solve_status}.subspan(group.begin, count);

  // Under HarvestedFromSolver the solve additionally hands back the cold mark
  // at each accepted strike -- the value the dedicated base Price pass would
  // otherwise recompute. An empty span keeps the solver's harvest branches off
  // entirely under DedicatedPricePass.
  const std::span<double> harvested =
      harvest_base_marks(config) ? doubles(scratch.harvested_base_price) : std::span<double>{};
  const Status solved = solve_american_delta_batch(
      base, const_doubles(scratch.solve_t), sides, const_doubles(scratch.solve_target),
      config.delta_tolerance, scratch.delta_scratch, doubles(scratch.strike),
      doubles(scratch.base_delta), evaluations, row_status, harvested);
  if (!solved) {
    return false;
  }

  for (std::size_t slot = group.begin; slot < group.end; ++slot) {
    if (!row_status[slot - group.begin]) {
      return false;
    }
    const NormalizedVarLeg &leg = impl.legs[impl.grouped_option_indices[slot]];
    // Defense in depth: solve_american_delta_batch already cold-confirms
    // every accepted row to `config.delta_tolerance` internally (batch
    // passes accept at tolerance/2 against the laned delta; the scalar
    // fallback tail re-solves any unconverged row at the full tolerance
    // against the SAME scalar cold oracle CORRECTNESS GATE 1 requires -- see
    // AmericanDeltaBatchScratch's documented contract in
    // contract_projection.hpp). Re-checking here is cheap and guards the
    // gate even if that contract ever regresses.
    if (!std::isfinite(scratch.base_delta[slot]) ||
        std::fabs(std::fabs(scratch.base_delta[slot]) - leg.target_abs_delta) >
            config.delta_tolerance) {
      return false;
    }
    scratch.base_time[slot] = scratch.solve_t[slot];
    ProjectedOptionDefinition definition;
    definition.contract =
        OptionContract{leg.uid, scratch.strike[slot], scratch.solve_t[slot], leg.side};
    definition.valuation_ts_ns = scenario.base_ts_ns;
    definition.expiry_ts_ns = scratch.solve_expiry[slot];
    definition.multiplier = leg.multiplier;
    scratch.fingerprint[slot] = projected_definition_fingerprint(definition);
    // scratch.delta_scratch was just (re)sized to this group's row count by
    // solve_american_delta_batch above, so it is indexed locally from
    // group.begin -- forward[i] is the seed-time F(T) for local row i,
    // populated regardless of which internal pass converged that row.
    const double forward = scratch.delta_scratch.forward[slot - group.begin];
    scratch.log_moneyness[slot] =
        finite_positive(forward) ? std::log(scratch.strike[slot] / forward) : kNaN;
  }
  return true;
}

// Resolve+cold-confirm every slot in `group` (delegating to
// resolve_group_contracts_cross_sectional) and additionally fill
// scratch.shifted_time/base_spot/shifted_spot for its leader slots.
// evaluate_scenario_batched's cross-sectional branch and evaluate_scenario's
// cross-sectional resolution phase both call this ONE function on the same
// per-group inputs, so the aggregate and retained-leg routes resolve to the
// identical strikes -- the property this task exists to establish. Returns
// false under the same conditions resolve_group_contracts_cross_sectional
// does, and also when a resolved leg has already expired by the shifted date
// (the scalar path's ExpiredBeforeShift check, applied here at whole-group
// granularity since the caller downgrades the whole scenario on failure).
template <class PreparedState>
[[nodiscard]] bool
resolve_group_window_cross_sectional(const PreparedState &impl, const VarOptionGroup &group,
                                     const SurfaceRef &base, const SurfaceRef &shifted,
                                     const VarScenario &scenario, const VarEvaluationConfig &config,
                                     VarBatchScratch &scratch) {
  if (!resolve_group_contracts_cross_sectional(impl, group, base, scenario, config, scratch)) {
    return false;
  }
  for (std::size_t slot = group.begin; slot < group.end; ++slot) {
    const double shifted_time =
        static_cast<double>(scratch.solve_expiry[slot] - scenario.shifted_ts_ns) / kNsPerYear;
    if (!finite_positive(shifted_time)) {
      return false;
    }
    scratch.shifted_time[slot] = shifted_time;
    scratch.base_spot[slot] = base.pricing().S;
    scratch.shifted_spot[slot] = shifted.pricing().S;
    // I3, review fix round 1: both extrapolates_tenor calls read the
    // surface's already-resolved TenorDomain (O(1), no fresh curve
    // resolve) -- cheap enough to compute unconditionally here, shared by
    // BOTH the retained cross-sectional route (evaluate_scenario) and the
    // aggregate route (evaluate_scenario_batched), since they both funnel
    // through this one function.
    scratch.tenor_extrapolated[slot] = (base.extrapolates_tenor(scratch.base_time[slot]) ||
                                        shifted.extrapolates_tenor(shifted_time))
                                           ? 1u
                                           : 0u;
  }
  return true;
}

// Shared tail of evaluate_option_leg/evaluate_option_leg_resolved: prices
// scalar base/shifted marks at the already-resolved `contract`, sizes to the
// leg's target dollar delta, and fills `frame`. Neither caller has resolved
// (or re-resolves) the contract here -- that is entirely their own
// responsibility; this function trusts `contract`/`base_delta`/`fingerprint`
// as given.
//
// `harvested_base_mark`, when engaged, REPLACES this function's own base
// EvalField::Price evaluation with the cold mark the cross-sectional solver
// already produced at `contract.K` (VarBaseMarkSource::HarvestedFromSolver).
// Only evaluate_option_leg_resolved ever engages it, and only with the value
// the aggregate route reads from the same scratch slot. A non-finite engaged
// value is a PricingError exactly as a failed evaluation would be -- it never
// silently falls back to computing the mark, which would reintroduce the pass
// this knob exists to delete and hide the failure.
[[nodiscard]] VarLegStatus
finish_option_leg(const NormalizedVarLeg &leg, const SurfaceRef &base, const SurfaceRef &shifted,
                  double base_spot, double shifted_spot, const OptionContract &contract,
                  double base_delta, double shifted_time, double log_moneyness,
                  std::uint64_t fingerprint, const VarEvaluationConfig &config, VarLegFrame &frame,
                  std::optional<double> harvested_base_mark = std::nullopt) {
  double base_mark = kNaN;
  if (harvested_base_mark.has_value()) {
    base_mark = *harvested_base_mark;
  } else {
    const PricedSurface::FusedResult base_evaluation =
        base.evaluate(contract.K, contract.T, contract.side, PricedSurface::EvalField::Price, false,
                      config.valuation_execution);
    // A failed evaluation leaves base_mark NaN, so the one finiteness check
    // below covers both the Err status and the finite-status/non-finite-price
    // case the pre-harvest code tested separately.
    if (base_evaluation.status) {
      base_mark = base_evaluation.price;
    }
  }
  if (!std::isfinite(base_mark)) {
    poison_leg(frame, VarLegStatus::PricingError, leg.kind, leg.uid);
    return frame.status;
  }
  if (!std::isfinite(base_delta) || std::fabs(base_delta) <= config.delta_tolerance) {
    poison_leg(frame, VarLegStatus::InvalidDelta, leg.kind, leg.uid);
    return frame.status;
  }
  // I4: a restrike root this far into the wing is pure parametric
  // extrapolation -- no quote has ever existed there. Reject explicitly
  // (InvalidDelta) rather than silently pricing whatever the wing model says.
  // NaN-safe: a non-finite log_moneyness (forward unavailable) also rejects.
  if (!(std::fabs(log_moneyness) <= config.max_restrike_abs_log_moneyness)) {
    poison_leg(frame, VarLegStatus::InvalidDelta, leg.kind, leg.uid);
    return frame.status;
  }
  const Result<VarSizingResult> sizing = resolve_var_sizing(
      VarSizingInput{leg.target_dollar_delta, base_spot, base_delta, leg.multiplier});
  if (!sizing) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }
  if (!finite_positive(shifted_time)) {
    poison_leg(frame, VarLegStatus::ExpiredBeforeShift, leg.kind, leg.uid);
    return frame.status;
  }
  const PricedSurface::FusedResult shifted_evaluation =
      shifted.evaluate(contract.K, shifted_time, contract.side, PricedSurface::EvalField::Price,
                       false, config.valuation_execution);
  if (!shifted_evaluation.status || !std::isfinite(shifted_evaluation.price)) {
    poison_leg(frame, VarLegStatus::PricingError, leg.kind, leg.uid);
    return frame.status;
  }

  const double base_value = sizing->units * leg.multiplier * base_mark;
  const double shifted_value = sizing->units * leg.multiplier * shifted_evaluation.price;
  const double pnl = shifted_value - base_value;
  const double allowed_error =
      config.delta_tolerance * std::max(1.0, std::fabs(leg.target_dollar_delta));
  if (!std::isfinite(base_value) || !std::isfinite(shifted_value) || !std::isfinite(pnl) ||
      std::fabs(sizing->achieved_dollar_delta - leg.target_dollar_delta) > allowed_error) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }

  frame = {};
  frame.kind = VarLegKind::Option;
  frame.status = VarLegStatus::Ok;
  frame.uid = leg.uid;
  frame.units = sizing->units;
  frame.base_spot = base_spot;
  frame.shifted_spot = shifted_spot;
  frame.base_mark = base_mark;
  frame.shifted_mark = shifted_evaluation.price;
  frame.base_delta = base_delta;
  frame.dollar_delta = sizing->achieved_dollar_delta;
  frame.base_value = base_value;
  frame.shifted_value = shifted_value;
  frame.pnl = pnl;
  frame.strike = contract.K;
  frame.base_time_to_expiry = contract.T;
  frame.shifted_time_to_expiry = shifted_time;
  frame.definition_fingerprint = fingerprint;
  frame.diagnostic_flags =
      option_diagnostic_flags(base, shifted, contract.T, shifted_time, log_moneyness,
                              config.max_restrike_abs_log_moneyness);
  return frame.status;
}

[[nodiscard]] VarLegStatus evaluate_option_leg(const NormalizedVarLeg &leg,
                                               const VarScenario &scenario,
                                               const VarEvaluationConfig &config,
                                               VarLegFrame &frame) {
  const SurfaceRef base = scenario.base_surfaces->find(leg.uid);
  const SurfaceRef shifted = scenario.shifted_surfaces->find(leg.uid);
  if (base == nullptr || shifted == nullptr) {
    poison_leg(frame, VarLegStatus::SurfaceUnavailable, leg.kind, leg.uid);
    return frame.status;
  }
  if (base.pricing().now_ts_ns != scenario.base_ts_ns ||
      shifted.pricing().now_ts_ns != scenario.shifted_ts_ns) {
    poison_leg(frame, VarLegStatus::TimestampMismatch, leg.kind, leg.uid);
    return frame.status;
  }
  const double base_spot = base.pricing().S;
  const double shifted_spot = shifted.pricing().S;
  if (!finite_positive(base_spot) || !finite_positive(shifted_spot)) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }

  ResolvedVarContract resolved;
  if (!resolve_var_contract(leg, base, scenario, config, resolved)) {
    poison_leg(frame, VarLegStatus::ProjectionUnavailable, leg.kind, leg.uid);
    return frame.status;
  }
  const double shifted_time =
      static_cast<double>(resolved.expiry_ts_ns - scenario.shifted_ts_ns) / kNsPerYear;
  return finish_option_leg(leg, base, shifted, base_spot, shifted_spot, resolved.contract,
                           resolved.base_delta, shifted_time, resolved.log_moneyness,
                           resolved.fingerprint, config, frame);
}

// Retained-leg counterpart to evaluate_scenario_batched's aggregate path
// under CrossSectionalColdConfirm: consumes the SAME resolved strike/
// base_time/shifted_time/base_delta/fingerprint slot data that
// resolve_group_window_cross_sectional already produced for this leg's
// group, so the two routes price the identical contract. There is no
// internal resolve_var_contract call -- the market/timestamp/spot guards and
// the cross-sectional cold-confirm already ran once per group in
// evaluate_scenario's resolution phase before this is ever reached.
[[nodiscard]] VarLegStatus
evaluate_option_leg_resolved(const NormalizedVarLeg &leg, double strike, double base_time,
                             double shifted_time, double base_delta, double log_moneyness,
                             std::uint64_t fingerprint, const VarScenario &scenario,
                             const VarEvaluationConfig &config, VarLegFrame &frame,
                             std::optional<double> harvested_base_mark = std::nullopt) {
  const SurfaceRef base = scenario.base_surfaces->find(leg.uid);
  const SurfaceRef shifted = scenario.shifted_surfaces->find(leg.uid);
  const OptionContract contract{leg.uid, strike, base_time, leg.side};
  return finish_option_leg(leg, base, shifted, base.pricing().S, shifted.pricing().S, contract,
                           base_delta, shifted_time, log_moneyness, fingerprint, config, frame,
                           harvested_base_mark);
}

[[nodiscard]] VarLegStatus reuse_option_pricing(const NormalizedVarLeg &leg,
                                                const VarLegFrame &leader,
                                                const VarEvaluationConfig &config,
                                                VarLegFrame &frame) noexcept {
  if (leader.status != VarLegStatus::Ok) {
    poison_leg(frame, leader.status, leg.kind, leg.uid);
    return frame.status;
  }
  const Result<VarSizingResult> sizing = resolve_var_sizing(
      VarSizingInput{leg.target_dollar_delta, leader.base_spot, leader.base_delta, leg.multiplier});
  if (!sizing) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }
  const double base_value = sizing->units * leg.multiplier * leader.base_mark;
  const double shifted_value = sizing->units * leg.multiplier * leader.shifted_mark;
  const double pnl = shifted_value - base_value;
  const double allowed_error =
      config.delta_tolerance * std::max(1.0, std::fabs(leg.target_dollar_delta));
  if (!std::isfinite(base_value) || !std::isfinite(shifted_value) || !std::isfinite(pnl) ||
      std::fabs(sizing->achieved_dollar_delta - leg.target_dollar_delta) > allowed_error) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }

  frame = leader;
  frame.units = sizing->units;
  frame.dollar_delta = sizing->achieved_dollar_delta;
  frame.base_value = base_value;
  frame.shifted_value = shifted_value;
  frame.pnl = pnl;
  return frame.status;
}

[[nodiscard]] VarLegStatus evaluate_stock_leg(const NormalizedVarLeg &leg,
                                              const VarScenario &scenario,
                                              VarLegFrame &frame) noexcept {
  const SurfaceRef base = scenario.base_surfaces->find(leg.uid);
  const SurfaceRef shifted = scenario.shifted_surfaces->find(leg.uid);
  if (base == nullptr || shifted == nullptr) {
    poison_leg(frame, VarLegStatus::SurfaceUnavailable, leg.kind, leg.uid);
    return frame.status;
  }
  if (base.pricing().now_ts_ns != scenario.base_ts_ns ||
      shifted.pricing().now_ts_ns != scenario.shifted_ts_ns) {
    poison_leg(frame, VarLegStatus::TimestampMismatch, leg.kind, leg.uid);
    return frame.status;
  }
  const double base_spot = base.pricing().S;
  const double shifted_spot = shifted.pricing().S;
  if (!finite_positive(base_spot) || !finite_positive(shifted_spot)) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }
  const Result<VarSizingResult> sizing =
      resolve_var_sizing(VarSizingInput{leg.target_dollar_delta, base_spot, 1.0, 1.0});
  if (!sizing) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }
  const double base_value = sizing->units * base_spot;
  const double shifted_value = sizing->units * shifted_spot;
  const double pnl = shifted_value - base_value;
  if (!std::isfinite(base_value) || !std::isfinite(shifted_value) || !std::isfinite(pnl)) {
    poison_leg(frame, VarLegStatus::InvalidValue, leg.kind, leg.uid);
    return frame.status;
  }

  frame = {};
  frame.kind = VarLegKind::Stock;
  frame.status = VarLegStatus::Ok;
  frame.uid = leg.uid;
  frame.units = sizing->units;
  frame.base_spot = base_spot;
  frame.shifted_spot = shifted_spot;
  frame.base_mark = base_spot;
  frame.shifted_mark = shifted_spot;
  frame.base_delta = 1.0;
  frame.dollar_delta = sizing->achieved_dollar_delta;
  frame.base_value = base_value;
  frame.shifted_value = shifted_value;
  frame.pnl = pnl;
  frame.definition_fingerprint = leg.fingerprint;
  return frame.status;
}

template <class PreparedState>
void evaluate_scenario(const PreparedState &impl, const VarScenario &scenario,
                       VarScenarioFrame &frame, std::span<VarLegFrame> leg_frames,
                       const VarEvaluationConfig &config,
                       VarBatchScratch *batch_scratch = nullptr) {
  const bool cross_sectional =
      batch_scratch != nullptr &&
      config.projection_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  // Harvesting rides entirely on the cross-sectional resolution below, so the
  // downgrade `evaluate_scenario(..., fallback_config)` recursion -- which
  // rewrites the policy to FastScreenColdConfirm and passes no scratch -- lands
  // here with both flags false and prices its own marks.
  const bool harvest = cross_sectional && harvest_base_marks(config);
  if (cross_sectional) {
    for (const VarOptionGroup &group : impl.option_groups) {
      const SurfaceRef base = scenario.base_surfaces->find(group.uid);
      const SurfaceRef shifted = scenario.shifted_surfaces->find(group.uid);
      const bool resolved = base != nullptr && shifted != nullptr &&
                            base.pricing().now_ts_ns == scenario.base_ts_ns &&
                            shifted.pricing().now_ts_ns == scenario.shifted_ts_ns &&
                            finite_positive(base.pricing().S) &&
                            finite_positive(shifted.pricing().S) &&
                            resolve_group_window_cross_sectional(impl, group, base, shifted,
                                                                 scenario, config, *batch_scratch);
      if (!resolved) {
        // SAFETY: mirrors evaluate_scenario_batched's fallback() (Task 5) --
        // CrossSectionalColdConfirm has no scalar analog in
        // resolve_var_contract/project_option_contract (treated identically
        // to FastScreenColdConfirm for a batch of one, per
        // OptionDeltaSolvePolicy's own docs in contract_projection.hpp). ANY
        // group failing to resolve/cold-confirm, or expiring before the
        // shifted date, downgrades the WHOLE scenario to the scalar
        // FastScreenColdConfirm route so the aggregate and retained-leg
        // routes report identical per-leg statuses on failure. This re-runs
        // every leg from scratch; it never touches a frame this call already
        // wrote for an earlier group.
        VarEvaluationConfig fallback_config = config;
        fallback_config.projection_solve_policy = OptionDeltaSolvePolicy::FastScreenColdConfirm;
        evaluate_scenario(impl, scenario, frame, leg_frames, fallback_config);
        return;
      }
    }
  }

  frame = {};
  frame.base_ts_ns = scenario.base_ts_ns;
  frame.shifted_ts_ns = scenario.shifted_ts_ns;
  frame.status = VarScenarioStatus::Ok;
  std::size_t aggregate_fingerprint = static_cast<std::size_t>(impl.fingerprint);
  VarScenarioStatus failure_status = VarScenarioStatus::Ok;

  for (std::size_t index = 0; index < impl.legs.size(); ++index) {
    const NormalizedVarLeg &leg = impl.legs[index];
    VarLegFrame &leg_frame = leg_frames[index];
    VarLegStatus status = VarLegStatus::Ok;
    if (leg.kind == VarLegKind::Option && leg.pricing_leader != index) {
      status = reuse_option_pricing(leg, leg_frames[leg.pricing_leader], config, leg_frame);
    } else if (leg.kind == VarLegKind::Option) {
      if (cross_sectional) {
        const std::size_t slot = impl.option_slot_by_leg[index];
        // The SAME scratch slot evaluate_scenario_batched reads for this leg,
        // so aggregate-vs-retained base-mark parity is by construction here.
        status = evaluate_option_leg_resolved(
            leg, batch_scratch->strike[slot], batch_scratch->base_time[slot],
            batch_scratch->shifted_time[slot], batch_scratch->base_delta[slot],
            batch_scratch->log_moneyness[slot], batch_scratch->fingerprint[slot], scenario, config,
            leg_frame,
            harvest ? std::optional<double>{batch_scratch->harvested_base_price[slot]}
                    : std::nullopt);
      } else {
        status = evaluate_option_leg(leg, scenario, config, leg_frame);
      }
    } else {
      status = evaluate_stock_leg(leg, scenario, leg_frame);
    }
    if (status != VarLegStatus::Ok) {
      ++frame.n_failed;
      if (failure_status == VarScenarioStatus::Ok) {
        failure_status = scenario_status_for(status);
      }
      continue;
    }
    ++frame.n_ok;
    frame.base_value += leg_frame.base_value;
    frame.shifted_value += leg_frame.shifted_value;
    frame.pnl += leg_frame.pnl;
    frame.dollar_delta += leg_frame.dollar_delta;
    if ((leg_frame.diagnostic_flags &
         (kDiagBaseTenorExtrapolated | kDiagShiftedTenorExtrapolated)) != 0u) {
      ++frame.n_tenor_extrapolated;
    }
    aggregate_fingerprint =
        atx::core::hash_combine(aggregate_fingerprint, leg_frame.definition_fingerprint);
  }
  if (frame.n_failed != 0u) {
    frame.status = failure_status;
    frame.base_value = kNaN;
    frame.shifted_value = kNaN;
    frame.pnl = kNaN;
    frame.dollar_delta = kNaN;
    return;
  }
  frame.definition_fingerprint = static_cast<std::uint64_t>(aggregate_fingerprint);
  if (frame.definition_fingerprint == 0u) {
    frame.definition_fingerprint = 1u;
  }
}

template <class PreparedState>
void evaluate_scenario_batched(const PreparedState &impl, const VarScenario &scenario,
                               VarScenarioFrame &frame, VarBatchScratch &scratch,
                               const VarEvaluationConfig &config) {
  const auto fallback = [&] {
    // SAFETY: CrossSectionalColdConfirm has no scalar analog in
    // evaluate_scenario/resolve_var_contract -- the scalar
    // project_option_contract entry point treats it identically to
    // FastScreenColdConfirm for a batch of one (OptionDeltaSolvePolicy's own
    // docs in contract_projection.hpp). This downgrade executes ONLY on the
    // failure path (a group-solve or market-availability failure for this
    // scenario, status granularity only); it re-derives the same per-leg
    // VarLegStatus values FastScreenColdConfirm would have produced and never
    // touches the accepted route's own results or statuses.
    VarEvaluationConfig fallback_config = config;
    if (fallback_config.projection_solve_policy ==
        OptionDeltaSolvePolicy::CrossSectionalColdConfirm) {
      fallback_config.projection_solve_policy = OptionDeltaSolvePolicy::FastScreenColdConfirm;
    }
    evaluate_scenario(impl, scenario, frame, scratch.fallback_frames, fallback_config);
  };
  constexpr auto price_field = PricedSurface::EvalField::Price;
  const bool cross_sectional =
      config.projection_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  // Same predicate the retained-leg route uses, and likewise false inside the
  // fallback() downgrade (which rewrites the policy before recursing).
  const bool harvest = cross_sectional && harvest_base_marks(config);
  for (const VarOptionGroup &group : impl.option_groups) {
    const SurfaceRef base = scenario.base_surfaces->find(group.uid);
    const SurfaceRef shifted = scenario.shifted_surfaces->find(group.uid);
    if (base == nullptr || shifted == nullptr || base.pricing().now_ts_ns != scenario.base_ts_ns ||
        shifted.pricing().now_ts_ns != scenario.shifted_ts_ns ||
        !finite_positive(base.pricing().S) || !finite_positive(shifted.pricing().S)) {
      fallback();
      return;
    }
    if (cross_sectional) {
      if (!resolve_group_window_cross_sectional(impl, group, base, shifted, scenario, config,
                                                scratch)) {
        fallback();
        return;
      }
    } else {
      for (std::size_t slot = group.begin; slot < group.end; ++slot) {
        const NormalizedVarLeg &leg = impl.legs[impl.grouped_option_indices[slot]];
        ResolvedVarContract resolved;
        if (!resolve_var_contract(leg, base, scenario, config, resolved)) {
          fallback();
          return;
        }
        const double shifted_time =
            static_cast<double>(resolved.expiry_ts_ns - scenario.shifted_ts_ns) / kNsPerYear;
        if (!finite_positive(shifted_time)) {
          fallback();
          return;
        }
        scratch.strike[slot] = resolved.contract.K;
        scratch.base_time[slot] = resolved.contract.T;
        scratch.shifted_time[slot] = shifted_time;
        scratch.base_spot[slot] = base.pricing().S;
        scratch.shifted_spot[slot] = shifted.pricing().S;
        scratch.base_delta[slot] = resolved.base_delta;
        scratch.log_moneyness[slot] = resolved.log_moneyness;
        // I3, review fix round 1: same either-side definition as
        // resolve_group_window_cross_sectional's cross-sectional branch and
        // finish_option_leg's retained-route flags -- base/shifted are
        // already resolved SurfaceRefs in this scope, so this is not a
        // fresh surface lookup.
        scratch.tenor_extrapolated[slot] = (base.extrapolates_tenor(resolved.contract.T) ||
                                            shifted.extrapolates_tenor(shifted_time))
                                               ? 1u
                                               : 0u;
        scratch.fingerprint[slot] = resolved.fingerprint;
      }
    }

    const std::size_t count = group.end - group.begin;
    const auto doubles = [begin = group.begin, count](std::vector<double> &values) {
      return std::span<double>{values}.subspan(begin, count);
    };
    const auto const_doubles = [begin = group.begin, count](const std::vector<double> &values) {
      return std::span<const double>{values}.subspan(begin, count);
    };
    const std::span<const Side> sides{scratch.side.data() + group.begin, count};
    if (harvest) {
      // [perf] F1: the dedicated base mark pass is DELETED here, not merely
      // shortened -- resolve_group_window_cross_sectional's solve already
      // produced the cold mark at every accepted strike. The leg loop below
      // reads scratch.base_price/base_status exactly as it does under
      // DedicatedPricePass, so only the source of those two columns changes.
      for (std::size_t slot = group.begin; slot < group.end; ++slot) {
        scratch.base_price[slot] = scratch.harvested_base_price[slot];
        // A row whose harvest failed carries NaN; stamping Ok here and letting
        // the leg loop's own finiteness check demote it to PricingError keeps
        // this route's status derivation identical to the dedicated pass's.
        scratch.base_status[slot] = Ok();
        // base_iv is dead data on this route (see the M8 note below) and the
        // solver does not hand its per-row IV back, so it is poisoned rather
        // than left holding a previous scenario's value.
        scratch.base_iv[slot] = kNaN;
      }
    } else {
      // [proj] M8 (deferred-minor fixed): the base and shifted mark passes
      // each get their own IV scratch column now -- IV is otherwise dead data
      // here (never read into any leg/frame field), but aliasing the two
      // passes onto scratch.shifted_iv was a trap for a future reader who
      // starts consuming base IVs.
      PricedSurface::EvaluationSoA base_output;
      base_output.iv = doubles(scratch.base_iv);
      base_output.price = doubles(scratch.base_price);
      base_output.status = std::span<Status>{scratch.base_status}.subspan(group.begin, count);
      const Status base_batch = base.evaluate_batch(
          const_doubles(scratch.strike), const_doubles(scratch.base_time), sides, price_field,
          false, base_output, simd::SimdIsa::Auto, config.valuation_execution);
      if (!base_batch) {
        fallback();
        return;
      }
    }

    PricedSurface::EvaluationSoA shifted_output;
    shifted_output.iv = doubles(scratch.shifted_iv);
    shifted_output.price = doubles(scratch.shifted_price);
    shifted_output.status = std::span<Status>{scratch.shifted_status}.subspan(group.begin, count);
    const Status shifted_batch = shifted.evaluate_batch(
        const_doubles(scratch.strike), const_doubles(scratch.shifted_time), sides, price_field,
        false, shifted_output, simd::SimdIsa::Auto, config.valuation_execution);
    if (!shifted_batch) {
      fallback();
      return;
    }
  }

  frame = {};
  frame.base_ts_ns = scenario.base_ts_ns;
  frame.shifted_ts_ns = scenario.shifted_ts_ns;
  frame.status = VarScenarioStatus::Ok;
  std::size_t aggregate_fingerprint = static_cast<std::size_t>(impl.fingerprint);
  VarScenarioStatus failure_status = VarScenarioStatus::Ok;
  for (std::size_t index = 0u; index < impl.legs.size(); ++index) {
    const NormalizedVarLeg &leg = impl.legs[index];
    VarLegStatus status = VarLegStatus::Ok;
    double base_value = 0.0;
    double shifted_value = 0.0;
    double pnl = 0.0;
    double dollar_delta = 0.0;
    std::uint64_t fingerprint = 0u;
    bool tenor_extrapolated = false;
    if (leg.kind == VarLegKind::Stock) {
      VarLegFrame stock_frame;
      status = evaluate_stock_leg(leg, scenario, stock_frame);
      if (status == VarLegStatus::Ok) {
        base_value = stock_frame.base_value;
        shifted_value = stock_frame.shifted_value;
        pnl = stock_frame.pnl;
        dollar_delta = stock_frame.dollar_delta;
        fingerprint = stock_frame.definition_fingerprint;
      }
    } else {
      const std::size_t slot = impl.option_slot_by_leg[index];
      // Read regardless of this leg's eventual status -- it is only USED
      // below once status == Ok is confirmed, and every resolved slot's
      // scratch.tenor_extrapolated was already populated in the group loop
      // above (review fix round 1).
      tenor_extrapolated = scratch.tenor_extrapolated[slot] != 0u;
      if (!scratch.base_status[slot] || !scratch.shifted_status[slot] ||
          !std::isfinite(scratch.base_price[slot]) || !std::isfinite(scratch.shifted_price[slot])) {
        status = VarLegStatus::PricingError;
      } else if (!std::isfinite(scratch.base_delta[slot]) ||
                 std::fabs(scratch.base_delta[slot]) <= config.delta_tolerance) {
        status = VarLegStatus::InvalidDelta;
      } else if (!(std::fabs(scratch.log_moneyness[slot]) <=
                   config.max_restrike_abs_log_moneyness)) {
        // I4: mirrors finish_option_leg's wing-bound rejection so the
        // aggregate route's per-scenario status/n_ok/n_failed/value totals
        // stay in parity with the retained-leg route (NaN-safe: see there).
        status = VarLegStatus::InvalidDelta;
      } else {
        const Result<VarSizingResult> sizing =
            resolve_var_sizing(VarSizingInput{leg.target_dollar_delta, scratch.base_spot[slot],
                                              scratch.base_delta[slot], leg.multiplier});
        if (!sizing) {
          status = VarLegStatus::InvalidValue;
        } else {
          dollar_delta = sizing->achieved_dollar_delta;
          base_value = sizing->units * leg.multiplier * scratch.base_price[slot];
          shifted_value = sizing->units * leg.multiplier * scratch.shifted_price[slot];
        }
        pnl = shifted_value - base_value;
        const double allowed_error =
            config.delta_tolerance * std::max(1.0, std::fabs(leg.target_dollar_delta));
        if (status == VarLegStatus::Ok &&
            (!std::isfinite(dollar_delta) || !std::isfinite(base_value) ||
             !std::isfinite(shifted_value) || !std::isfinite(pnl) ||
             std::fabs(dollar_delta - leg.target_dollar_delta) > allowed_error)) {
          status = VarLegStatus::InvalidValue;
        }
        fingerprint = scratch.fingerprint[slot];
      }
    }
    if (status != VarLegStatus::Ok) {
      ++frame.n_failed;
      if (failure_status == VarScenarioStatus::Ok) {
        failure_status = scenario_status_for(status);
      }
      continue;
    }
    ++frame.n_ok;
    frame.base_value += base_value;
    frame.shifted_value += shifted_value;
    frame.pnl += pnl;
    frame.dollar_delta += dollar_delta;
    if (tenor_extrapolated) {
      ++frame.n_tenor_extrapolated;
    }
    aggregate_fingerprint = atx::core::hash_combine(aggregate_fingerprint, fingerprint);
  }
  if (frame.n_failed != 0u) {
    frame.status = failure_status;
    frame.base_value = kNaN;
    frame.shifted_value = kNaN;
    frame.pnl = kNaN;
    frame.dollar_delta = kNaN;
    return;
  }
  frame.definition_fingerprint = static_cast<std::uint64_t>(aggregate_fingerprint);
  if (frame.definition_fingerprint == 0u) {
    frame.definition_fingerprint = 1u;
  }
}

// `scenario_status_override`, when set, replaces the scenario status
// scenario_status_for(status) would otherwise derive. Needed for
// VarScenarioStatus::ArchiveError ([solver] F4): an archive load failure has
// no dedicated VarLegStatus of its own (legs still poison as
// SurfaceUnavailable, economically the closest existing leg status), but the
// SCENARIO must be distinguishable from an ordinary MarketUnavailable so
// VarScenarioFailurePolicy::ExcludeFromDistribution cannot silently absorb
// it.
void fill_loaded_failure(
    std::span<const VarReferenceLeg> reference_legs, VarScenarioFrame &frame,
    std::span<VarLegFrame> output, VarLegStatus status, std::int64_t base_ts_ns,
    std::int64_t shifted_ts_ns,
    std::optional<VarScenarioStatus> scenario_status_override = std::nullopt) noexcept {
  VarScenario scenario;
  scenario.base_ts_ns = base_ts_ns;
  scenario.shifted_ts_ns = shifted_ts_ns;
  poison_scenario(frame, scenario, scenario_status_override.value_or(scenario_status_for(status)),
                  reference_legs.size());
  for (std::size_t index = 0; index < output.size(); ++index) {
    poison_leg(output[index], status, reference_legs[index].kind, reference_legs[index].uid);
  }
}

} // namespace

Result<VarSizingResult> resolve_var_sizing(const VarSizingInput &input) {
  if (!std::isfinite(input.target_dollar_delta) || !finite_positive(input.spot) ||
      !std::isfinite(input.unit_delta) || input.unit_delta == 0.0 ||
      std::fabs(input.unit_delta) > 1.0 || !finite_positive(input.multiplier)) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid dollar-delta sizing input");
  }
  const double denominator = input.multiplier * input.unit_delta * input.spot;
  const double units = input.target_dollar_delta / denominator;
  const double achieved_dollar_delta = units * denominator;
  if (!std::isfinite(denominator) || denominator == 0.0 || !std::isfinite(units) ||
      !std::isfinite(achieved_dollar_delta)) {
    return Err(ErrorCode::OutOfRange, "historical VaR: dollar-delta sizing overflow");
  }
  return Ok(VarSizingResult{units, achieved_dollar_delta});
}

const char *to_string(VarLegStatus status) noexcept {
  switch (status) {
  case VarLegStatus::Ok:
    return "Ok";
  case VarLegStatus::SurfaceUnavailable:
    return "SurfaceUnavailable";
  case VarLegStatus::TimestampMismatch:
    return "TimestampMismatch";
  case VarLegStatus::ProjectionUnavailable:
    return "ProjectionUnavailable";
  case VarLegStatus::PricingError:
    return "PricingError";
  case VarLegStatus::InvalidDelta:
    return "InvalidDelta";
  case VarLegStatus::InvalidValue:
    return "InvalidValue";
  case VarLegStatus::ProvenanceRejected:
    return "ProvenanceRejected";
  case VarLegStatus::ExpiredBeforeShift:
    return "ExpiredBeforeShift";
  }
  return "Unknown";
}

const char *to_string(VarScenarioStatus status) noexcept {
  switch (status) {
  case VarScenarioStatus::Ok:
    return "Ok";
  case VarScenarioStatus::MarketUnavailable:
    return "MarketUnavailable";
  case VarScenarioStatus::TimestampMismatch:
    return "TimestampMismatch";
  case VarScenarioStatus::LegFailure:
    return "LegFailure";
  case VarScenarioStatus::ArchiveError:
    return "ArchiveError";
  }
  return "Unknown";
}

PreparedVarPortfolio::PreparedVarPortfolio() : impl_(std::make_unique<Impl>()) {}
PreparedVarPortfolio::~PreparedVarPortfolio() = default;
PreparedVarPortfolio::PreparedVarPortfolio(PreparedVarPortfolio &&) noexcept = default;
PreparedVarPortfolio &PreparedVarPortfolio::operator=(PreparedVarPortfolio &&) noexcept = default;

Result<PreparedVarPortfolio> PreparedVarPortfolio::create(std::span<const VarPosition> positions,
                                                          const SurfaceSet &reference_surfaces,
                                                          const VarEvaluationConfig &config) {
  if (positions.empty() || !valid_evaluation_config(config) ||
      positions.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid portfolio/config");
  }
  PreparedVarPortfolio prepared;
  prepared.impl_->legs.reserve(positions.size());
  prepared.impl_->reference_legs.reserve(positions.size());
  std::vector<std::pair<std::uint32_t, std::string>> uid_symbols;
  uid_symbols.reserve(positions.size());
  std::vector<std::uint64_t> fingerprint_words;
  fingerprint_words.reserve(positions.size() * 4u);
  std::unordered_map<OptionAnchorKey, std::size_t, OptionAnchorKeyHash> option_anchors;
  option_anchors.reserve(positions.size());

  for (std::size_t position_index = 0u; position_index < positions.size(); ++position_index) {
    const VarPosition &position = positions[position_index];
    VarReferenceLeg reference;
    Result<NormalizedVarLeg> normalized =
        Err(ErrorCode::Internal, "historical VaR: position normalization not attempted");
    if (const auto *option = std::get_if<VarOptionPosition>(&position); option != nullptr) {
      const OptionAnchorKey key = option_anchor_key(*option);
      const auto prototype = option_anchors.find(key);
      if (prototype == option_anchors.end()) {
        normalized = prepare_option(*option, reference_surfaces, config, reference);
        if (normalized) {
          normalized->pricing_leader = position_index;
          option_anchors.emplace(key, position_index);
        }
      } else {
        normalized =
            reuse_option_anchor(*option, prepared.impl_->legs[prototype->second],
                                prepared.impl_->reference_legs[prototype->second], reference);
      }
    } else {
      normalized =
          prepare_stock(std::get<VarStockPosition>(position), reference_surfaces, reference);
      if (normalized) {
        normalized->pricing_leader = position_index;
      }
    }
    if (!normalized) {
      return Err(normalized.error());
    }
    const SurfaceRef surface = reference_surfaces.find(normalized->uid);
    if (surface == nullptr) {
      return Err(ErrorCode::NotFound, "historical VaR: normalized reference surface unavailable");
    }
    const std::int64_t timestamp = surface.pricing().now_ts_ns;
    if (prepared.impl_->reference_ts_ns == 0) {
      prepared.impl_->reference_ts_ns = timestamp;
    } else if (prepared.impl_->reference_ts_ns != timestamp) {
      return Err(ErrorCode::InvalidArgument,
                 "historical VaR: reference surfaces disagree on timestamp");
    }
    uid_symbols.emplace_back(normalized->uid, normalized->underlier);
    prepared.impl_->reference_value +=
        reference.reference_units * reference.reference_mark * normalized->multiplier;
    prepared.impl_->reference_dollar_delta += reference.target_dollar_delta;
    fingerprint_words.push_back(normalized->fingerprint);
    fingerprint_words.push_back(std::bit_cast<std::uint64_t>(normalized->target_dollar_delta));
    fingerprint_words.push_back(std::bit_cast<std::uint64_t>(normalized->target_abs_delta));
    fingerprint_words.push_back(static_cast<std::uint64_t>(normalized->kind));
    prepared.impl_->reference_legs.push_back(std::move(reference));
    prepared.impl_->legs.push_back(std::move(*normalized));
  }
  prepared.impl_->grouped_option_indices.reserve(prepared.impl_->legs.size());
  prepared.impl_->option_slot_by_leg.assign(prepared.impl_->legs.size(),
                                            std::numeric_limits<std::size_t>::max());
  for (std::size_t index = 0u; index < prepared.impl_->legs.size(); ++index) {
    if (prepared.impl_->legs[index].kind == VarLegKind::Option &&
        prepared.impl_->legs[index].pricing_leader == index) {
      prepared.impl_->grouped_option_indices.push_back(index);
    }
  }
  std::stable_sort(
      prepared.impl_->grouped_option_indices.begin(), prepared.impl_->grouped_option_indices.end(),
      [&](std::size_t left, std::size_t right) {
        const NormalizedVarLeg &lhs = prepared.impl_->legs[left];
        const NormalizedVarLeg &rhs = prepared.impl_->legs[right];
        return std::tie(lhs.uid, lhs.expiry_offset_ns) < std::tie(rhs.uid, rhs.expiry_offset_ns);
      });
  for (std::size_t slot = 0u; slot < prepared.impl_->grouped_option_indices.size(); ++slot) {
    prepared.impl_->option_slot_by_leg[prepared.impl_->grouped_option_indices[slot]] = slot;
  }
  for (std::size_t index = 0u; index < prepared.impl_->legs.size(); ++index) {
    const NormalizedVarLeg &leg = prepared.impl_->legs[index];
    if (leg.kind == VarLegKind::Option && leg.pricing_leader != index) {
      const std::size_t leader_slot = prepared.impl_->option_slot_by_leg[leg.pricing_leader];
      if (leader_slot == std::numeric_limits<std::size_t>::max()) {
        return Err(ErrorCode::Internal, "historical VaR: duplicate option leader unavailable");
      }
      prepared.impl_->option_slot_by_leg[index] = leader_slot;
    }
  }
  for (std::size_t begin = 0u; begin < prepared.impl_->grouped_option_indices.size();) {
    const std::uint32_t uid =
        prepared.impl_->legs[prepared.impl_->grouped_option_indices[begin]].uid;
    std::size_t end = begin + 1u;
    while (end < prepared.impl_->grouped_option_indices.size() &&
           prepared.impl_->legs[prepared.impl_->grouped_option_indices[end]].uid == uid) {
      ++end;
    }
    prepared.impl_->option_groups.push_back(VarOptionGroup{uid, begin, end});
    begin = end;
  }
  std::sort(uid_symbols.begin(), uid_symbols.end());
  for (std::size_t index = 1; index < uid_symbols.size(); ++index) {
    if (uid_symbols[index - 1u].first == uid_symbols[index].first &&
        uid_symbols[index - 1u].second != uid_symbols[index].second) {
      return Err(ErrorCode::InvalidArgument, "historical VaR: underlier uid collision");
    }
  }
  if (!std::isfinite(prepared.impl_->reference_value) ||
      !std::isfinite(prepared.impl_->reference_dollar_delta) ||
      prepared.impl_->reference_ts_ns <= 0) {
    return Err(ErrorCode::Unavailable, "historical VaR: invalid reference aggregate");
  }
  prepared.impl_->fingerprint =
      nonzero_hash(fingerprint_words.data(), fingerprint_words.size() * sizeof(std::uint64_t));
  return Ok(std::move(prepared));
}

std::size_t PreparedVarPortfolio::size() const noexcept { return impl_->legs.size(); }
double PreparedVarPortfolio::reference_value() const noexcept { return impl_->reference_value; }
double PreparedVarPortfolio::reference_dollar_delta() const noexcept {
  return impl_->reference_dollar_delta;
}
std::int64_t PreparedVarPortfolio::reference_ts_ns() const noexcept {
  return impl_->reference_ts_ns;
}
std::uint64_t PreparedVarPortfolio::fingerprint() const noexcept { return impl_->fingerprint; }
std::span<const VarReferenceLeg> PreparedVarPortfolio::reference_legs() const noexcept {
  return impl_->reference_legs;
}

Status PreparedVarPortfolio::replay_into(std::span<const VarScenario> scenarios,
                                         std::span<VarScenarioFrame> frames,
                                         std::span<VarLegFrame> leg_frames,
                                         const VarEvaluationConfig &config) const {
  if (scenarios.empty() || frames.size() != scenarios.size() || !valid_evaluation_config(config) ||
      impl_->legs.empty() ||
      scenarios.size() > std::numeric_limits<std::size_t>::max() / impl_->legs.size() ||
      (!leg_frames.empty() && leg_frames.size() != scenarios.size() * impl_->legs.size())) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid replay input/output/config");
  }
  std::int64_t previous_base = 0;
  for (const VarScenario &scenario : scenarios) {
    if (scenario.base_ts_ns <= 0 || scenario.shifted_ts_ns <= scenario.base_ts_ns ||
        scenario.base_surfaces == nullptr || scenario.shifted_surfaces == nullptr ||
        (previous_base != 0 && scenario.base_ts_ns <= previous_base)) {
      return Err(ErrorCode::InvalidArgument, "historical VaR: invalid/unsorted scenario");
    }
    previous_base = scenario.base_ts_ns;
  }

  const bool cross_sectional =
      config.projection_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  try {
    run_balanced_ranges(scenarios.size(), config.n_threads, [&](std::size_t lo, std::size_t hi) {
      // Scenarios are mathematically independent, and run_balanced_ranges
      // hands each worker a contiguous, non-overlapping scenario subrange (a
      // scenario is never split across workers). So a fresh VarBatchScratch
      // built once per range and reused across that range's scenarios cannot
      // leak state between scenarios -- and since solve_american_delta_batch
      // is itself pack/composition-invariant (Task 3's
      // EvaluateBatchLanedGreeksPackCompositionInvariant), the strikes/deltas
      // it resolves do not depend on how the scenario set was partitioned
      // across threads. That is exactly the thread-count bit-invariance the
      // aggregate route already relied on, and now the retained-leg route
      // shares it too.
      VarBatchScratch batch_scratch;
      if (leg_frames.empty() || cross_sectional) {
        batch_scratch = make_var_batch_scratch(*impl_);
      }
      for (std::size_t scenario_index = lo; scenario_index < hi; ++scenario_index) {
        if (leg_frames.empty()) {
          evaluate_scenario_batched(*impl_, scenarios[scenario_index], frames[scenario_index],
                                    batch_scratch, config);
        } else {
          const std::span<VarLegFrame> output =
              leg_frames.subspan(scenario_index * impl_->legs.size(), impl_->legs.size());
          evaluate_scenario(*impl_, scenarios[scenario_index], frames[scenario_index], output,
                            config, cross_sectional ? &batch_scratch : nullptr);
        }
      }
    });
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "historical VaR: worker allocation failed");
  } catch (const std::exception &) {
    return Err(ErrorCode::Internal, "historical VaR: worker failed");
  }
  return Ok();
}

Result<VarRiskStatistics> historical_var_statistics(std::span<const VarScenarioFrame> frames,
                                                    double confidence) {
  if (frames.empty() || !std::isfinite(confidence) || confidence <= 0.0 || confidence >= 1.0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR statistics: invalid input");
  }
  struct LossRecord {
    double loss{0.0};
    std::size_t scenario_index{0};
  };
  std::vector<LossRecord> losses;
  losses.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const VarScenarioFrame &frame = frames[index];
    if (frame.status == VarScenarioStatus::Ok && frame.n_failed == 0u && frame.n_ok != 0u &&
        frame.definition_fingerprint != 0u && std::isfinite(frame.pnl)) {
      losses.push_back(LossRecord{-frame.pnl, index});
    }
  }
  if (losses.empty()) {
    return Err(ErrorCode::Unavailable, "historical VaR statistics: no successful scenarios");
  }
  std::sort(losses.begin(), losses.end(), [](const LossRecord &left, const LossRecord &right) {
    return std::tie(left.loss, left.scenario_index) < std::tie(right.loss, right.scenario_index);
  });
  const std::size_t rank =
      static_cast<std::size_t>(std::ceil(confidence * static_cast<double>(losses.size())));
  const std::size_t index = std::min(losses.size() - 1u, rank == 0u ? 0u : rank - 1u);
  double tail_sum = 0.0;
  for (std::size_t tail = index; tail < losses.size(); ++tail) {
    tail_sum += losses[tail].loss;
  }
  VarRiskStatistics result;
  result.confidence = confidence;
  result.value_at_risk = losses[index].loss;
  result.expected_shortfall = tail_sum / static_cast<double>(losses.size() - index);
  result.n_scenarios = losses.size();
  return Ok(result);
}

namespace {

// Same qualifying-frame filter as historical_var_statistics's nearest-rank
// path above, shared so the weighted overload can never silently pick a
// different frame set for the same input.
[[nodiscard]] bool var_frame_qualifies(const VarScenarioFrame &frame) noexcept {
  return frame.status == VarScenarioStatus::Ok && frame.n_failed == 0u && frame.n_ok != 0u &&
         frame.definition_fingerprint != 0u && std::isfinite(frame.pnl);
}

struct WeightedLossRecord {
  double loss{0.0};
  double weight{0.0}; // normalized; sums to 1 across every returned record
  std::size_t scenario_index{0};
};

// Qualifying frames, sorted ascending by loss (ties broken by scenario
// index, matching the unweighted path), each carrying its normalized
// age/recency weight. Age 0 = most recent scenario by shifted_ts_ns among
// the qualifying frames; ties in shifted_ts_ns keep ascending scenario-index
// order so age assignment is deterministic.
[[nodiscard]] Result<std::vector<WeightedLossRecord>>
build_weighted_losses(std::span<const VarScenarioFrame> frames, const VarWeighting &weighting) {
  if (frames.empty() || !std::isfinite(weighting.ewma_lambda) || weighting.ewma_lambda <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR statistics: invalid input");
  }
  struct Candidate {
    double loss{0.0};
    std::int64_t shifted_ts_ns{0};
    std::size_t scenario_index{0};
  };
  std::vector<Candidate> candidates;
  candidates.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (var_frame_qualifies(frames[index])) {
      candidates.push_back(Candidate{-frames[index].pnl, frames[index].shifted_ts_ns, index});
    }
  }
  if (candidates.empty()) {
    return Err(ErrorCode::Unavailable, "historical VaR statistics: no successful scenarios");
  }
  std::vector<std::size_t> by_recency(candidates.size());
  std::iota(by_recency.begin(), by_recency.end(), std::size_t{0});
  std::stable_sort(by_recency.begin(), by_recency.end(), [&](std::size_t left, std::size_t right) {
    return candidates[left].shifted_ts_ns > candidates[right].shifted_ts_ns;
  });
  std::vector<double> raw_weight(candidates.size(), 0.0);
  double total_weight = 0.0;
  for (std::size_t age = 0; age < by_recency.size(); ++age) {
    const double weight = std::pow(weighting.ewma_lambda, static_cast<double>(age));
    raw_weight[by_recency[age]] = weight;
    total_weight += weight;
  }
  std::vector<WeightedLossRecord> losses;
  losses.reserve(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    losses.push_back(WeightedLossRecord{candidates[i].loss, raw_weight[i] / total_weight,
                                        candidates[i].scenario_index});
  }
  std::sort(losses.begin(), losses.end(),
            [](const WeightedLossRecord &left, const WeightedLossRecord &right) {
              return std::tie(left.loss, left.scenario_index) <
                     std::tie(right.loss, right.scenario_index);
            });
  return Ok(std::move(losses));
}

// Repeated floating-point summation of normalized weights carries rounding
// noise on the order of a few ULPs per addition -- many orders of magnitude
// smaller than any weight increment this function actually sees. Note this
// code path is only reached for ewma_lambda != 1.0: the exact-equal-weight
// case (where increments could get as fine as 1/n) is special-cased away to
// a direct call into the unweighted two-arg overload before ever reaching
// here -- see historical_var_statistics's ewma_lambda == 1.0 branch below.
// This epsilon absorbs that summation noise so the crossing index lands on
// the mathematically-exact side of a confidence boundary even when that
// boundary is not itself exactly representable in binary floating point.
constexpr double kCumulativeWeightEpsilon = 1.0e-9;

[[nodiscard]] VarRiskStatistics
weighted_statistics_from_sorted_losses(std::span<const WeightedLossRecord> sorted_losses,
                                       double confidence) {
  double cumulative = 0.0;
  std::size_t var_index = sorted_losses.size() - 1u;
  for (std::size_t i = 0; i < sorted_losses.size(); ++i) {
    cumulative += sorted_losses[i].weight;
    if (cumulative >= confidence - kCumulativeWeightEpsilon) {
      var_index = i;
      break;
    }
  }
  double tail_weight = 0.0;
  double tail_weighted_sum = 0.0;
  for (std::size_t i = var_index; i < sorted_losses.size(); ++i) {
    tail_weight += sorted_losses[i].weight;
    tail_weighted_sum += sorted_losses[i].weight * sorted_losses[i].loss;
  }
  VarRiskStatistics result;
  result.confidence = confidence;
  result.value_at_risk = sorted_losses[var_index].loss;
  result.expected_shortfall = tail_weighted_sum / tail_weight;
  result.n_scenarios = sorted_losses.size();
  return result;
}

} // namespace

Result<VarRiskStatistics> historical_var_statistics(std::span<const VarScenarioFrame> frames,
                                                    double confidence,
                                                    const VarWeighting &weighting) {
  // Equal weighting is mathematically identical to the plain nearest-rank
  // path above, but the general weighted-quantile arithmetic (a running sum
  // of weight*loss products, divided by a running sum of weights) is not
  // bit-identical to that path's direct rank/average -- the two sum in a
  // different order. Delegating on the exact ewma_lambda == 1.0 default
  // guarantees the bit-exact reproduction this overload promises, rather
  // than relying on floating-point arithmetic happening to agree.
  if (weighting.ewma_lambda == 1.0) {
    return historical_var_statistics(frames, confidence);
  }
  if (!std::isfinite(confidence) || confidence <= 0.0 || confidence >= 1.0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR statistics: invalid input");
  }
  ATX_TRY(std::vector<WeightedLossRecord> losses, build_weighted_losses(frames, weighting));
  return Ok(weighted_statistics_from_sorted_losses(losses, confidence));
}

Result<std::vector<VarRiskStatistics>>
historical_var_curve(std::span<const VarScenarioFrame> frames, std::span<const double> confidences,
                     const VarWeighting &weighting) {
  if (confidences.empty()) {
    return Err(ErrorCode::InvalidArgument, "historical VaR curve: confidences must be non-empty");
  }
  for (const double confidence : confidences) {
    if (!std::isfinite(confidence) || confidence <= 0.0 || confidence >= 1.0) {
      return Err(ErrorCode::InvalidArgument, "historical VaR curve: invalid confidence");
    }
  }
  std::vector<VarRiskStatistics> curve;
  curve.reserve(confidences.size());
  for (const double confidence : confidences) {
    ATX_TRY(VarRiskStatistics stats, historical_var_statistics(frames, confidence, weighting));
    curve.push_back(stats);
  }
  return Ok(std::move(curve));
}

Result<HistoricalVarResult> run_historical_var(const SurfaceDb &db,
                                               std::span<const VarPosition> positions,
                                               const VarRunConfig &config) {
  if (!valid_evaluation_config(config.evaluation) || !std::isfinite(config.confidence) ||
      config.confidence <= 0.0 || config.confidence >= 1.0 || config.max_session_gap_days < 0 ||
      !std::isfinite(config.max_excluded_fraction) || config.max_excluded_fraction < 0.0 ||
      config.max_excluded_fraction > 1.0) {
    return Err(ErrorCode::InvalidArgument, "historical VaR: invalid run config");
  }
  ATX_TRY(std::vector<std::uint32_t> uids, required_uids(positions));
  ATX_TRY(Clock full_clock, Clock::from_surface_db(db));
  const std::span<const SnapshotRef> all_refs = full_clock.refs();
  const std::string reference_date =
      config.reference_date.empty() ? all_refs.back().date : config.reference_date;
  const auto reference_it =
      std::find_if(all_refs.begin(), all_refs.end(),
                   [&](const SnapshotRef &ref) { return ref.date == reference_date; });
  if (reference_it == all_refs.end()) {
    return Err(ErrorCode::NotFound, "historical VaR: reference date not present");
  }
  Result<MarketSnapshot> reference_snapshot = MarketSnapshot::load(
      reference_it->archive_path, config.query_pricing_tier, uids, config.archive_backing);
  if (!reference_snapshot) {
    return Err(reference_snapshot.error());
  }
  ATX_TRY_VOID(validate_snapshot(*reference_snapshot, uids, config.provenance_policy));
  for (const VarPosition &position : positions) {
    const std::string &symbol =
        std::visit([](const auto &leg) -> const std::string & { return leg.underlier; }, position);
    const std::optional<std::uint32_t> archive_uid = reference_snapshot->uid_of(symbol);
    if (!archive_uid.has_value() || *archive_uid != uid_for_symbol(symbol)) {
      return Err(ErrorCode::NotFound,
                 "historical VaR: reference underlier missing or has noncanonical uid");
    }
  }
  ATX_TRY(PreparedVarPortfolio prepared,
          PreparedVarPortfolio::create(positions, reference_snapshot->set(), config.evaluation));

  const std::string date_begin =
      config.date_begin.empty() ? all_refs.front().date : config.date_begin;
  const std::string date_end = config.date_end.empty() ? all_refs.back().date : config.date_end;
  ATX_TRY(Clock window, full_clock.between(date_begin, date_end));
  if (window.size() < 2u) {
    return Err(ErrorCode::InvalidArgument,
               "historical VaR: date range needs at least two observations");
  }
  const std::span<const SnapshotRef> refs = window.refs();
  // I1: filter adjacent-partition pairs whose calendar gap exceeds the guard
  // (disabled at 0) BEFORE any snapshot for the pair is loaded, so a wholly
  // missing run of partitions never bridges silently into a single
  // multi-session observation the way an unguarded adjacency walk would.
  std::vector<std::size_t> surviving_base_ref;
  surviving_base_ref.reserve(refs.size() > 0u ? refs.size() - 1u : 0u);
  std::size_t n_gap_skipped = 0u;
  for (std::size_t index = 0; index + 1u < refs.size(); ++index) {
    if (config.max_session_gap_days > 0) {
      const std::int64_t gap =
          session_date_ordinal(refs[index + 1u].date) - session_date_ordinal(refs[index].date);
      if (gap > static_cast<std::int64_t>(config.max_session_gap_days)) {
        ++n_gap_skipped;
        continue;
      }
    }
    surviving_base_ref.push_back(index);
  }
  const std::size_t scenario_count = surviving_base_ref.size();
  if (prepared.size() != 0u &&
      scenario_count > std::numeric_limits<std::size_t>::max() / prepared.size()) {
    return Err(ErrorCode::OutOfRange, "historical VaR: scenario/leg output size overflows");
  }

  HistoricalVarResult result;
  result.reference_date = reference_date;
  result.reference_ts_ns = prepared.reference_ts_ns();
  result.reference_value = prepared.reference_value();
  result.reference_dollar_delta = prepared.reference_dollar_delta();
  result.n_legs = prepared.size();
  result.n_gap_skipped = n_gap_skipped;
  result.base_dates.reserve(scenario_count);
  result.shifted_dates.reserve(scenario_count);
  result.frames.resize(scenario_count);
  for (std::size_t index = 0; index < scenario_count; ++index) {
    result.base_dates.push_back(refs[surviving_base_ref[index]].date);
    result.shifted_dates.push_back(refs[surviving_base_ref[index] + 1u].date);
  }
  if (config.retain_leg_frames) {
    result.leg_frames.resize(scenario_count * prepared.size());
  }

  try {
    run_balanced_ranges(
        scenario_count, config.evaluation.n_threads, [&](std::size_t lo, std::size_t hi) {
          std::size_t base_ref_index = surviving_base_ref[lo];
          SnapshotLoad base = load_snapshot(refs[base_ref_index], uids, config);
          for (std::size_t scenario_index = lo; scenario_index < hi; ++scenario_index) {
            const std::size_t expected_base_ref = surviving_base_ref[scenario_index];
            if (expected_base_ref != base_ref_index) {
              // A gap-skipped pair breaks the "shifted becomes next base"
              // reuse chain; reload fresh at this (rare) boundary.
              base = load_snapshot(refs[expected_base_ref], uids, config);
              base_ref_index = expected_base_ref;
            }
            SnapshotLoad shifted = load_snapshot(refs[expected_base_ref + 1u], uids, config);
            std::span<VarLegFrame> output =
                result.leg_frames.empty() ? std::span<VarLegFrame>{}
                                          : std::span<VarLegFrame>{result.leg_frames}.subspan(
                                                scenario_index * prepared.size(), prepared.size());
            if (!base.snapshot.has_value() || !shifted.snapshot.has_value()) {
              const bool archive_error =
                  !base.snapshot.has_value() ? base.archive_error : shifted.archive_error;
              const VarLegStatus failure =
                  !base.snapshot.has_value() ? base.failure : shifted.failure;
              const std::int64_t base_ts =
                  base.snapshot.has_value() ? base.snapshot->ts_ns() : std::int64_t{0};
              const std::int64_t shifted_ts =
                  shifted.snapshot.has_value() ? shifted.snapshot->ts_ns() : std::int64_t{0};
              // [solver] F4: a corrupt/truncated archive or I/O failure is
              // structural, not a market condition -- ArchiveError overrides
              // the MarketUnavailable that scenario_status_for(failure)
              // would otherwise derive from SurfaceUnavailable.
              fill_loaded_failure(
                  prepared.reference_legs(), result.frames[scenario_index], output, failure,
                  base_ts, shifted_ts,
                  archive_error ? std::optional<VarScenarioStatus>{VarScenarioStatus::ArchiveError}
                                : std::nullopt);
            } else {
              const VarScenario scenario{base.snapshot->ts_ns(), &base.snapshot->set(),
                                         shifted.snapshot->ts_ns(), &shifted.snapshot->set()};
              VarEvaluationConfig serial = config.evaluation;
              serial.n_threads = 1u;
              const Status replay = prepared.replay_into(
                  std::span<const VarScenario>{&scenario, 1u},
                  std::span<VarScenarioFrame>{&result.frames[scenario_index], 1u}, output, serial);
              if (!replay) {
                // [solver] F5: replay_into's own pre-flight scenario check
                // (shifted_ts_ns <= base_ts_ns) is the realistic reason this
                // single-scenario replay fails -- a structural non-monotone-
                // timestamp archive fault, not a generic invalid value.
                const VarLegStatus failure = scenario.shifted_ts_ns <= scenario.base_ts_ns
                                                 ? VarLegStatus::TimestampMismatch
                                                 : VarLegStatus::InvalidValue;
                fill_loaded_failure(prepared.reference_legs(), result.frames[scenario_index],
                                    output, failure, scenario.base_ts_ns, scenario.shifted_ts_ns);
              }
            }
            base = std::move(shifted);
            base_ref_index = expected_base_ref + 1u;
          }
        });
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "historical VaR: replay allocation failed");
  } catch (const std::exception &) {
    return Err(ErrorCode::Internal, "historical VaR: replay worker failed");
  }

  // I3 (review fix round 1): sum the per-scenario tallies both
  // evaluate_scenario and evaluate_scenario_batched now stamp on
  // VarScenarioFrame::n_tenor_extrapolated -- populated on EVERY route, not
  // just when leg_frames are retained. result.frames is a fixed-size,
  // scenario-index-ordered array each worker writes into by index (never
  // appended), so summing it in ascending index order here is a
  // deterministic, thread-count-invariant reduction: it depends on
  // scenario order, never on which worker finished which scenario first.
  {
    std::size_t extrapolated = 0u;
    for (const VarScenarioFrame &frame : result.frames) {
      extrapolated += frame.n_tenor_extrapolated;
    }
    result.n_tenor_extrapolated_legs = extrapolated;
  }

  if (config.failure_policy == VarScenarioFailurePolicy::RejectRun) {
    for (std::size_t index = 0; index < result.frames.size(); ++index) {
      if (result.frames[index].status != VarScenarioStatus::Ok) {
        return Err(ErrorCode::Unavailable, "historical VaR: scenario " + result.base_dates[index] +
                                               " -> " + result.shifted_dates[index] + " failed (" +
                                               to_string(result.frames[index].status) + ")");
      }
    }
  } else {
    // [solver] F4: ExcludeFromDistribution otherwise excludes any non-Ok
    // scenario from the loss distribution silently (historical_var_statistics
    // below only sums Ok frames). ArchiveError is structural -- a corrupt
    // archive or I/O fault, not an ordinary missing-market day -- so it must
    // still fail the run even under this policy; a systematically
    // I/O-degraded run must not silently shrink the loss distribution.
    std::size_t excluded = 0u;
    for (std::size_t index = 0; index < result.frames.size(); ++index) {
      if (result.frames[index].status == VarScenarioStatus::ArchiveError) {
        return Err(ErrorCode::Unavailable, "historical VaR: scenario " + result.base_dates[index] +
                                               " -> " + result.shifted_dates[index] + " failed (" +
                                               to_string(result.frames[index].status) + ")");
      }
      if (result.frames[index].status != VarScenarioStatus::Ok) {
        ++excluded;
      }
    }
    result.n_excluded_from_distribution = excluded;
    // I5: 1.0 disables the guard (today's unbounded ExcludeFromDistribution
    // behavior). Below 1.0, a failure-correlated shrink of the loss
    // distribution beyond this fraction fails the run outright instead of
    // silently thinning the tail exactly when the data is worst.
    if (config.max_excluded_fraction < 1.0 && !result.frames.empty()) {
      const double fraction =
          static_cast<double>(excluded) / static_cast<double>(result.frames.size());
      if (fraction > config.max_excluded_fraction) {
        return Err(ErrorCode::Unavailable,
                   "historical VaR: excluded scenario fraction exceeds max_excluded_fraction");
      }
    }
  }
  ATX_TRY(result.risk, historical_var_statistics(result.frames, config.confidence));
  return Ok(std::move(result));
}

} // namespace atx::vol
