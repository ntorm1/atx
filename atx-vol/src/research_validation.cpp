#include "atx/vol/research_validation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/math.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr double kEulerMascheroni = 0.5772156649015328606;

struct DecisionGroup {
  std::int64_t timestamp{0};
  std::size_t begin{0};
  std::size_t end{0};
};

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

void hash_byte(std::uint64_t &hash, std::uint8_t value) noexcept {
  hash ^= static_cast<std::uint64_t>(value);
  hash *= kFnvPrime;
}

void hash_u64(std::uint64_t &hash, std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffu));
    value >>= 8u;
  }
}

void hash_string(std::uint64_t &hash, const std::string &value) noexcept {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  for (const char character : value) {
    hash_byte(hash, static_cast<std::uint8_t>(character));
  }
}

[[nodiscard]] double normalized_double(double value) noexcept { return value == 0.0 ? 0.0 : value; }

void hash_parameter_value(std::uint64_t &hash, const ResearchParameterValue &value) noexcept {
  hash_byte(hash, static_cast<std::uint8_t>(value.index()));
  std::visit(
      [&hash](const auto &item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::int64_t>) {
          hash_u64(hash, std::bit_cast<std::uint64_t>(item));
        } else if constexpr (std::is_same_v<Item, std::uint64_t>) {
          hash_u64(hash, item);
        } else if constexpr (std::is_same_v<Item, double>) {
          hash_u64(hash, std::bit_cast<std::uint64_t>(normalized_double(item)));
        } else if constexpr (std::is_same_v<Item, bool>) {
          hash_byte(hash, item ? 1u : 0u);
        } else {
          hash_string(hash, item);
        }
      },
      value);
}

[[nodiscard]] std::uint64_t
parameter_set_identity(const std::vector<ResearchGridParameter> &parameters) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  hash_u64(hash, kResearchValidationSchemaSalt);
  hash_string(hash, "parameter-set");
  hash_u64(hash, static_cast<std::uint64_t>(parameters.size()));
  for (const ResearchGridParameter &parameter : parameters) {
    hash_string(hash, parameter.name);
    hash_parameter_value(hash, parameter.value);
  }
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] std::uint64_t candidate_identity(const ResearchSignalCandidate &candidate) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  hash_u64(hash, kResearchValidationSchemaSalt);
  hash_string(hash, "signal-candidate");
  hash_string(hash, candidate.id);
  hash_byte(hash, static_cast<std::uint8_t>(candidate.transform));
  hash_u64(hash, static_cast<std::uint64_t>(candidate.lag));
  hash_u64(hash, static_cast<std::uint64_t>(candidate.lookback));
  hash_byte(hash, static_cast<std::uint8_t>(candidate.direction));
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] Status
validate_canonical_observations(std::span<const ResearchObservation> observations) {
  ATX_TRY(auto canonical, canonicalize_research_observations(observations));
  if (!std::equal(canonical.begin(), canonical.end(), observations.begin(), observations.end())) {
    return Err(ErrorCode::InvalidArgument,
               "research observations must use canonical (decision_ts_ns, uid) ordering");
  }
  return Ok();
}

[[nodiscard]] std::vector<DecisionGroup>
decision_groups(std::span<const ResearchObservation> observations) {
  std::vector<DecisionGroup> groups;
  std::size_t begin = 0u;
  while (begin < observations.size()) {
    std::size_t end = begin + 1u;
    while (end < observations.size() &&
           observations[end].decision_ts_ns == observations[begin].decision_ts_ns) {
      ++end;
    }
    groups.push_back(DecisionGroup{observations[begin].decision_ts_ns, begin, end});
    begin = end;
  }
  return groups;
}

[[nodiscard]] Result<ResearchValidationPlan>
make_plan_impl(std::span<const ResearchObservation> observations,
               const ResearchWalkForwardSpec &spec) {
  if (spec.min_train_groups == 0u || spec.test_groups == 0u || spec.step_groups == 0u) {
    return Err(ErrorCode::InvalidArgument,
               "walk-forward train, test, and step group counts must be positive");
  }
  if (spec.step_groups < spec.test_groups) {
    return Err(ErrorCode::InvalidArgument,
               "walk-forward test windows overlap: step_groups must be >= test_groups");
  }
  if (spec.embargo_ns < 0) {
    return Err(ErrorCode::InvalidArgument, "walk-forward embargo_ns must be non-negative");
  }
  if (spec.kind == ResearchWalkForwardKind::Rolling &&
      (spec.max_train_groups == 0u || spec.max_train_groups < spec.min_train_groups)) {
    return Err(ErrorCode::InvalidArgument,
               "rolling walk-forward max_train_groups must cover min_train_groups");
  }
  if (spec.kind != ResearchWalkForwardKind::Anchored &&
      spec.kind != ResearchWalkForwardKind::Rolling) {
    return Err(ErrorCode::InvalidArgument, "unknown walk-forward kind");
  }

  const std::vector<DecisionGroup> groups = decision_groups(observations);
  if (groups.size() < spec.min_train_groups + spec.test_groups) {
    return Err(ErrorCode::InvalidArgument,
               "insufficient decision groups for one complete walk-forward fold");
  }

  ResearchValidationPlan plan;
  plan.spec = spec;
  std::size_t test_begin = spec.min_train_groups;
  std::uint32_t fold_id = 0u;
  while (test_begin <= groups.size() - spec.test_groups) {
    const std::size_t test_end = test_begin + spec.test_groups;
    std::size_t train_begin = 0u;
    if (spec.kind == ResearchWalkForwardKind::Rolling && test_begin > spec.max_train_groups) {
      train_begin = test_begin - spec.max_train_groups;
    }

    ResearchValidationFold fold;
    fold.id = fold_id;
    for (std::size_t group_index = test_begin; group_index < test_end; ++group_index) {
      for (std::size_t index = groups[group_index].begin; index < groups[group_index].end;
           ++index) {
        fold.test_indices.push_back(index);
      }
    }

    std::int64_t test_observed_min = std::numeric_limits<std::int64_t>::max();
    for (const std::size_t index : fold.test_indices) {
      test_observed_min = std::min(test_observed_min, observations[index].observed_ts_ns);
    }
    const std::int64_t embargo_begin =
        test_observed_min < std::numeric_limits<std::int64_t>::min() + spec.embargo_ns
            ? std::numeric_limits<std::int64_t>::min()
            : test_observed_min - spec.embargo_ns;

    for (std::size_t group_index = train_begin; group_index < test_begin; ++group_index) {
      const DecisionGroup &group = groups[group_index];
      const bool outcome_overlaps =
          std::any_of(observations.begin() + static_cast<std::ptrdiff_t>(group.begin),
                      observations.begin() + static_cast<std::ptrdiff_t>(group.end),
                      [test_observed_min](const ResearchObservation &row) {
                        return row.label_end_ts_ns > test_observed_min;
                      });
      const bool inside_embargo = spec.embargo_ns > 0 && group.timestamp >= embargo_begin &&
                                  group.timestamp < test_observed_min;
      for (std::size_t index = group.begin; index < group.end; ++index) {
        if (outcome_overlaps) {
          fold.purged_indices.push_back(index);
        } else if (inside_embargo) {
          fold.embargoed_indices.push_back(index);
        } else {
          fold.train_indices.push_back(index);
        }
      }
    }
    if (fold.train_indices.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "purge and embargo removed every training observation");
    }

    plan.folds.push_back(std::move(fold));
    if (fold_id == std::numeric_limits<std::uint32_t>::max()) {
      return Err(ErrorCode::OutOfRange, "walk-forward fold id overflow");
    }
    ++fold_id;
    if (spec.step_groups > groups.size() - test_begin) {
      break;
    }
    test_begin += spec.step_groups;
  }
  return Ok(std::move(plan));
}

[[nodiscard]] Status validate_candidate(const ResearchSignalCandidate &candidate) {
  if (candidate.id.empty()) {
    return Err(ErrorCode::InvalidArgument, "research candidate id must not be empty");
  }
  if (candidate.direction != ResearchSignalDirection::LongHigh &&
      candidate.direction != ResearchSignalDirection::ShortHigh) {
    return Err(ErrorCode::InvalidArgument, "unknown research signal direction");
  }
  switch (candidate.transform) {
  case ResearchSignalTransform::Identity:
    if (candidate.lookback != 0u) {
      return Err(ErrorCode::InvalidArgument, "identity candidate lookback must be zero");
    }
    break;
  case ResearchSignalTransform::Difference:
    if (candidate.lookback == 0u) {
      return Err(ErrorCode::InvalidArgument, "difference candidate lookback must be positive");
    }
    break;
  case ResearchSignalTransform::RollingZScore:
    if (candidate.lookback < 2u) {
      return Err(ErrorCode::InvalidArgument,
                 "rolling z-score candidate lookback must be at least two");
    }
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "unknown research signal transform");
  }
  return Ok();
}

struct TransformCache {
  std::vector<std::vector<std::size_t>> uid_histories;
  std::vector<std::size_t> history_slot;
  std::vector<std::size_t> history_position;
};

[[nodiscard]] TransformCache
make_transform_cache(std::span<const ResearchObservation> observations) {
  std::map<std::uint32_t, std::vector<std::size_t>> by_uid;
  for (std::size_t index = 0; index < observations.size(); ++index) {
    by_uid[observations[index].uid].push_back(index);
  }

  TransformCache cache;
  cache.history_slot.resize(observations.size());
  cache.history_position.resize(observations.size());
  cache.uid_histories.reserve(by_uid.size());
  for (auto &[uid, history] : by_uid) {
    static_cast<void>(uid);
    const std::size_t slot = cache.uid_histories.size();
    for (std::size_t position = 0; position < history.size(); ++position) {
      cache.history_slot[history[position]] = slot;
      cache.history_position[history[position]] = position;
    }
    cache.uid_histories.push_back(std::move(history));
  }
  return cache;
}

[[nodiscard]] double signal_position(std::span<const ResearchObservation> observations,
                                     const TransformCache &cache,
                                     const ResearchSignalCandidate &candidate,
                                     std::size_t index) noexcept {
  const std::vector<std::size_t> &history = cache.uid_histories[cache.history_slot[index]];
  const std::size_t position = cache.history_position[index];
  if (position < candidate.lag) {
    return 0.0;
  }
  const std::size_t source_position = position - candidate.lag;
  double transformed = 0.0;
  switch (candidate.transform) {
  case ResearchSignalTransform::Identity:
    transformed = observations[history[source_position]].signal;
    break;
  case ResearchSignalTransform::Difference:
    if (source_position < candidate.lookback) {
      return 0.0;
    }
    transformed = observations[history[source_position]].signal -
                  observations[history[source_position - candidate.lookback]].signal;
    break;
  case ResearchSignalTransform::RollingZScore: {
    if (source_position < candidate.lookback) {
      return 0.0;
    }
    const std::size_t window_begin = source_position - candidate.lookback;
    double mean = 0.0;
    for (std::size_t position_index = window_begin; position_index < source_position;
         ++position_index) {
      mean += observations[history[position_index]].signal;
    }
    mean /= static_cast<double>(candidate.lookback);
    double sum_squared_deviation = 0.0;
    for (std::size_t position_index = window_begin; position_index < source_position;
         ++position_index) {
      const double deviation = observations[history[position_index]].signal - mean;
      sum_squared_deviation += deviation * deviation;
    }
    const double standard_deviation =
        std::sqrt(sum_squared_deviation / static_cast<double>(candidate.lookback - 1u));
    transformed = standard_deviation > 0.0
                      ? (observations[history[source_position]].signal - mean) / standard_deviation
                      : 0.0;
    break;
  }
  default:
    return 0.0;
  }
  double position_value = static_cast<double>(atx::core::sign(transformed));
  if (candidate.direction == ResearchSignalDirection::ShortHigh) {
    position_value = -position_value;
  }
  return position_value;
}

[[nodiscard]] Result<std::vector<ResearchReturnObservation>>
build_returns(std::span<const ResearchObservation> observations, const TransformCache &cache,
              const ResearchSignalCandidate &candidate,
              std::span<const std::size_t> input_indices) {
  std::vector<std::size_t> indices(input_indices.begin(), input_indices.end());
  std::sort(indices.begin(), indices.end());
  if (std::adjacent_find(indices.begin(), indices.end()) != indices.end()) {
    return Err(ErrorCode::InvalidArgument, "research return index set contains duplicates");
  }
  for (const std::size_t index : indices) {
    if (index >= observations.size()) {
      return Err(ErrorCode::OutOfRange, "research return index is out of range");
    }
  }

  std::vector<ResearchReturnObservation> returns;
  std::size_t begin = 0u;
  while (begin < indices.size()) {
    const std::int64_t timestamp = observations[indices[begin]].decision_ts_ns;
    std::size_t end = begin;
    double pnl = 0.0;
    double capital = 0.0;
    while (end < indices.size() && observations[indices[end]].decision_ts_ns == timestamp) {
      const std::size_t index = indices[end];
      pnl +=
          signal_position(observations, cache, candidate, index) * observations[index].forward_pnl;
      capital += observations[index].lagged_capital;
      ++end;
    }
    if (!finite(pnl) || !finite(capital) || capital <= 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "candidate aggregation produced invalid PnL or capital");
    }
    returns.push_back(ResearchReturnObservation{timestamp, pnl, capital, pnl / capital});
    begin = end;
  }
  return Ok(std::move(returns));
}

[[nodiscard]] double inverse_normal_cdf(double probability) noexcept {
  constexpr double a1 = -3.969683028665376e+01;
  constexpr double a2 = 2.209460984245205e+02;
  constexpr double a3 = -2.759285104469687e+02;
  constexpr double a4 = 1.383577518672690e+02;
  constexpr double a5 = -3.066479806614716e+01;
  constexpr double a6 = 2.506628277459239e+00;
  constexpr double b1 = -5.447609879822406e+01;
  constexpr double b2 = 1.615858368580409e+02;
  constexpr double b3 = -1.556989798598866e+02;
  constexpr double b4 = 6.680131188771972e+01;
  constexpr double b5 = -1.328068155288572e+01;
  constexpr double c1 = -7.784894002430293e-03;
  constexpr double c2 = -3.223964580411365e-01;
  constexpr double c3 = -2.400758277161838e+00;
  constexpr double c4 = -2.549732539343734e+00;
  constexpr double c5 = 4.374664141464968e+00;
  constexpr double c6 = 2.938163982698783e+00;
  constexpr double d1 = 7.784695709041462e-03;
  constexpr double d2 = 3.224671290700398e-01;
  constexpr double d3 = 2.445134137142996e+00;
  constexpr double d4 = 3.754408661907416e+00;
  constexpr double low = 0.02425;
  constexpr double high = 1.0 - low;

  if (probability <= 0.0) {
    return -std::numeric_limits<double>::infinity();
  }
  if (probability >= 1.0) {
    return std::numeric_limits<double>::infinity();
  }
  if (probability < low) {
    const double q = std::sqrt(-2.0 * std::log(probability));
    return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
           ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
  }
  if (probability <= high) {
    const double q = probability - 0.5;
    const double r = q * q;
    return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
           (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - probability));
  return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
         ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
}

[[nodiscard]] double sharpe_probability(double sharpe, double threshold, double skewness,
                                        double kurtosis, std::size_t count) noexcept {
  if (count < 2u) {
    return 0.0;
  }
  const double denominator_squared =
      1.0 - skewness * sharpe + 0.25 * (kurtosis - 1.0) * sharpe * sharpe;
  if (!finite(denominator_squared) || denominator_squared <= 0.0) {
    return 0.0;
  }
  const double statistic = (sharpe - threshold) * std::sqrt(static_cast<double>(count - 1u)) /
                           std::sqrt(denominator_squared);
  return finite(statistic) ? atx::core::norm_cdf(statistic) : (statistic > 0.0 ? 1.0 : 0.0);
}

[[nodiscard]] double sample_variance(std::span<const double> values) noexcept {
  if (values.size() < 2u) {
    return 0.0;
  }
  const double mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double sum = 0.0;
  for (const double value : values) {
    const double deviation = value - mean;
    sum += deviation * deviation;
  }
  return sum / static_cast<double>(values.size() - 1u);
}

[[nodiscard]] Result<std::vector<double>>
validate_raw_p_values(std::span<const double> raw_p_values) {
  if (raw_p_values.empty()) {
    return Err(ErrorCode::InvalidArgument, "p-value family must not be empty");
  }
  std::vector<double> values(raw_p_values.begin(), raw_p_values.end());
  for (const double value : values) {
    if (!finite(value) || value < 0.0 || value > 1.0) {
      return Err(ErrorCode::InvalidArgument, "raw p-values must be finite and in [0,1]");
    }
  }
  return Ok(std::move(values));
}

[[nodiscard]] ResearchPromotionGateResult boolean_gate(ResearchPromotionGateCode code, bool passed,
                                                       std::string detail) {
  return ResearchPromotionGateResult{code, passed, passed ? 1.0 : 0.0, 1.0, std::move(detail)};
}

} // namespace

Result<std::vector<ResearchObservation>>
canonicalize_research_observations(std::span<const ResearchObservation> observations) {
  if (observations.empty()) {
    return Err(ErrorCode::InvalidArgument, "research observations must not be empty");
  }
  std::vector<ResearchObservation> canonical(observations.begin(), observations.end());
  for (const ResearchObservation &row : canonical) {
    if (row.uid == 0u) {
      return Err(ErrorCode::InvalidArgument, "research observation uid must be nonzero");
    }
    if (row.observed_ts_ns > row.available_ts_ns || row.available_ts_ns > row.decision_ts_ns ||
        row.decision_ts_ns >= row.execution_ts_ns || row.execution_ts_ns >= row.label_end_ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "research observation violates point-in-time clock ordering");
    }
    if (!finite(row.signal) || !finite(row.forward_pnl) || !finite(row.lagged_capital) ||
        row.lagged_capital <= 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "research signal, PnL, and positive lagged capital must be finite");
    }
    if (row.source_identity.file_size == 0u) {
      return Err(ErrorCode::InvalidArgument,
                 "research observation source identity must be populated");
    }
  }
  std::sort(canonical.begin(), canonical.end(),
            [](const ResearchObservation &left, const ResearchObservation &right) {
              return std::tie(left.decision_ts_ns, left.uid) <
                     std::tie(right.decision_ts_ns, right.uid);
            });
  for (std::size_t index = 1u; index < canonical.size(); ++index) {
    if (canonical[index - 1u].decision_ts_ns == canonical[index].decision_ts_ns &&
        canonical[index - 1u].uid == canonical[index].uid) {
      return Err(ErrorCode::AlreadyExists, "duplicate research panel key (decision_ts_ns, uid)");
    }
  }
  return Ok(std::move(canonical));
}

Result<std::vector<ResearchParameterSet>>
enumerate_research_parameter_grid(const ResearchParameterGrid &grid) {
  if (grid.axes.empty()) {
    return Err(ErrorCode::InvalidArgument, "research parameter grid must contain axes");
  }
  if (grid.max_trials == 0u) {
    return Err(ErrorCode::InvalidArgument, "research parameter max_trials must be positive");
  }
  std::vector<ResearchParameterAxis> axes = grid.axes;
  for (ResearchParameterAxis &axis : axes) {
    if (axis.name.empty() || axis.values.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "research parameter axes require a name and at least one value");
    }
    for (ResearchParameterValue &value : axis.values) {
      if (double *number = std::get_if<double>(&value); number != nullptr) {
        if (!finite(*number)) {
          return Err(ErrorCode::InvalidArgument, "research parameter doubles must be finite");
        }
        *number = normalized_double(*number);
      }
    }
    std::sort(axis.values.begin(), axis.values.end());
    if (std::adjacent_find(axis.values.begin(), axis.values.end()) != axis.values.end()) {
      return Err(ErrorCode::AlreadyExists, "research parameter axis values must be unique");
    }
  }
  std::sort(axes.begin(), axes.end(),
            [](const ResearchParameterAxis &left, const ResearchParameterAxis &right) {
              return left.name < right.name;
            });
  for (std::size_t index = 1u; index < axes.size(); ++index) {
    if (axes[index - 1u].name == axes[index].name) {
      return Err(ErrorCode::AlreadyExists, "research parameter axis names must be unique");
    }
  }

  std::uint64_t count = 1u;
  for (const ResearchParameterAxis &axis : axes) {
    const std::uint64_t axis_size = static_cast<std::uint64_t>(axis.values.size());
    if (axis_size > grid.max_trials / count ||
        axis_size > std::numeric_limits<std::uint64_t>::max() / count) {
      return Err(ErrorCode::OutOfRange, "research parameter grid exceeds max_trials or overflows");
    }
    count *= axis_size;
  }
  if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return Err(ErrorCode::OutOfRange, "research parameter grid exceeds addressable size");
  }

  std::vector<ResearchParameterSet> sets(1u);
  for (const ResearchParameterAxis &axis : axes) {
    std::vector<ResearchParameterSet> expanded;
    if (sets.size() > std::numeric_limits<std::size_t>::max() / axis.values.size()) {
      return Err(ErrorCode::OutOfRange, "research parameter grid allocation overflows");
    }
    expanded.reserve(sets.size() * axis.values.size());
    for (const ResearchParameterSet &prefix : sets) {
      for (const ResearchParameterValue &value : axis.values) {
        ResearchParameterSet set = prefix;
        set.values.push_back(ResearchGridParameter{axis.name, value});
        expanded.push_back(std::move(set));
      }
    }
    sets = std::move(expanded);
  }
  for (ResearchParameterSet &set : sets) {
    set.identity = parameter_set_identity(set.values);
  }
  return Ok(std::move(sets));
}

Result<ResearchValidationPlan>
make_purged_walk_forward_plan(std::span<const ResearchObservation> canonical_observations,
                              const ResearchWalkForwardSpec &spec) {
  ATX_TRY_VOID(validate_canonical_observations(canonical_observations));
  return make_plan_impl(canonical_observations, spec);
}

Status
validate_research_plan_no_leakage(std::span<const ResearchObservation> canonical_observations,
                                  const ResearchValidationPlan &plan) {
  ATX_TRY_VOID(validate_canonical_observations(canonical_observations));
  ATX_TRY(auto expected, make_plan_impl(canonical_observations, plan.spec));
  if (expected != plan) {
    return Err(ErrorCode::InvalidArgument,
               "stored research validation plan differs from canonical leak-free plan");
  }
  return Ok();
}

Result<ResearchReturnStats>
compute_research_return_stats(std::span<const ResearchReturnObservation> returns,
                              std::size_t newey_west_lag,
                              const ResearchSelectionAdjustment &selection) {
  if (!selection.family_sealed || selection.attempted_trials == 0u ||
      !finite(selection.successful_trial_sharpe_variance) ||
      selection.successful_trial_sharpe_variance < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "return statistics require a sealed, nonempty trial family and valid variance");
  }
  if (returns.size() < 2u || newey_west_lag >= returns.size()) {
    return Err(ErrorCode::InvalidArgument,
               "return statistics require at least two rows and Newey-West lag < row count");
  }
  for (std::size_t index = 0u; index < returns.size(); ++index) {
    const ResearchReturnObservation &row = returns[index];
    if (!finite(row.pnl) || !finite(row.lagged_capital) || row.lagged_capital <= 0.0 ||
        !finite(row.value)) {
      return Err(ErrorCode::InvalidArgument, "research returns contain invalid numeric values");
    }
    const double expected_value = row.pnl / row.lagged_capital;
    if (!atx::core::isclose(row.value, expected_value, 1.0e-12, 1.0e-15)) {
      return Err(ErrorCode::InvalidArgument,
                 "research return value is inconsistent with PnL and lagged capital");
    }
    if (index > 0u && returns[index - 1u].decision_ts_ns >= row.decision_ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "research return timestamps must be strictly increasing");
    }
  }

  ResearchReturnStats stats;
  stats.n_observations = returns.size();
  stats.attempted_trials = selection.attempted_trials;
  stats.newey_west_lag = newey_west_lag;
  for (const ResearchReturnObservation &row : returns) {
    stats.mean += row.value;
  }
  stats.mean /= static_cast<double>(returns.size());
  if (!finite(stats.mean)) {
    return Err(ErrorCode::OutOfRange, "research return mean overflowed finite arithmetic");
  }

  double centered_sum_squares = 0.0;
  double centered_sum_cubes = 0.0;
  double centered_sum_fourths = 0.0;
  for (const ResearchReturnObservation &row : returns) {
    const double deviation = row.value - stats.mean;
    const double squared = deviation * deviation;
    centered_sum_squares += squared;
    centered_sum_cubes += squared * deviation;
    centered_sum_fourths += squared * squared;
  }
  if (!finite(centered_sum_squares) || !finite(centered_sum_cubes) ||
      !finite(centered_sum_fourths)) {
    return Err(ErrorCode::OutOfRange, "research return moments overflowed finite arithmetic");
  }
  stats.sample_standard_deviation =
      std::sqrt(centered_sum_squares / static_cast<double>(returns.size() - 1u));
  if (centered_sum_squares > 0.0) {
    const double population_variance = centered_sum_squares / static_cast<double>(returns.size());
    stats.skewness = (centered_sum_cubes / static_cast<double>(returns.size())) /
                     std::pow(population_variance, 1.5);
    stats.pearson_kurtosis = (centered_sum_fourths / static_cast<double>(returns.size())) /
                             (population_variance * population_variance);
    stats.sharpe = stats.mean / stats.sample_standard_deviation;
  }
  if (!finite(stats.sample_standard_deviation) || !finite(stats.skewness) ||
      !finite(stats.pearson_kurtosis) || !finite(stats.sharpe)) {
    return Err(ErrorCode::OutOfRange,
               "research return standardized moments overflowed finite arithmetic");
  }

  stats.newey_west_long_run_variance = centered_sum_squares / static_cast<double>(returns.size());
  for (std::size_t lag = 1u; lag <= newey_west_lag; ++lag) {
    double autocovariance = 0.0;
    for (std::size_t index = lag; index < returns.size(); ++index) {
      autocovariance +=
          (returns[index].value - stats.mean) * (returns[index - lag].value - stats.mean);
    }
    autocovariance /= static_cast<double>(returns.size());
    const double weight = 1.0 - static_cast<double>(lag) / static_cast<double>(newey_west_lag + 1u);
    stats.newey_west_long_run_variance += 2.0 * weight * autocovariance;
  }
  if (!finite(stats.newey_west_long_run_variance)) {
    return Err(ErrorCode::OutOfRange, "Newey-West long-run variance overflowed finite arithmetic");
  }
  stats.newey_west_long_run_variance = std::max(0.0, stats.newey_west_long_run_variance);
  stats.hac_mean_standard_error =
      std::sqrt(stats.newey_west_long_run_variance / static_cast<double>(returns.size()));
  if (stats.hac_mean_standard_error > 0.0) {
    stats.hac_t_statistic = stats.mean / stats.hac_mean_standard_error;
    stats.one_sided_p_value = 1.0 - atx::core::norm_cdf(stats.hac_t_statistic);
  }
  if (!finite(stats.hac_mean_standard_error) || !finite(stats.hac_t_statistic) ||
      !finite(stats.one_sided_p_value)) {
    return Err(ErrorCode::OutOfRange, "Newey-West inference overflowed finite arithmetic");
  }

  stats.probabilistic_sharpe_probability =
      sharpe_probability(stats.sharpe, 0.0, stats.skewness, stats.pearson_kurtosis, returns.size());
  if (selection.attempted_trials > 1u && selection.successful_trial_sharpe_variance > 0.0) {
    const double trials = static_cast<double>(selection.attempted_trials);
    const double first_quantile = inverse_normal_cdf(1.0 - 1.0 / trials);
    const double second_quantile = inverse_normal_cdf(1.0 - 1.0 / (trials * std::exp(1.0)));
    stats.deflated_sharpe_threshold =
        std::sqrt(selection.successful_trial_sharpe_variance) *
        ((1.0 - kEulerMascheroni) * first_quantile + kEulerMascheroni * second_quantile);
  }
  stats.deflated_sharpe_probability =
      sharpe_probability(stats.sharpe, stats.deflated_sharpe_threshold, stats.skewness,
                         stats.pearson_kurtosis, returns.size());
  if (!finite(stats.probabilistic_sharpe_probability) ||
      !finite(stats.deflated_sharpe_probability) || !finite(stats.deflated_sharpe_threshold)) {
    return Err(ErrorCode::OutOfRange, "Sharpe inference overflowed finite arithmetic");
  }

  double cumulative = 0.0;
  double peak = 0.0;
  for (const ResearchReturnObservation &row : returns) {
    cumulative += row.value;
    if (!finite(cumulative)) {
      return Err(ErrorCode::OutOfRange, "research cumulative return overflowed finite arithmetic");
    }
    peak = std::max(peak, cumulative);
    stats.max_drawdown = std::max(stats.max_drawdown, peak - cumulative);
  }
  if (!finite(stats.max_drawdown)) {
    return Err(ErrorCode::OutOfRange, "research maximum drawdown overflowed finite arithmetic");
  }
  return Ok(stats);
}

Result<ResearchCandidateEvaluation> evaluate_research_signal_candidate(
    std::span<const ResearchObservation> canonical_observations, const ResearchValidationPlan &plan,
    const ResearchSignalCandidate &candidate, std::size_t newey_west_lag,
    const ResearchSelectionAdjustment &selection) {
  ATX_TRY_VOID(validate_research_plan_no_leakage(canonical_observations, plan));
  ATX_TRY_VOID(validate_candidate(candidate));
  if (plan.folds.empty()) {
    return Err(ErrorCode::InvalidArgument, "research validation plan has no folds");
  }

  const TransformCache cache = make_transform_cache(canonical_observations);
  const std::vector<std::size_t> &in_sample_indices = plan.folds.front().train_indices;
  std::vector<std::size_t> oos_indices;
  for (const ResearchValidationFold &fold : plan.folds) {
    oos_indices.insert(oos_indices.end(), fold.test_indices.begin(), fold.test_indices.end());
  }

  ResearchCandidateEvaluation evaluation;
  evaluation.candidate = candidate;
  evaluation.candidate_identity = candidate_identity(candidate);
  ATX_TRY(evaluation.in_sample_returns,
          build_returns(canonical_observations, cache, candidate, in_sample_indices));
  ATX_TRY(evaluation.oos_returns,
          build_returns(canonical_observations, cache, candidate, oos_indices));
  ATX_TRY(evaluation.in_sample_stats,
          compute_research_return_stats(evaluation.in_sample_returns, newey_west_lag, selection));
  ATX_TRY(evaluation.oos_stats,
          compute_research_return_stats(evaluation.oos_returns, newey_west_lag, selection));
  return Ok(std::move(evaluation));
}

Result<ResearchMiningResult>
mine_research_signal_candidates(std::span<const ResearchObservation> canonical_observations,
                                const ResearchValidationPlan &plan,
                                std::span<const ResearchSignalCandidate> candidates,
                                std::size_t newey_west_lag, const ResearchTrialFamily &family) {
  if (!family.sealed || family.attempted_trials == 0u ||
      family.attempted_trials < candidates.size()) {
    return Err(ErrorCode::InvalidArgument,
               "mining requires a sealed family counting every submitted candidate");
  }
  if (candidates.empty()) {
    return Err(ErrorCode::InvalidArgument, "research mining candidate family is empty");
  }

  std::vector<std::pair<std::uint64_t, ResearchSignalCandidate>> ordered;
  ordered.reserve(candidates.size());
  std::vector<std::string> candidate_ids;
  candidate_ids.reserve(candidates.size());
  for (const ResearchSignalCandidate &candidate : candidates) {
    ATX_TRY_VOID(validate_candidate(candidate));
    ordered.emplace_back(candidate_identity(candidate), candidate);
    candidate_ids.push_back(candidate.id);
  }
  std::sort(candidate_ids.begin(), candidate_ids.end());
  if (std::adjacent_find(candidate_ids.begin(), candidate_ids.end()) != candidate_ids.end()) {
    return Err(ErrorCode::AlreadyExists, "research mining candidate ids must be unique");
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &left, const auto &right) { return left.first < right.first; });
  for (std::size_t index = 1u; index < ordered.size(); ++index) {
    if (ordered[index - 1u].first == ordered[index].first) {
      return Err(ErrorCode::AlreadyExists, "research mining candidate identities must be unique");
    }
  }

  ResearchMiningResult result;
  result.evaluations.reserve(ordered.size());
  const ResearchSelectionAdjustment provisional{true, family.attempted_trials, 0.0};
  for (const auto &[identity, candidate] : ordered) {
    static_cast<void>(identity);
    ATX_TRY(auto evaluation,
            evaluate_research_signal_candidate(canonical_observations, plan, candidate,
                                               newey_west_lag, provisional));
    result.evaluations.push_back(std::move(evaluation));
  }

  std::vector<double> sharpes;
  sharpes.reserve(result.evaluations.size());
  for (const ResearchCandidateEvaluation &evaluation : result.evaluations) {
    sharpes.push_back(evaluation.oos_stats.sharpe);
  }
  const ResearchSelectionAdjustment adjusted{true, family.attempted_trials,
                                             sample_variance(sharpes)};
  for (ResearchCandidateEvaluation &evaluation : result.evaluations) {
    ATX_TRY(evaluation.in_sample_stats,
            compute_research_return_stats(evaluation.in_sample_returns, newey_west_lag, adjusted));
    ATX_TRY(evaluation.oos_stats,
            compute_research_return_stats(evaluation.oos_returns, newey_west_lag, adjusted));
  }

  const auto best = std::max_element(
      result.evaluations.begin(), result.evaluations.end(),
      [](const ResearchCandidateEvaluation &left, const ResearchCandidateEvaluation &right) {
        if (left.oos_stats.hac_t_statistic != right.oos_stats.hac_t_statistic) {
          return left.oos_stats.hac_t_statistic < right.oos_stats.hac_t_statistic;
        }
        if (left.oos_stats.mean != right.oos_stats.mean) {
          return left.oos_stats.mean < right.oos_stats.mean;
        }
        if (left.candidate.lookback != right.candidate.lookback) {
          return left.candidate.lookback > right.candidate.lookback;
        }
        if (left.candidate.lag != right.candidate.lag) {
          return left.candidate.lag > right.candidate.lag;
        }
        return left.candidate_identity > right.candidate_identity;
      });
  result.selected_candidate_id = best->candidate.id;
  result.selected_candidate_identity = best->candidate_identity;
  return Ok(std::move(result));
}

Result<std::vector<double>> holm_adjusted_p_values(std::span<const double> raw_p_values) {
  ATX_TRY(auto values, validate_raw_p_values(raw_p_values));
  std::vector<std::size_t> order(values.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&values](std::size_t left, std::size_t right) {
    return values[left] < values[right];
  });
  std::vector<double> adjusted(values.size());
  double running_max = 0.0;
  for (std::size_t rank = 0u; rank < order.size(); ++rank) {
    const double scaled = static_cast<double>(order.size() - rank) * values[order[rank]];
    running_max = std::max(running_max, scaled);
    adjusted[order[rank]] = std::min(1.0, running_max);
  }
  return Ok(std::move(adjusted));
}

Result<std::vector<double>>
benjamini_yekutieli_adjusted_p_values(std::span<const double> raw_p_values) {
  ATX_TRY(auto values, validate_raw_p_values(raw_p_values));
  std::vector<std::size_t> order(values.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&values](std::size_t left, std::size_t right) {
    return values[left] < values[right];
  });
  double harmonic = 0.0;
  for (std::size_t index = 1u; index <= values.size(); ++index) {
    harmonic += 1.0 / static_cast<double>(index);
  }
  std::vector<double> adjusted(values.size());
  double running_min = 1.0;
  for (std::size_t reverse_rank = order.size(); reverse_rank > 0u; --reverse_rank) {
    const std::size_t rank = reverse_rank - 1u;
    const double scaled = static_cast<double>(values.size()) * harmonic * values[order[rank]] /
                          static_cast<double>(rank + 1u);
    running_min = std::min(running_min, scaled);
    adjusted[order[rank]] = std::min(1.0, running_min);
  }
  return Ok(std::move(adjusted));
}

Result<ResearchPromotionDecision>
evaluate_research_promotion(const ResearchPromotionEvidence &evidence,
                            const ResearchPromotionGateSpec &spec) {
  if (spec.min_oos_observations == 0u || !finite(spec.min_hac_t_statistic) ||
      !finite(spec.min_deflated_sharpe_probability) || spec.min_deflated_sharpe_probability < 0.0 ||
      spec.min_deflated_sharpe_probability > 1.0 || !finite(spec.max_adjusted_p_value) ||
      spec.max_adjusted_p_value < 0.0 || spec.max_adjusted_p_value > 1.0 ||
      !finite(spec.max_drawdown) || spec.max_drawdown < 0.0) {
    return Err(ErrorCode::InvalidArgument, "research promotion thresholds are invalid");
  }

  ResearchPromotionDecision decision;
  decision.gates.reserve(11u);
  decision.gates.push_back(boolean_gate(ResearchPromotionGateCode::SourceLineage,
                                        evidence.source_lineage_complete,
                                        "source lineage must be complete"));
  decision.gates.push_back(boolean_gate(ResearchPromotionGateCode::FamilySealed,
                                        evidence.family_sealed,
                                        "experiment family must be sealed"));
  decision.gates.push_back(boolean_gate(ResearchPromotionGateCode::IndependentValidation,
                                        evidence.independent_validation_passed,
                                        "independent validation must pass"));
  decision.gates.push_back(boolean_gate(ResearchPromotionGateCode::HoldoutConsumed,
                                        evidence.holdout_consumed,
                                        "holdout must be explicitly consumed"));
  decision.gates.push_back(boolean_gate(ResearchPromotionGateCode::CostStress,
                                        evidence.cost_stress_passed, "cost stress must pass"));
  decision.gates.push_back(boolean_gate(ResearchPromotionGateCode::Concentration,
                                        evidence.concentration_passed,
                                        "concentration limits must pass"));

  decision.gates.push_back(ResearchPromotionGateResult{
      ResearchPromotionGateCode::MinimumObservations,
      evidence.oos_stats.n_observations >= spec.min_oos_observations,
      static_cast<double>(evidence.oos_stats.n_observations),
      static_cast<double>(spec.min_oos_observations), "minimum stitched OOS observation count"});

  const bool t_valid = finite(evidence.oos_stats.hac_t_statistic);
  decision.gates.push_back(ResearchPromotionGateResult{
      ResearchPromotionGateCode::HacTStatistic,
      t_valid && evidence.oos_stats.hac_t_statistic >= spec.min_hac_t_statistic,
      t_valid ? evidence.oos_stats.hac_t_statistic : 0.0, spec.min_hac_t_statistic,
      t_valid ? "one-sided Newey-West HAC t-statistic" : "HAC t-statistic is non-finite"});

  const bool dsr_valid = finite(evidence.oos_stats.deflated_sharpe_probability);
  decision.gates.push_back(ResearchPromotionGateResult{
      ResearchPromotionGateCode::DeflatedSharpe,
      dsr_valid &&
          evidence.oos_stats.deflated_sharpe_probability >= spec.min_deflated_sharpe_probability,
      dsr_valid ? evidence.oos_stats.deflated_sharpe_probability : 0.0,
      spec.min_deflated_sharpe_probability,
      dsr_valid ? "deflated Sharpe probability" : "deflated Sharpe is non-finite"});

  const bool p_valid = finite(evidence.adjusted_p_value);
  decision.gates.push_back(ResearchPromotionGateResult{
      ResearchPromotionGateCode::AdjustedPValue,
      p_valid && evidence.adjusted_p_value <= spec.max_adjusted_p_value,
      p_valid ? evidence.adjusted_p_value : 1.0, spec.max_adjusted_p_value,
      p_valid ? "family-adjusted p-value" : "adjusted p-value is non-finite"});

  const bool drawdown_valid = finite(evidence.oos_stats.max_drawdown);
  decision.gates.push_back(ResearchPromotionGateResult{
      ResearchPromotionGateCode::MaximumDrawdown,
      drawdown_valid && evidence.oos_stats.max_drawdown <= spec.max_drawdown,
      drawdown_valid ? evidence.oos_stats.max_drawdown : 0.0, spec.max_drawdown,
      drawdown_valid ? "maximum additive drawdown" : "maximum drawdown is non-finite"});

  decision.promoted =
      std::all_of(decision.gates.begin(), decision.gates.end(),
                  [](const ResearchPromotionGateResult &gate) { return gate.passed; });
  return Ok(std::move(decision));
}

} // namespace atx::vol
