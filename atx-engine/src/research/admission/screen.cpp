#include "atx/engine/research/admission/screen.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <new>
#include <string>
#include <utility>

#include "atx/engine/eval/hac.hpp"

namespace atx::engine::research::admission {
namespace {

namespace hac = atx::engine::eval::hac;

[[nodiscard]] core::Error invalid(const std::string &message) {
  return core::Error(core::ErrorCode::InvalidArgument, "admission screen: " + message);
}

// fit_composition_weights.newey_west_t on finite values.
[[nodiscard]] std::optional<f64> fitter_newey_west_t(std::span<const f64> x, usize lag) noexcept {
  const usize n = x.size();
  if (n < 2U) {
    return std::nullopt;
  }
  // A constant series has no defined t (rounding would invent one): exact equality, as the fitter.
  if (std::all_of(x.begin(), x.end(), [&](f64 v) { return v == x[0]; })) {
    return std::nullopt;
  }
  if (n > lag) {
    // mean_inference clamps its lag to n - 1; inactive here, so its weights are the fitter's.
    const hac::MeanInference inference =
        hac::mean_inference(x, hac::Kernel::BartlettV1, lag, false);
    if (inference.defined == 0U) {
      return std::nullopt;
    }
    return inference.t;
  }
  // n <= lag: lags 1..n-1 only, each weighted 1 - l / (lag + 1) at the declared lag
  // (fit:1579-1580), on the engine's own primitives (ascending sums) as the branch above.
  const f64 mean = hac::detail::mean_of(x);
  f64 s = hac::detail::lag_cross_product(x, mean, 0U);
  for (usize l = 1U; l < n; ++l) {
    s += 2.0 * hac::kernel_weight(hac::Kernel::BartlettV1, l, lag) *
         hac::detail::lag_cross_product(x, mean, l);
  }
  const f64 nf = static_cast<f64>(n);
  const f64 var = s / (nf * nf);
  if (!(var > 0.0) || !std::isfinite(var) || !std::isfinite(mean)) {
    return std::nullopt;
  }
  return mean / std::sqrt(var);
}

// The live TRAIN values of candidate k, ascending by decision.
void live_values(const ScreenInput &input, usize k, std::vector<f64> &out) {
  out.clear();
  for (usize d = 0; d < input.decisions; ++d) {
    const f64 v = input.factors[d * input.candidates + k];
    if (input.train_mask[d] != 0U && std::isfinite(v)) {
      out.push_back(v);
    }
  }
}

[[nodiscard]] core::Status validate(const ScreenInput &input, const ScreenRules &rules) {
  const usize k = input.candidates;
  if (k == 0U || input.decisions == 0U) {
    return core::Err(invalid("no candidate or no decision"));
  }
  if (input.factors.size() / k != input.decisions || input.factors.size() % k != 0U ||
      input.taus.size() != k || input.train_mask.size() != input.decisions ||
      input.tier_rank.size() != k || input.prior_signs.size() != k) {
    return core::Err(invalid("inconsistent geometry"));
  }
  if (std::any_of(input.train_mask.begin(), input.train_mask.end(), [](u8 m) { return m > 1U; })) {
    return core::Err(invalid("a train mask byte above 1"));
  }
  if (std::any_of(input.prior_signs.begin(), input.prior_signs.end(),
                  [](i32 s) { return s != 0 && s != 1; })) {
    return core::Err(
        invalid("a prior sign other than 0 or 1 (v4 embeds the prior sign in the DSL)"));
  }
  if (hac_method_name(rules.hac_method).empty()) {
    return core::Err(invalid("unknown HAC method"));
  }
  return core::Ok();
}

// The checks of one candidate, in declared order, and its statistics.
[[nodiscard]] ScreenRow screen_row(const ScreenInput &input, const ScreenRules &rules, usize k,
                                   std::span<const f64> live) {
  ScreenRow row;
  const MeanSharpe stats = mean_sharpe(live, rules.sharpe_annualization);
  row.s_k = input.prior_signs[k];
  row.tau = input.taus[k];
  row.train_days = live.size();
  row.train_mean = stats.mean;
  row.train_sharpe = stats.sharpe;
  row.hac_t = hac_t(live, rules.hac_method, rules.hac_lag);
  if (row.s_k == 0) {
    row.failed_checks.push_back(Check::NoPrior);
  }
  if (row.train_days < rules.min_train_days) {
    row.failed_checks.push_back(Check::Insufficient);
  }
  if (row.tau > rules.tau_limit) {
    row.failed_checks.push_back(Check::Turnover);
  }
  if (rules.cost_tau_limit && row.tau > *rules.cost_tau_limit) {
    row.failed_checks.push_back(Check::TurnoverCost);
  }
  if (row.hac_t && *row.hac_t < rules.veto_t) {
    row.failed_checks.push_back(Check::Veto);
  }
  return row;
}

[[nodiscard]] Status reject_status(Check first) noexcept {
  switch (first) {
  case Check::NoPrior:
    return Status::RejectNoPrior;
  case Check::Insufficient:
    return Status::RejectInsufficient;
  case Check::Turnover:
    return Status::RejectTurnover;
  case Check::TurnoverCost:
    return Status::RejectTurnoverCost;
  case Check::Veto:
    return Status::RejectVeto;
  }
  return Status::RejectVeto; // unreachable for valid enumerators
}

// The greedy redundancy pass over the survivors in (tier rank, roster index) order (fit:1640-1660).
void redundancy_pass(const ScreenInput &input, const ScreenRules &rules,
                     std::vector<ScreenRow> &rows, const std::vector<u8> &survives) {
  std::vector<usize> order;
  for (usize k = 0; k < rows.size(); ++k) {
    if (survives[k] != 0U) {
      order.push_back(k);
    }
  }
  std::sort(order.begin(), order.end(), [&](usize a, usize b) {
    return input.tier_rank[a] != input.tier_rank[b] ? input.tier_rank[a] < input.tier_rank[b]
                                                     : a < b;
  });
  std::vector<usize> admitted;
  for (const usize k : order) {
    std::optional<f64> worst;
    usize worst_with = 0U;
    for (const usize j : admitted) {
      const PairRho pair = pair_correlation(input, k, j);
      if (pair.days < rules.min_common_days) {
        rows[k].low_overlap_with.push_back(j);
        continue;
      }
      if (!pair.rho) {
        rows[k].undefined_rho_with.push_back(j);
        continue;
      }
      const f64 magnitude = std::fabs(*pair.rho);
      if (magnitude > rules.rho_limit && (!worst || magnitude > *worst)) {
        worst = magnitude;
        worst_with = j;
      }
    }
    if (!worst) {
      admitted.push_back(k);
      rows[k].status = Status::Admitted;
      rows[k].admission_rank = admitted.size();
    } else {
      rows[k].status = Status::RejectRedundant;
      rows[k].redundant_with = worst_with;
      rows[k].redundant_rho = worst;
    }
  }
  // max |rho| of every row against the admitted set (fit:1661-1670).
  for (usize k = 0; k < rows.size(); ++k) {
    for (const usize j : admitted) {
      if (j == k) {
        continue;
      }
      const PairRho pair = pair_correlation(input, k, j);
      if (!pair.rho || pair.days < rules.min_common_days) {
        continue;
      }
      const f64 magnitude = std::fabs(*pair.rho);
      if (!rows[k].max_abs_rho || magnitude > *rows[k].max_abs_rho) {
        rows[k].max_abs_rho = magnitude;
        rows[k].max_abs_rho_with = j;
      }
    }
  }
}

} // namespace

std::string_view hac_method_name(HacMethod method) noexcept {
  switch (method) {
  case HacMethod::FitterNeweyWestV1:
    return "fitter-newey-west-v1";
  case HacMethod::Unknown:
    return "";
  }
  return "";
}

std::optional<f64> hac_t(std::span<const f64> x, HacMethod method, usize lag) noexcept {
  switch (method) {
  case HacMethod::FitterNeweyWestV1:
    return fitter_newey_west_t(x, lag);
  case HacMethod::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

std::string_view screen_name(ScreenId id) noexcept {
  switch (id) {
  case ScreenId::V4PriorV1:
    return "v4-prior-v1";
  case ScreenId::V4PriorV2:
    return "v4-prior-v2";
  case ScreenId::Unknown:
    return "";
  }
  return "";
}

std::optional<ScreenId> screen_from_name(std::string_view name) noexcept {
  for (const ScreenId id : {ScreenId::V4PriorV1, ScreenId::V4PriorV2}) {
    if (name == screen_name(id)) {
      return id;
    }
  }
  return std::nullopt;
}

ScreenRules screen_rules(ScreenId id) noexcept {
  ScreenRules rules{};
  if (id == ScreenId::V4PriorV2) {
    rules.cost_tau_limit = 0.08; // V42_COST_TAU_LIMIT (fit:295)
  }
  return rules;
}

std::string_view check_name(Check check) noexcept {
  switch (check) {
  case Check::NoPrior:
    return "no_prior";
  case Check::Insufficient:
    return "insufficient";
  case Check::Turnover:
    return "turnover";
  case Check::TurnoverCost:
    return "turnover_cost";
  case Check::Veto:
    return "veto";
  }
  return "";
}

std::string_view status_name(Status status) noexcept {
  switch (status) {
  case Status::Admitted:
    return "admitted";
  case Status::RejectNoPrior:
    return "reject_no_prior";
  case Status::RejectInsufficient:
    return "reject_insufficient";
  case Status::RejectTurnover:
    return "reject_turnover";
  case Status::RejectTurnoverCost:
    return "reject_turnover_cost";
  case Status::RejectVeto:
    return "reject_veto";
  case Status::RejectRedundant:
    return "reject_redundant";
  }
  return "";
}

MeanSharpe mean_sharpe(std::span<const f64> x, f64 annualization) noexcept {
  MeanSharpe out;
  if (x.empty()) {
    return out;
  }
  const f64 n = static_cast<f64>(x.size());
  f64 total = 0.0;
  for (const f64 v : x) {
    total += v;
  }
  const f64 mean = total / n;
  out.mean = mean;
  if (x.size() < 2U) {
    return out;
  }
  f64 squares = 0.0;
  for (const f64 v : x) {
    squares += (v - mean) * (v - mean);
  }
  const f64 sd = std::sqrt(squares / (n - 1.0));
  if (sd > 0.0) {
    out.sharpe = mean / sd * std::sqrt(annualization);
  }
  return out;
}

PairRho pair_correlation(const ScreenInput &input, usize a, usize b) noexcept {
  PairRho out;
  const usize k = input.candidates;
  f64 sum_a = 0.0;
  f64 sum_b = 0.0;
  for (usize d = 0; d < input.decisions; ++d) {
    const f64 x = input.factors[d * k + a];
    const f64 y = input.factors[d * k + b];
    if (input.train_mask[d] != 0U && std::isfinite(x) && std::isfinite(y)) {
      ++out.days;
      sum_a += x;
      sum_b += y;
    }
  }
  if (out.days < 2U) {
    return out;
  }
  const f64 n = static_cast<f64>(out.days);
  const f64 mean_a = sum_a / n;
  const f64 mean_b = sum_b / n;
  f64 sxx = 0.0;
  f64 syy = 0.0;
  f64 sxy = 0.0;
  for (usize d = 0; d < input.decisions; ++d) {
    const f64 x = input.factors[d * k + a];
    const f64 y = input.factors[d * k + b];
    if (input.train_mask[d] != 0U && std::isfinite(x) && std::isfinite(y)) {
      const f64 dx = x - mean_a;
      const f64 dy = y - mean_b;
      sxx += dx * dx;
      syy += dy * dy;
      sxy += dx * dy;
    }
  }
  const f64 den = std::sqrt(sxx * syy);
  if (den > 0.0) {
    out.rho = sxy / den;
  }
  return out;
}

core::Result<std::vector<ScreenRow>> screen_v4(const ScreenInput &input,
                                               const ScreenRules &rules) {
  ATX_TRY_VOID(validate(input, rules));
  try {
    std::vector<ScreenRow> rows;
    rows.reserve(input.candidates);
    std::vector<u8> survives(input.candidates, 0U);
    std::vector<f64> live;
    live.reserve(input.decisions);
    for (usize k = 0; k < input.candidates; ++k) {
      live_values(input, k, live);
      rows.push_back(screen_row(input, rules, k, live));
      ScreenRow &row = rows.back();
      if (row.failed_checks.empty()) {
        survives[k] = 1U;
      } else {
        row.status = reject_status(row.failed_checks.front());
      }
    }
    redundancy_pass(input, rules, rows, survives);
    return core::Ok(std::move(rows));
  } catch (const std::bad_alloc &) {
    return core::Err(core::ErrorCode::OutOfRange, "admission screen: allocation failed");
  }
}

std::vector<usize> admitted_order(std::span<const ScreenRow> rows) {
  std::vector<usize> out;
  for (usize k = 0; k < rows.size(); ++k) {
    if (rows[k].status == Status::Admitted && rows[k].admission_rank) {
      out.push_back(k);
    }
  }
  std::sort(out.begin(), out.end(),
            [&](usize a, usize b) { return *rows[a].admission_rank < *rows[b].admission_rank; });
  return out;
}

} // namespace atx::engine::research::admission
