#include "strategy_ic_theme_hedge.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "atx/engine/combine/char_cov_hedge.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "strategy_ic_rules.hpp"
#include "strategy_price_exposures.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
namespace cb = atx::engine::combine;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
// The registered constants as the kernel takes them.
constexpr cb::CharCovHedgeConfig hedge_config{theme_hedge_lookback, theme_hedge_lag,
                                              theme_hedge_step, theme_hedge_min_pairs};
} // namespace

co::Result<u64> theme_hedge_working_bytes(usize dates, usize names) {
  if (dates == 0U || names == 0U || dates > std::numeric_limits<usize>::max() / names)
    return co::Err(co::ErrorCode::InvalidArgument, "theme-cov-hedge-v1: dimensions");
  ATX_TRY(const u64 scratch, cb::char_cov_hedge_scratch_bytes(dates, names, hedge_config));
  const u64 cells = static_cast<u64>(dates) * static_cast<u64>(names);
  constexpr u64 per_cell = 2U * sizeof(f64) + sizeof(u8); // returns, hedged plane, presence copy
  if (cells > (std::numeric_limits<u64>::max() - scratch) / per_cell)
    return co::Err(co::ErrorCode::OutOfRange, "theme-cov-hedge-v1: working bytes overflow");
  return co::Ok(cells * per_cell + scratch);
}

co::Status add_theme_hedged(std::span<const std::vector<f64>> planes, std::span<const f64> mass,
                            std::span<const IcThemeBlock> schedule,
                            std::span<const f64> session_returns, usize names,
                            std::span<f64> out) {
  const auto bad_plane = [&out](const std::vector<f64>& plane) {
    return plane.size() != out.size();
  };
  const auto bad_block = [&planes](const IcThemeBlock& block) {
    return block.mass.size() != planes.size();
  };
  if (names == 0U || planes.size() != mass.size() || out.size() % names != 0U ||
      session_returns.size() != out.size() ||
      std::any_of(planes.begin(), planes.end(), bad_plane) ||
      std::any_of(schedule.begin(), schedule.end(), bad_block) ||
      std::any_of(mass.begin(), mass.end(), [](f64 w) { return !std::isfinite(w); }))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "theme-cov-hedge-v1: plane, theme weight, schedule, return or blend shape");
  if (planes.empty()) return co::Ok();
  const usize dates = out.size() / names;
  std::vector<f64> hedged;
  cb::CharCovHedgeScratch scratch;
  try {
    hedged.resize(out.size());
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange,
                   "theme-cov-hedge-v1: hedged plane allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange,
                   "theme-cov-hedge-v1: hedged plane extent exceeded");
  }
  for (usize t = 0; t < planes.size(); ++t) {
    ATX_TRY_VOID(
        cb::char_cov_hedge(planes[t], session_returns, names, hedge_config, hedged, scratch));
    // The blocks in force, as IcComposition::finish walks them: W_theme before the first block.
    for (usize b = 0; b <= schedule.size(); ++b) {
      const usize begin = b == 0U ? 0U : schedule[b - 1U].begin;
      const usize end = b == schedule.size() ? dates : schedule[b].begin;
      const f64 weight = b == 0U ? mass[t] : schedule[b - 1U].mass[t];
      if (begin >= end || end > dates || !(weight > 0)) continue;
      for (usize k = begin * names; k < end * names; ++k)
        if (!std::isnan(hedged[k])) out[k] += weight * hedged[k];
    }
  }
  return co::Ok();
}

co::Result<std::vector<f64>> theme_hedge_session_returns(
    const engine::data::StrategyRoleData& role) {
  const auto& panel = role.panel;
  const usize dates = panel.dates(), names = panel.instruments();
  if (dates == 0U || names == 0U || dates > std::numeric_limits<usize>::max() / names)
    return co::Err(co::ErrorCode::InvalidArgument, "theme-cov-hedge-v1: role dimensions");
  ATX_TRY(const auto close_id, panel.field_id("close"));
  ATX_TRY(const auto raw_id, panel.field_id("raw_close"));
  std::vector<u8> present;
  std::vector<f64> out;
  try {
    present.resize(dates * names);
    out.assign(dates * names, nan);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange,
                   "theme-cov-hedge-v1: session return allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange,
                   "theme-cov-hedge-v1: session return extent exceeded");
  }
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i)
      present[d * names + i] = static_cast<u8>(panel.in_universe(d, i));
  // Volume is not read by the interval kernel.
  const PriceExposureInput input{dates, names, panel.field_all(close_id),
                                 panel.field_all(raw_id), {}, present};
  PriceExposureScratch scratch;
  for (usize s = 1; s < dates; ++s)
    ATX_TRY_VOID(session_interval_returns(input, s, scratch,
                                          std::span<f64>(out).subspan(s * names, names)));
  return co::Ok(std::move(out));
}
} // namespace atx::impl::strategy

namespace atx::impl::strategy::ic_detail {
co::Status attach_theme_hedge(const PinnedWeights& pinned,
                              const engine::data::StrategyRoleData& role,
                              IcComposition& composition) {
  const auto rules = running_rules(pinned);
  const bool hedge = std::any_of(rules.begin(), rules.end(), [](const CompositionRule* rule) {
    return rule->stage == IcStageKind::hedge;
  });
  if (!hedge) return co::Ok();
  ATX_TRY(auto returns, theme_hedge_session_returns(role));
  return composition.set_theme_hedge(std::move(returns));
}

// Optional top-level `theme_hedge` (fitter flag --theme-hedge theme-cov-hedge-v1, P9 lane AL-COMB;
// the fitter writes only the spec): exactly
// {"rule":"theme-cov-hedge-v1","lookback":252,"lag":2,"step":21, "min_pairs":126}, the registered
// constants, beside a theme_standardise block with rerank true and without theme_residualise and
// theme_sleeves (finish hedges the standardised re-ranks; a residualised or split composite is
// another object). Nothing is recorded to verify: the hedge is estimated at apply time
// (attach_theme_hedge, add_theme_hedged). The rule table records the row (pinned.rules); absent,
// nothing changes. Runs last, after composition_sleeves, before any role payload.
co::Status composition_hedge(const Json& j, const RuleInputs& /*in*/, PinnedWeights& pinned) {
  if (!j.contains("theme_hedge")) return co::Ok();
  const auto& block = j.at("theme_hedge");
  const auto registered = [&block](const char* key, usize value) {
    return block.contains(key) && block.at(key).is_number_integer() &&
           block.at(key).get<i64>() == static_cast<i64>(value);
  };
  if (!block.is_object() || block.size() != 5U || !block.contains("rule") ||
      !block.at("rule").is_string() ||
      block.at("rule").get_ref<const std::string&>() != theme_hedge_rule ||
      !registered("lookback", theme_hedge_lookback) || !registered("lag", theme_hedge_lag) ||
      !registered("step", theme_hedge_step) || !registered("min_pairs", theme_hedge_min_pairs))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_hedge must be {rule: theme-cov-hedge-v1, lookback: 252, "
                   "lag: 2, step: 21, min_pairs: 126}");
  if (pinned.std_themes.empty() || pinned.residualise || !pinned.sleeves.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_hedge needs a theme_standardise block with rerank true and "
                   "no theme_residualise or theme_sleeves (it hedges the standardised theme "
                   "composites)");
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
