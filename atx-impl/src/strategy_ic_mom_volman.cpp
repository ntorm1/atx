#include "strategy_ic_mom_volman.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include "strategy_ic_rules.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
co::Error refused(const std::string& what) {
  return co::Error(co::ErrorCode::InvalidArgument, std::string(mom_volman_rule) + ": " + what);
}
} // namespace

co::Result<std::vector<MomVolmanBlock>> mom_volman_blocks(std::span<const f64> sleeve) {
  const usize first = mom_volman_lookback + mom_volman_lag - 1U;
  if (sleeve.size() <= first || sleeve.size() > mom_volman_max_decisions)
    return co::Err(refused("the sleeve needs more than " + std::to_string(first) + " and at most " +
                           std::to_string(mom_volman_max_decisions) + " decisions"));
  if (std::any_of(sleeve.begin(), sleeve.end(), [](f64 r) { return !std::isfinite(r); }))
    return co::Err(refused("the sleeve's values must be finite"));
  std::vector<MomVolmanBlock> out;
  out.reserve((sleeve.size() - first + mom_volman_step - 1U) / mom_volman_step);
  f64 sigma_sum = 0.0;
  for (usize j = first; j < sleeve.size(); j += mom_volman_step) {
    f64 squares = 0.0;
    for (usize d = j + 1U - mom_volman_lag - mom_volman_lookback; d <= j - mom_volman_lag; ++d)
      squares += sleeve[d] * sleeve[d];
    const f64 sigma = std::sqrt(squares / static_cast<f64>(mom_volman_lookback));
    if (!std::isfinite(sigma) || !(sigma > 0.0))
      return co::Err(refused("the realised volatility before decision " + std::to_string(j) +
                             " must be finite and > 0"));
    sigma_sum += sigma;
    const f64 target = sigma_sum / static_cast<f64>(out.size() + 1U);
    out.push_back({j, sigma, target, target / sigma});
  }
  return co::Ok(std::move(out));
}
} // namespace atx::impl::strategy

namespace atx::impl::strategy::ic_detail {
namespace {
// The top-level `theme_schedule` block with rule mom-volman-v1 (fitter flag --mom-volman
// mom-volman-v1, P9 lane AL-COMB): exactly
// {"rule":"mom-volman-v1","theme":"price_momentum","lookback":126,"lag":2,
// "step":21,"sessions":[i64 session key, ...],"sleeve":[number, ...]}, the registered constants and
// the recorded sleeve (one finite value per decision, sessions strictly increasing, 128..65536 of
// each), beside a theme_standardise block with rerank true and without theme_residualise, whose
// weighted themes include price_momentum. Each block of mom_volman_blocks starts at the session of
// its decision and keeps the parent's W_theme (the sum of the theme's pinned weights in library
// order, as IcComposition::create sums them) except W_price_momentum x scale, by composition theme
// index, for score_role, which places each block at the first session >= its session. Fills the
// same pinned schedule as theme-tsmom-v1 (no theme is switched off: schedule_off 0).
co::Status composition_volman(const Json& j, const RuleInputs& in, PinnedWeights& pinned) {
  const auto& lib = in.lib;
  const auto& block = j.at("theme_schedule");
  const auto registered = [&block](const char* key, usize value) {
    return block.contains(key) && block.at(key).is_number_integer() &&
           block.at(key).get<i64>() == static_cast<i64>(value);
  };
  const auto series = [&block](const char* key) {
    return block.contains(key) && block.at(key).is_array() &&
           block.at(key).size() >= mom_volman_lookback + mom_volman_lag &&
           block.at(key).size() <= mom_volman_max_decisions;
  };
  if (!block.is_object() || block.size() != 7U || !block.contains("theme") ||
      !block.at("theme").is_string() ||
      block.at("theme").get_ref<const std::string&>() != mom_volman_theme ||
      !registered("lookback", mom_volman_lookback) || !registered("lag", mom_volman_lag) ||
      !registered("step", mom_volman_step) || !series("sessions") || !series("sleeve") ||
      block.at("sessions").size() != block.at("sleeve").size())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_schedule must be {rule: mom-volman-v1, theme: price_momentum, "
                   "lookback: 126, lag: 2, step: 21, sessions: [session, ...], sleeve: [number, "
                   "...]} (128..65536 decisions, one sleeve value per session)");
  if (pinned.std_themes.empty() || pinned.residualise)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_schedule needs a theme_standardise block with rerank true and "
                   "no theme_residualise (it reweights the standardised theme composites)");
  // The standardise block's theme names by composition index (theme_indices: first appearance in
  // library order) and the parent's W_theme, summed as IcComposition::create sums std_mass.
  const auto& listed = j.at("theme_standardise").at("themes");
  std::vector<std::string> names(pinned.std_theme_count);
  std::vector<f64> mass(pinned.std_theme_count, 0.0);
  for (usize k = 0; k < lib.candidates.size(); ++k)
    if (pinned.values[k] > 0) {
      names[pinned.std_themes[k]] = listed.at(lib.candidates[k].id).get<std::string>();
      mass[pinned.std_themes[k]] += pinned.values[k];
    }
  const auto pm = std::find(names.begin(), names.end(), std::string(mom_volman_theme));
  if (pm == names.end())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_schedule mom-volman-v1: price_momentum is not a weighted "
                   "theme of theme_standardise");
  const auto momentum = static_cast<usize>(pm - names.begin());
  const auto& sessions = block.at("sessions");
  const auto& values = block.at("sleeve");
  std::vector<f64> sleeve(values.size());
  for (usize d = 0; d < values.size(); ++d) {
    // Session d - 1 passed this check one step earlier.
    if (!sessions[d].is_number_integer() ||
        (d > 0 && !(sessions[d].get<i64>() > sessions[d - 1].get<i64>())))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "IC runner: theme_schedule mom-volman-v1 sessions must be integers that "
                     "increase strictly");
    sleeve[d] = values[d].is_number() ? values[d].get<f64>() : quiet_nan;
  }
  auto blocks = mom_volman_blocks(sleeve);
  if (!blocks)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_schedule: " + blocks.error().message());
  std::vector<i64> from;
  std::vector<std::vector<f64>> scheduled;
  from.reserve(blocks->size());
  scheduled.reserve(blocks->size());
  for (const auto& b : *blocks) {
    from.push_back(sessions[b.decision].get<i64>());
    auto masses = mass;
    masses[momentum] = mass[momentum] * b.scale;
    scheduled.push_back(std::move(masses));
  }
  pinned.schedule = std::string(mom_volman_rule);
  pinned.schedule_from = std::move(from);
  pinned.schedule_mass = std::move(scheduled);
  pinned.schedule_off = 0;
  return co::Ok();
}
} // namespace

// The theme_schedule rows' shared parse (theme-tsmom-v1 and mom-volman-v1, the rule table): a block
// whose rule is mom-volman-v1 is read above, any other (or none) by composition_schedule,
// unchanged.
co::Status composition_schedule_rules(const Json& j, const RuleInputs& in, PinnedWeights& pinned) {
  if (j.contains("theme_schedule")) {
    const auto& block = j.at("theme_schedule");
    if (block.is_object() && block.contains("rule") && block.at("rule").is_string() &&
        block.at("rule").get_ref<const std::string&>() == mom_volman_rule)
      return composition_volman(j, in, pinned);
  }
  return composition_schedule(j, in, pinned);
}
} // namespace atx::impl::strategy::ic_detail
