// mv-aim-v1 (P9 lane COV-MV): the NAV target rule over the engine kernel
// (engine::book::mv_aim) and its printed block. The rule is stated in strategy_mv_aim.hpp; the
// v7 seam (strategy_nav_v7.cpp) parses --target-rule mv-aim-v1, owns the Rule and calls it from
// the NAV path's desired-target extension point (book/nav_decide.cpp).
#include "strategy_mv_aim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/safe_math.hpp"

namespace atx::impl::strategy::mv_aim {
namespace {
using namespace atx;
namespace co = atx::core;
namespace eb = atx::engine::book;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();

bool finite_positive(f64 x) { return std::isfinite(x) && x > 0; }
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
Json stats_json(const Stats& s) {
  return Json{{"n", s.n},
              {"mean", finite_or_null(s.n ? s.sum / static_cast<f64>(s.n) : nan)},
              {"min", finite_or_null(s.n ? s.lo : nan)},
              {"max", finite_or_null(s.n ? s.hi : nan)}};
}
// Grows v once to exactly n entries of capacity (workspace_bytes counts exactly that).
template <class T> void reserve_exact(std::vector<T>& v, usize n) {
  if (v.capacity() >= n) return;
  std::vector<T> fresh;
  fresh.reserve(n);
  v.swap(fresh);
}
co::Status invalid(std::string what) {
  return co::Err(co::ErrorCode::InvalidArgument, "mv-aim-v1: " + std::move(what));
}
} // namespace

void Stats::add(f64 v) noexcept {
  if (!std::isfinite(v)) return;
  lo = n ? std::min(lo, v) : v;
  hi = n ? std::max(hi, v) : v;
  sum += v;
  ++n;
}

std::string declaration() {
  return "mv-aim-v1 (P9 COV-MV, cov-design section 5.2; Rulings COV-7, W3-17, PRE-24a, COV-13 "
         "(c)): at every rebalance decision d whose shared desired target proceeds, the priced "
         "members (a member with a finite desired weight and a risk row at d: industry slot "
         "0..49 and a finite specific variance D_i > 0) take g x w, w = P V_d^-1 P alpha / "
         "sum |P V_d^-1 P alpha| over them, alpha_i = sigma_i x desired_i with sigma_i = "
         "sqrt(D_i), P the dollar-neutral projection on the priced names, V_d = X F X' + D of "
         "row d of the pinned store (--risk-model; RiskStore::read(d), the index the risk "
         "target and spo read: nothing after session d), solved as u - D^-1 X y with (I + F G) "
         "y = F X' u, u = D^-1 P alpha, G = X' D^-1 X (F is never inverted; defined for a "
         "singular F), and g = the desired target's gross over the priced names (the priced "
         "block keeps it and is dollar neutral); unpriced members and nonmembers keep their "
         "weights, so the total gross is the desired target's. No constant: an IC, horizon or "
         "the score's SD washes out under the normalisation; no ridge, cap, clip or floor. "
         "Unchanged (counted): no forecast at d, fewer than two priced names, a flat tilt. "
         "Construction (aim-partial-v5) and the leverage rule follow unchanged; under "
         "--locate-in-aim a special-tier name can take a negative target (the books' locate "
         "block still applies at the plan; counted)";
}

u64 workspace_bytes(usize names) noexcept {
  using co::sat_add;
  using co::sat_mul;
  const u64 n = names;
  constexpr u64 k = spo::risk_factors, s = spo::risk_styles;
  constexpr u64 f = sizeof(f64);
  // Row d (RiskStore::read: slot, styles, specific, covariance and its f32 style buffer).
  const u64 row_name = 1U + s * f + f + s * sizeof(f32);
  u64 total = sat_add(sat_mul(n, row_name), k * k * f);
  // The priced view and its target (group, exposures, specific, score, sigma, out, index) and
  // the print's weights.
  const u64 view_name = sizeof(u32) + s * f + 5U * f + sizeof(usize);
  total = sat_add(total, sat_mul(n, view_name));
  total = sat_add(total, eb::mv_aim_working_bytes(names, spo::risk_industry_slots, s));
  // The eigen print: the block, Eigen's result and solver (K x K each, bounded by four) and their
  // K-vectors, the block's factor list (its growth included) and the exposure vector.
  total = sat_add(total, 4U * k * k * f + 4U * k * f + 2U * k * sizeof(usize) + k * f);
  return total;
}

Rule::Rule(std::shared_ptr<const spo::RiskStore> store) : store_(std::move(store)) {}

void Rule::begin_run() {
  formed_ = false;
  last_ = 0;
  counts_ = Counts{};
}

co::Status Rule::apply(const TargetReplayInput& x, usize d, std::span<const u8> no_short,
                       std::vector<f64>& desired) {
  if (!store_) return invalid("no risk model (--risk-model)");
  const usize n = x.instruments;
  if (d >= x.dates || desired.size() != n || x.member.size() != x.dates * n ||
      x.instrument_ids.size() != n || (!no_short.empty() && no_short.size() != n))
    return invalid("decision geometry");
  ATX_TRY_VOID(store_->check_axes(x.session_keys, n));
  // Counted once per decision index: the first time the run forms it (Ruling COV-4 trace: the
  // decision at d, after d's mark, first traded at d + 1's close).
  const bool fresh = !formed_ || d > last_;
  if (fresh) {
    formed_ = true;
    last_ = d;
  }
  const bool scored = d >= x.decision_begin;
  counts_.locate_in_aim = counts_.locate_in_aim || !no_short.empty();
  const auto pass_through = [&](usize Counts::*outcome) {
    if (!fresh) return;
    if (!scored) {
      ++counts_.warm_up;
      return;
    }
    ++counts_.decisions;
    ++(counts_.*outcome);
  };
  if (auto row = store_->read(d, slice_); !row) {
    if (row.error().code() != co::ErrorCode::Unavailable) return row;
    pass_through(&Counts::no_forecast);
    return co::Ok();
  }
  // Ruling COV-11 (S11): every slot of row d is an industry (0..49) or no row (255).
  for (usize i = 0; i < n; ++i) {
    const u8 slot = slice_.slot[i];
    if (slot < spo::risk_industry_slots || slot == spo::risk_no_row) continue;
    return invalid("risk model row " + std::to_string(d) + " (session " +
                   std::to_string(slice_.session) + ") gives instrument " +
                   std::to_string(x.instrument_ids[i]) + " the industry slot " +
                   std::to_string(slot) + " (neither 0..49 nor 255; Ruling COV-11)");
  }
  for (auto* v : {&specific_, &score_, &sigma_, &out_, &weights_}) reserve_exact(*v, n);
  reserve_exact(exposures_, n * spo::risk_styles);
  reserve_exact(group_, n);
  reserve_exact(index_, n);
  reserve_exact(exposure_, spo::risk_factors);
  group_.clear(); exposures_.clear(); specific_.clear(); score_.clear(); sigma_.clear();
  index_.clear();
  usize unpriced = 0;
  const auto member = x.member.subspan(d * n, n);
  for (usize i = 0; i < n; ++i) {
    if (!member[i] || !std::isfinite(desired[i])) continue; // a nonmember; NaN: no opinion
    const u8 slot = slice_.slot[i];
    const f64 specific = slice_.specific[i];
    if (slot >= spo::risk_industry_slots || !finite_positive(specific)) {
      ++unpriced;
      continue;
    }
    group_.push_back(1U + static_cast<u32>(slot));
    const auto styles = slice_.styles.begin() + static_cast<std::ptrdiff_t>(i * spo::risk_styles);
    exposures_.insert(exposures_.end(), styles,
                      styles + static_cast<std::ptrdiff_t>(spo::risk_styles));
    specific_.push_back(specific);
    score_.push_back(desired[i]);
    sigma_.push_back(std::sqrt(specific));
    index_.push_back(i);
  }
  const usize priced = index_.size();
  if (priced < 2) {
    pass_through(&Counts::fewer_than_two_priced);
    return co::Ok();
  }
  const eb::FactorRiskView view{spo::risk_industry_slots, spo::risk_styles, group_, exposures_,
                                slice_.covariance, specific_};
  out_.assign(priced, 0.0);
  if (auto tilt = eb::mv_aim(view, score_, sigma_, out_, scratch_); !tilt) {
    if (tilt.error().code() != co::ErrorCode::Unavailable) return co::Err(tilt.error());
    pass_through(&Counts::flat);
    return co::Ok();
  }
  // Ruling W3-17: the priced block keeps the desired target's gross over the priced names.
  f64 gross = 0;
  for (const f64 s : score_) gross += std::abs(s);
  for (usize k = 0; k < priced; ++k) desired[index_[k]] = gross * out_[k];
  if (!fresh) return co::Ok();
  if (!scored) {
    ++counts_.warm_up;
    ++counts_.warm_up_applied;
    return co::Ok();
  }
  ++counts_.decisions;
  ++counts_.applied;
  counts_.priced.add(static_cast<f64>(priced));
  counts_.unpriced.add(static_cast<f64>(unpriced));
  return record(no_short, desired, gross);
}

co::Status Rule::record(std::span<const u8> no_short, const std::vector<f64>& desired,
                        f64 gross) {
  const usize priced = index_.size();
  // Ruling PRE-24a: the gross-weighted mean specific volatility of the target over that of the
  // desired target, on the priced names (both have gross g there, so the ratio of the sums).
  weights_.assign(priced, 0.0);
  f64 target = 0, before = 0;
  for (usize k = 0; k < priced; ++k) {
    weights_[k] = gross * out_[k];
    target += std::abs(weights_[k]) * sigma_[k];
    before += std::abs(score_[k]) * sigma_[k];
  }
  counts_.pooled_target += target;
  counts_.pooled_desired += before;
  counts_.ratio.add(before > 0 ? target / before : nan);
  // Ruling COV-13 (c): the share of e' F_d e on eigen ranks 00-02, e = X' w over the priced names.
  const eb::FactorRiskView view{spo::risk_industry_slots, spo::risk_styles, group_, exposures_,
                                slice_.covariance, specific_};
  ATX_TRY_VOID(eb::factor_eigen(slice_.covariance, spo::risk_factors, eigen_));
  exposure_.assign(spo::risk_factors, 0.0);
  ATX_TRY_VOID(eb::factor_exposure(view, weights_, exposure_));
  counts_.share_mv.add(eb::low_eigen_share(eigen_, exposure_, low_ranks));
  ATX_TRY_VOID(eb::factor_exposure(view, score_, exposure_));
  counts_.share_desired.add(eb::low_eigen_share(eigen_, exposure_, low_ranks));
  // Under --locate-in-aim: special-tier names the target puts short.
  usize negative = 0;
  for (usize i = 0; i < no_short.size(); ++i)
    if (no_short[i] != 0 && desired[i] < 0) ++negative;
  counts_.negative_names += negative;
  counts_.negative_decisions += negative > 0 ? 1U : 0U;
  return co::Ok();
}

Json Rule::parameters_json() const {
  Json block{{"rule", std::string(rule_id)}, {"declaration", declaration()}};
  block["risk_model"] = store_ ? Json{{"directory", store_->directory()},
                                      {"manifest_sha256", store_->manifest_sha256()}}
                               : Json(nullptr);
  return block;
}

Json Rule::summary_json() const {
  const auto& c = counts_;
  Json out = parameters_json();
  out["decisions"] =
      Json{{"formed", c.decisions},
           {"applied", c.applied},
           {"pass_through", Json{{"no_forecast", c.no_forecast},
                                 {"fewer_than_two_priced", c.fewer_than_two_priced},
                                 {"flat", c.flat}}},
           {"scope", "scored rebalance decisions whose desired target proceeded, each counted "
                     "once (the first time the run formed it)"}};
  out["warm_up_decisions"] = Json{{"formed", c.warm_up}, {"applied", c.warm_up_applied}};
  out["priced_names"] = stats_json(c.priced);
  out["unpriced_members"] = stats_json(c.unpriced);
  out["specific_vol_ratio"] = Json{
      {"pooled", finite_or_null(c.pooled_desired > 0 ? c.pooled_target / c.pooled_desired : nan)},
      {"per_decision", stats_json(c.ratio)},
      {"definition", "Ruling PRE-24a: sum |w_i| sigma_i / sum |desired_i| sigma_i over the "
                     "priced names of an applied decision (equal gross there), sigma_i = "
                     "sqrt(D_i) of row d; pooled = the ratio of the sums over decisions"}};
  out["eigen_00_02_share"] = Json{
      {"mv", stats_json(c.share_mv)},
      {"desired", stats_json(c.share_desired)},
      {"definition", "Ruling COV-13 (c): sum_{r < 3} lambda_r (v_r' e)^2 / e' F_d e over the "
                     "forecast block of F_d (F_jj > 0), ranks ascending (00 = the smallest), "
                     "e = X' w of the priced names (mv: the target, desired: the desired "
                     "target)"}};
  out["special_tier_negative"] = Json{{"locate_in_aim", c.locate_in_aim},
                                      {"name_decisions", c.negative_names},
                                      {"decisions", c.negative_decisions}};
  return out;
}

} // namespace atx::impl::strategy::mv_aim
