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

co::Result<u64> theme_hedge_working_bytes(usize dates, usize names, usize themes) {
  if (dates == 0U || names == 0U || dates > std::numeric_limits<usize>::max() / names)
    return co::Err(co::ErrorCode::InvalidArgument, "theme-cov-hedge-v1: dimensions");
  ATX_TRY(const u64 scratch, cb::char_cov_hedge_scratch_bytes(dates, names, hedge_config));
  const u64 cells = static_cast<u64>(dates) * static_cast<u64>(names);
  constexpr u64 per_cell = 2U * sizeof(f64) + sizeof(u8); // returns, hedged plane, presence copy
  // The printed hedge ratios: the ratio in force per date, then per block its start and one ratio
  // per theme (add_theme_hedged's trace).
  namespace hd = cb::hedge_detail;
  const u64 blocks = theme_hedge_block_count(dates);
  u64 total = scratch;
  const bool fits = hd::add_u64(total, cells, per_cell) &&
                    hd::add_u64(total, dates, sizeof(f64)) &&
                    hd::add_u64(total, blocks, sizeof(usize)) &&
                    themes <= std::numeric_limits<u64>::max() / sizeof(f64) &&
                    hd::add_u64(total, blocks, static_cast<u64>(themes) * sizeof(f64));
  if (!fits)
    return co::Err(co::ErrorCode::OutOfRange, "theme-cov-hedge-v1: working bytes overflow");
  return co::Ok(total);
}

usize theme_hedge_block_count(usize dates) noexcept {
  const usize first = theme_hedge_lookback + theme_hedge_lag - 1U;
  return dates > first ? (dates - 1U - first) / theme_hedge_step + 1U : 0U;
}

co::Status add_theme_hedged(std::span<const std::vector<f64>> planes, std::span<const f64> mass,
                            std::span<const IcThemeBlock> schedule,
                            std::span<const f64> session_returns, usize names,
                            std::span<f64> out, std::vector<usize>* block,
                            std::vector<std::vector<f64>>* ratio) {
  const auto bad_plane = [&out](const std::vector<f64>& plane) {
    return plane.size() != out.size();
  };
  const auto bad_block = [&planes](const IcThemeBlock& block_mass) {
    return block_mass.mass.size() != planes.size();
  };
  if (names == 0U || planes.size() != mass.size() || out.size() % names != 0U ||
      session_returns.size() != out.size() ||
      std::any_of(planes.begin(), planes.end(), bad_plane) ||
      std::any_of(schedule.begin(), schedule.end(), bad_block) ||
      std::any_of(mass.begin(), mass.end(), [](f64 w) { return !std::isfinite(w); }) ||
      (block == nullptr) != (ratio == nullptr))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "theme-cov-hedge-v1: plane, theme weight, schedule, return, blend or trace "
                   "shape");
  const bool traced = block != nullptr;
  const usize dates = out.size() / names;
  const usize blocks = theme_hedge_block_count(dates);
  const usize first = theme_hedge_lookback + theme_hedge_lag - 1U;
  std::vector<f64> hedged, in_force;
  std::vector<usize> starts;
  std::vector<std::vector<f64>> table;
  cb::CharCovHedgeScratch scratch;
  try {
    if (!planes.empty()) hedged.resize(out.size());
    if (traced) {
      if (!planes.empty()) in_force.resize(dates);
      starts.resize(blocks);
      table.assign(planes.size(), std::vector<f64>(blocks, 0.0));
    }
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange,
                   "theme-cov-hedge-v1: hedged plane allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange,
                   "theme-cov-hedge-v1: hedged plane extent exceeded");
  }
  for (usize b = 0; b < starts.size(); ++b) starts[b] = first + b * theme_hedge_step;
  for (usize t = 0; t < planes.size(); ++t) {
    // The kernel's ratio in force per date (step 4), read at each block start for the trace.
    const cb::CharCovHedgeTrace trace{{}, {}, std::span<f64>(in_force), {}};
    ATX_TRY_VOID(cb::char_cov_hedge(planes[t], session_returns, names, hedge_config, hedged,
                                    scratch, trace));
    for (usize b = 0; b < starts.size(); ++b) table[t][b] = in_force[starts[b]];
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
  if (traced) {
    *block = std::move(starts);
    *ratio = std::move(table);
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
// (attach_theme_hedge, add_theme_hedged). The rule table records the row (pinned.rules); accepted,
// pinned.hedge_themes names the standardised themes by composition index (the summary's trace);
// absent, nothing changes. Runs last, after composition_sleeves, before any role payload.
co::Status composition_hedge(const Json& j, const RuleInputs& in, PinnedWeights& pinned) {
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
  // The standardise block's theme names by composition index (theme_indices: first appearance of
  // a weighted candidate's theme in library order; that parse checked every name).
  const auto& listed = j.at("theme_standardise").at("themes");
  std::vector<std::string> names(pinned.std_theme_count);
  for (usize k = 0; k < in.lib.candidates.size() && k < pinned.values.size(); ++k)
    if (pinned.values[k] > 0)
      names.at(pinned.std_themes.at(k)) = listed.at(in.lib.candidates[k].id).get<std::string>();
  pinned.hedge_themes = std::move(names);
  return co::Ok();
}

co::Status add_theme_hedge_trace(const PinnedWeights& pinned, std::span<const i64> sessions,
                                 const IcCompositionResult& combined, Json& result) {
  if (pinned.hedge_themes.empty()) return co::Ok();
  const usize themes = pinned.hedge_themes.size(), blocks = combined.hedge_block.size();
  const auto bad_row = [blocks](const std::vector<f64>& row) { return row.size() != blocks; };
  if (combined.hedge_ratio.size() != themes ||
      std::any_of(combined.hedge_ratio.begin(), combined.hedge_ratio.end(), bad_row) ||
      std::any_of(combined.hedge_block.begin(), combined.hedge_block.end(),
                  [&sessions](usize d) { return d >= sessions.size(); }))
    return co::Err(co::ErrorCode::Internal, "IC runner: theme-cov-hedge-v1 trace shape");
  Json rows = Json::array();
  for (usize b = 0; b < blocks; ++b) {
    Json h = Json::array();
    for (usize t = 0; t < themes; ++t) h.push_back(combined.hedge_ratio[t][b]);
    const usize d = combined.hedge_block[b];
    rows.push_back(Json{{"decision_index", d}, {"session_ns", sessions[d]}, {"h", std::move(h)}});
  }
  result["composition_hedge_trace"] = Json{
      {"rule", std::string(theme_hedge_rule)},
      {"themes", pinned.hedge_themes},
      {"semantics",
       "h[t]: theme t's hedge ratio in force from the block's decision_index (session session_ns) "
       "to the next block's: the OLS slope of the theme's sleeve on its hedge leg's return over "
       "the 252 decisions ending two before, 0 below 126 pairs or without hedge-leg variance; "
       "the theme adds W_t (R_t - h H_t); before the first block h is 0 (the parent's re-rank)"},
      {"blocks", std::move(rows)}};
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
