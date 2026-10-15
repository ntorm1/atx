#include "strategy_ic_mom_volman.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <locale>
#include <span>
#include <sstream>
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
    // No cap (registered, ALCOMB-4): a tiny but positive sigma gives a large finite scale; one so
    // small that target / sigma overflows (or a running sum that does) is refused here, naming the
    // rule, not later by the composition's mass check (review S-1).
    const f64 scale = target / sigma;
    if (!std::isfinite(target) || !std::isfinite(scale))
      return co::Err(refused("the scale target / sigma before decision " + std::to_string(j) +
                             " must be finite (no cap is registered)"));
    out.push_back({j, sigma, target, scale});
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
  std::vector<f64> sigma, target, scale;
  from.reserve(blocks->size());
  scheduled.reserve(blocks->size());
  sigma.reserve(blocks->size());
  target.reserve(blocks->size());
  scale.reserve(blocks->size());
  for (const auto& b : *blocks) {
    from.push_back(sessions[b.decision].get<i64>());
    auto masses = mass;
    masses[momentum] = mass[momentum] * b.scale;
    scheduled.push_back(std::move(masses));
    sigma.push_back(b.sigma);
    target.push_back(b.target);
    scale.push_back(b.scale);
  }
  pinned.schedule = std::string(mom_volman_rule);
  pinned.schedule_from = std::move(from);
  pinned.schedule_mass = std::move(scheduled);
  pinned.schedule_off = 0;
  // What the role summary prints (add_mom_volman_trace, ruling ALCOMB-5 S-3).
  pinned.volman_sigma = std::move(sigma);
  pinned.volman_target = std::move(target);
  pinned.volman_scale = std::move(scale);
  pinned.volman_theme = momentum; // P9 lane D3W: walk_forward rows scale this theme's row mass
  // P9 lane D3W (ruling ALCOMB-6): the recorded sleeve, for each scored role's sleeve files.
  for (const auto& s : sessions) pinned.volman_sessions.push_back(s.get<i64>());
  pinned.volman_sleeve = std::move(sleeve);
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

co::Status add_mom_volman_trace(const PinnedWeights& pinned, Json& result) {
  if (pinned.schedule != mom_volman_rule) return co::Ok();
  const usize blocks = pinned.schedule_from.size();
  if (pinned.volman_sigma.size() != blocks || pinned.volman_target.size() != blocks ||
      pinned.volman_scale.size() != blocks)
    return co::Err(co::ErrorCode::Internal, "IC runner: mom-volman-v1 trace shape");
  Json rows = Json::array();
  for (usize b = 0; b < blocks; ++b)
    rows.push_back(Json{{"from_session", pinned.schedule_from[b]},
                        {"sigma", pinned.volman_sigma[b]},
                        {"target", pinned.volman_target[b]},
                        {"scale", pinned.volman_scale[b]}});
  result["composition_schedule_trace"] = Json{
      {"rule", std::string(mom_volman_rule)},
      {"theme", std::string(mom_volman_theme)},
      {"semantics",
       "per block of the recorded sleeve, from its first session from_session to the next "
       "block's: sigma = the sleeve's realised volatility over the 126 decisions ending two "
       "before the block (the root of the mean square), target = the mean of sigma over this and "
       "every earlier block, scale = target / sigma, the factor on price_momentum's W_t (each "
       "role applies a block from its first session at or after from_session)"},
      {"blocks", std::move(rows)}};
  return co::Ok();
}

// P9 lane D3W (ruling ALCOMB-6): per scored role, the recorded sleeve before and after each block's
// scale, from the first block's session within the role (here beside composition_volman: review
// D3W S-5).
co::Status write_mom_volman_sleeves(const PinnedWeights& pinned, std::span<const i64> role_sessions,
                                    const std::filesystem::path& dir, const std::string& role) {
  if (pinned.schedule != mom_volman_rule || role_sessions.empty() || pinned.schedule_from.empty())
    return co::Ok();
  if (pinned.volman_sessions.size() != pinned.volman_sleeve.size() ||
      pinned.volman_scale.size() != pinned.schedule_from.size())
    return co::Err(co::ErrorCode::Internal, "IC runner: mom-volman-v1 sleeve shape");
  std::ostringstream raw, scaled;
  for (auto* text : {&raw, &scaled}) {
    text->imbue(std::locale::classic());
    *text << std::setprecision(17) << "session_ns,net_return\n";
  }
  usize rows = 0, block = 0;
  for (usize d = 0; d < pinned.volman_sleeve.size(); ++d) {
    const i64 session = pinned.volman_sessions[d];
    if (session < pinned.schedule_from.front() || session < role_sessions.front() ||
        session > role_sessions.back())
      continue;
    while (block + 1U < pinned.schedule_from.size() && pinned.schedule_from[block + 1U] <= session)
      ++block;
    raw << session << ',' << pinned.volman_sleeve[d] << '\n';
    scaled << session << ',' << pinned.volman_sleeve[d] * pinned.volman_scale[block] << '\n';
    ++rows;
  }
  if (rows == 0U) return co::Ok();
  for (const auto& [name, text] :
       {std::pair{std::string("_mom_volman_sleeve_raw.csv"), &raw},
        std::pair{std::string("_mom_volman_sleeve_scaled.csv"), &scaled}}) {
    const std::string bytes = text->str();
    std::ofstream file(dir / (role + name), std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file)
      return co::Err(co::ErrorCode::IoError,
                     "IC runner: cannot write " + (dir / (role + name)).string());
  }
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
