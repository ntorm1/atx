#include "strategy_ic_composition.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include "atx/engine/combine/group_rerank.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "strategy_ic_theme_resid.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace cb = atx::engine::combine;
constexpr usize max_candidates = 256;
constexpr usize max_themes = 32; // pinned within-theme redistribution (ew-theme-v6)
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
using Ranked = std::pair<f64, usize>;
bool safe_name(const std::string& s) {
  return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}
// Sort is bounded in-place; explicit index tie break makes accumulation stable.
void sort_ranks(std::vector<Ranked>& values) { std::sort(values.begin(), values.end()); }
template<class F> void each_centered_rank(const std::vector<Ranked>& v, F&& apply) {
  if (v.size() < 2) return;
  for (usize b = 0; b < v.size();) {
    usize e = b + 1;
    while (e < v.size() && v[e].first == v[b].first) ++e;
    const auto r = (static_cast<f64>(b) + static_cast<f64>(e - 1)) /
                   (2.0 * static_cast<f64>(v.size() - 1)) - 0.5;
    for (usize k = b; k < e; ++k) apply(v[k].second, r);
    b = e;
  }
}
// f64 planes per theme: redistribute keeps a blend and a present-weight plane,
// standardise (and residualise) one summed-rank plane (NaN = no member present).
constexpr u64 theme_planes(IcThemeRule rule) noexcept {
  return rule == IcThemeRule::redistribute ? 2U : 1U;
}
// ew-theme-std-v1 pooled add: dates split into contiguous bands as the pinned path splits
// them (quotient/remainder, no count*band), one ranked row per worker, allocated once.
// rank_dates(begin, end, row) must touch only rows [begin, end) of what it writes.
template <class RankDates>
co::Status pooled_date_bands(engine::parallel::DetPool& pool, usize dates, usize names,
                             std::vector<std::vector<Ranked>>& worker_rows, RankDates&& rank_dates) {
  const usize workers = pool.n_workers();
  if (worker_rows.size() != workers) {
    try {
      worker_rows.assign(workers, {});
      for (auto& row : worker_rows) row.reserve(names);
    } catch (const std::bad_alloc&) {
      worker_rows.clear();
      return co::Err(co::ErrorCode::OutOfRange, "IC composition: worker row allocation failed");
    } catch (const std::length_error&) {
      worker_rows.clear();
      return co::Err(co::ErrorCode::OutOfRange, "IC composition: worker row extent exceeded");
    }
  }
  const usize bands = std::min(dates, workers * 4U);
  pool.parallel_for(bands, [&](usize band, usize worker) {
    const usize begin = dates / bands * band + dates % bands * band / bands;
    const usize end = dates / bands * (band + 1U) + dates % bands * (band + 1U) / bands;
    rank_dates(begin, end, worker_rows[worker]);
  });
  return co::Ok();
}
} // namespace

co::Result<u64> ic_composition_working_bytes(usize dates, usize names, usize count, usize themes,
                                             IcThemeRule rule, bool sleeves) {
  if (!dates || !names || !count || count > max_candidates || themes > max_themes)
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: invalid dimensions/count");
  u64 total = 4096;
  const auto add = [&](u64 n, u64 width) {
    if (width && n > (std::numeric_limits<u64>::max() - total) / width) return false;
    total += n * width; return true;
  };
  if (dates > std::numeric_limits<usize>::max() / names)
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: cell count overflow");
  if (!add(dates * names, sizeof(f64) + sizeof(u8)) ||
      !add(dates, 4 * sizeof(f64) + sizeof(usize)) ||
      !add(names, sizeof(Ranked) + 2 * sizeof(f64)) || !add(count, 512))
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working bytes overflow");
  // Pinned themes: per theme a blend and a present-weight f64 plane (redistribute) or
  // one summed-rank f64 plane (standardise); none: +0.
  if (themes && !add(dates * names, theme_planes(rule) * sizeof(f64) * static_cast<u64>(themes)))
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working bytes overflow");
  // theme-resid-v1: per name the earlier themes' columns and the dependent (8 B per theme) and the
  // support index (add_theme_residualised's scratch); none otherwise.
  if (themes && rule == IcThemeRule::residualise &&
      !add(names, sizeof(f64) * static_cast<u64>(themes) + sizeof(usize)))
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working bytes overflow");
  // two-speed-v1 (v8 Y-5): the fast and slow sleeve planes and the fast share per date; none otherwise.
  if (sleeves && (!add(dates * names, 2 * sizeof(f64)) || !add(dates, sizeof(f64))))
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working bytes overflow");
  return co::Ok(total);
}

struct IcComposition::Impl {
  IcCompositionConfig cfg;
  std::vector<IcCompositionCandidate> candidates;
  std::vector<f64> weights;
  std::vector<u8> member;
  std::vector<Ranked> row;
  std::vector<std::vector<Ranked>> worker_rows; // pooled add only; one per pool worker
  std::vector<f64> current, target;
  // Pinned within-theme redistribution (empty: none): theme index per candidate,
  // W_theme, and per theme the date-major blend and present-weight planes.
  std::vector<usize> theme;
  std::vector<f64> theme_mass;
  std::vector<std::vector<f64>> theme_blend, theme_present;
  // ew-theme-std-v1 (empty: off): theme index per candidate, W_theme, and per theme the
  // date-major plane of summed weighted signed member ranks, NaN where no member is present.
  std::vector<usize> std_theme;
  std::vector<f64> std_mass;
  std::vector<std::vector<f64>> std_plane;
  bool residualise{}; // theme-resid-v1 on the std planes (theme index = registered-order position)
  std::vector<IcThemeBlock> schedule; // theme-tsmom-v1 mass schedule on the std planes (empty: W_theme throughout)
  std::vector<u8> sleeve_of;          // two-speed-v1: per theme index 1 fast, 0 slow (empty: no sleeves)
  IcCompositionResult result;
  usize next{};
  bool finished{};
};
IcComposition::IcComposition(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
IcComposition::~IcComposition() = default;
IcComposition::IcComposition(IcComposition&&) noexcept = default;
IcComposition& IcComposition::operator=(IcComposition&&) noexcept = default;

co::Result<IcComposition> IcComposition::create(const IcCompositionConfig& cfg,
    std::span<const IcCompositionCandidate> candidates, std::span<const u8> member,
    std::span<const f64> pinned, std::span<const usize> pinned_themes, IcThemeRule rule) {
  // Pinned themes need pinned weights; only positive-weight candidates name a theme.
  usize themes = 0;
  if (!pinned_themes.empty()) {
    if (pinned.size() != candidates.size() || pinned_themes.size() != candidates.size())
      return co::Err(co::ErrorCode::InvalidArgument, "IC composition: themes need pinned weights, one per candidate");
    for (usize i = 0; i < candidates.size(); ++i) {
      if (!(pinned[i] > 0)) continue;
      if (pinned_themes[i] >= max_themes)
        return co::Err(co::ErrorCode::InvalidArgument, "IC composition: theme index bound exceeded");
      themes = std::max(themes, pinned_themes[i] + 1);
    }
    if (!themes)
      return co::Err(co::ErrorCode::InvalidArgument, "IC composition: themes without a weighted candidate");
  }
  ATX_TRY(auto bytes, ic_composition_working_bytes(cfg.dates, cfg.instruments, candidates.size(), themes, rule));
  const bool standardise = themes && rule != IcThemeRule::redistribute; // standardise or residualise
  if (bytes > cfg.max_working_bytes)
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working budget exceeded");
  const usize cells = cfg.dates * cfg.instruments;
  if (member.size() != cells || cfg.decision_begin >= cfg.decision_end ||
      cfg.decision_end > cfg.dates || !cfg.cadence ||
      !std::isfinite(cfg.trade_fraction) || cfg.trade_fraction <= 0 || cfg.trade_fraction > 1 ||
      std::any_of(member.begin(), member.end(), [](u8 v) { return v > 1; }))
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: config/membership mismatch");
  if (!pinned.empty() && (pinned.size() != candidates.size() ||
      std::any_of(pinned.begin(), pinned.end(), [](f64 w) { return !std::isfinite(w) || w < 0; })))
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: pinned weights");
  usize families = 0;
  for (usize i = 0; i < candidates.size(); ++i) {
    const auto& c = candidates[i];
    if (!safe_name(c.id) || !safe_name(c.family))
      return co::Err(co::ErrorCode::InvalidArgument, "IC composition: invalid candidate metadata");
    bool first = true;
    for (usize j = 0; j < i; ++j) {
      if (candidates[j].id == c.id)
        return co::Err(co::ErrorCode::InvalidArgument, "IC composition: duplicate candidate");
      if (candidates[j].family == c.family) first = false;
    }
    families += first ? 1U : 0U;
  }
  if (families > 32)
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: family bound exceeded");
  try {
    auto p = std::make_unique<Impl>(); p->cfg = cfg;
    p->candidates.assign(candidates.begin(), candidates.end());
    p->member.assign(member.begin(), member.end()); p->weights.resize(candidates.size());
    for (usize i = 0; i < candidates.size(); ++i) {
      // Pinned values are stored verbatim and read by the same add() expression.
      if (!pinned.empty()) { p->weights[i] = pinned[i]; continue; }
      const auto n = std::count_if(candidates.begin(), candidates.end(), [&](const auto& c) {
        return c.family == candidates[i].family;
      });
      p->weights[i] = 1.0 / (static_cast<f64>(families) * static_cast<f64>(n));
    }
    p->row.reserve(cfg.instruments); p->current.resize(cfg.instruments); p->target.resize(cfg.instruments);
    if (standardise) {
      p->std_theme.assign(pinned_themes.begin(), pinned_themes.end());
      p->std_mass.assign(themes, 0.0);
      for (usize i = 0; i < candidates.size(); ++i)
        if (pinned[i] > 0) p->std_mass[pinned_themes[i]] += pinned[i];
      p->std_plane.resize(themes);
      for (auto& plane : p->std_plane) plane.assign(cells, nan);
      p->residualise = rule == IcThemeRule::residualise;
    } else if (themes) {
      p->theme.assign(pinned_themes.begin(), pinned_themes.end());
      p->theme_mass.assign(themes, 0.0);
      for (usize i = 0; i < candidates.size(); ++i)
        if (pinned[i] > 0) p->theme_mass[pinned_themes[i]] += pinned[i];
      p->theme_blend.resize(themes); p->theme_present.resize(themes);
      for (usize t = 0; t < themes; ++t) { p->theme_blend[t].assign(cells, 0.0); p->theme_present[t].assign(cells, 0.0); }
    }
    auto& out = p->result; out.signal.resize(cells);
    out.planned_turnover.assign(cfg.dates, nan); out.contribution_fraction.resize(cfg.dates);
    out.planned_gross.assign(cfg.dates, nan); out.planned_net.assign(cfg.dates, nan);
    out.eligible_names.resize(cfg.dates); out.deployment_date = cfg.dates;
    for (usize d = 0; d < cfg.dates; ++d) for (usize i = 0; i < cfg.instruments; ++i) {
      const auto at = d * cfg.instruments + i;
      if (member[at]) ++out.eligible_names[d]; else out.signal[at] = nan;
    }
    return co::Ok(IcComposition(std::move(p)));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: allocation extent exceeded");
  }
}

co::Status IcComposition::add(usize index, std::span<const f64> signal, int sign,
                              engine::parallel::DetPool* pool) {
  if (!impl_ || impl_->finished || index != impl_->next || index >= impl_->candidates.size() ||
      sign < -1 || sign > 1)
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: candidate order/finished state");
  auto& p = *impl_;
  if (signal.size() != p.result.signal.size() && !(sign == 0 && signal.empty()))
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: signal shape mismatch");
  // Default weights are strictly positive, so the weight test alters only pinned
  // zeros: skipping adds of +/-0 leaves every accumulator bit unchanged.
  if (sign == 0 || p.weights[index] == 0) { ++p.next; return co::Ok(); }
  if (!p.std_theme.empty()) return add_standardised(index, signal, sign, pool);
  const f64 weight = p.weights[index];
  // Pinned themes: the candidate accumulates into its theme's blend plane and adds
  // its weight to the theme's present-weight plane (folded by finish); otherwise
  // straight into the blend, the unchanged expression on the same cells.
  const bool themed = !p.theme.empty();
  f64* const blend = themed ? p.theme_blend[p.theme[index]].data() : p.result.signal.data();
  f64* const present = themed ? p.theme_present[p.theme[index]].data() : nullptr;
  // Dates [begin, end) touch only their own blend rows and coverage slots; `row`
  // has capacity for every name, so ranking never allocates.
  const auto rank_dates = [&](usize begin, usize end, std::vector<Ranked>& row) {
    for (usize d = begin; d < end; ++d) {
      row.clear(); const auto offset = d * p.cfg.instruments;
      for (usize i = 0; i < p.cfg.instruments; ++i)
        if (p.member[offset + i] && std::isfinite(signal[offset + i]))
          row.emplace_back(signal[offset + i], i);
      if (row.size() < 2) continue;
      sort_ranks(row);
      each_centered_rank(row, [&](usize i, f64 r) {
        blend[offset + i] += static_cast<f64>(sign) * weight * r;
        if (present != nullptr) present[offset + i] += weight;
      });
      p.result.contribution_fraction[d] += weight * static_cast<f64>(row.size());
    }
  };
  if (pool == nullptr || pool->n_workers() < 2 || p.cfg.dates < 2) {
    rank_dates(0, p.cfg.dates, p.row);
    ++p.next; return co::Ok();
  }
  const usize workers = pool->n_workers();
  if (p.worker_rows.size() != workers) {
    try {
      p.worker_rows.assign(workers, {});
      for (auto& row : p.worker_rows) row.reserve(p.cfg.instruments);
    } catch (const std::bad_alloc&) {
      p.worker_rows.clear();
      return co::Err(co::ErrorCode::OutOfRange, "IC composition: worker row allocation failed");
    } catch (const std::length_error&) {
      p.worker_rows.clear();
      return co::Err(co::ErrorCode::OutOfRange, "IC composition: worker row extent exceeded");
    }
  }
  // Same band split as the research IC rows: quotient/remainder, no count*band.
  const usize count = p.cfg.dates, bands = std::min(count, workers * 4U);
  // SAFETY (data races): bands partition [0, dates), so each blend (or theme
  // plane) cell and each contribution_fraction[d] has exactly one writer; worker w alone uses
  // worker_rows[w]; weights, membership and the borrowed signal are read-only.
  // parallel_for's barrier orders every write before this call returns.
  pool->parallel_for(bands, [&](usize band, usize worker) {
    const usize begin = count / bands * band + count % bands * band / bands;
    const usize end = count / bands * (band + 1U) + count % bands * (band + 1U) / bands;
    rank_dates(begin, end, p.worker_rows[worker]);
  });
  ++p.next; return co::Ok();
}

// ew-theme-std-v1: the member's centred tied rank over member names with a finite signal
// (the pinned path's rank and sign * weight * r expression) accumulates into its theme's
// plane instead of the blend; coverage is counted as the pinned path counts it. add()
// has validated the call and skipped neutral and zero-weight candidates.
co::Status IcComposition::add_standardised(usize index, std::span<const f64> signal, int sign,
                                           engine::parallel::DetPool* pool) {
  auto& p = *impl_;
  const f64 weight = p.weights[index];
  f64* const plane = p.std_plane[p.std_theme[index]].data();
  const auto rank_dates = [&](usize begin, usize end, std::vector<Ranked>& row) {
    for (usize d = begin; d < end; ++d) {
      row.clear(); const auto offset = d * p.cfg.instruments;
      for (usize i = 0; i < p.cfg.instruments; ++i)
        if (p.member[offset + i] && std::isfinite(signal[offset + i]))
          row.emplace_back(signal[offset + i], i);
      if (row.size() < 2) continue;
      cb::for_each_centered_rank(row, [&](usize i, f64 r) {
        cb::accumulate_group_cell(plane[offset + i], static_cast<f64>(sign) * weight * r);
      });
      p.result.contribution_fraction[d] += weight * static_cast<f64>(row.size());
    }
  };
  if (pool == nullptr || pool->n_workers() < 2 || p.cfg.dates < 2) {
    rank_dates(0, p.cfg.dates, p.row);
  } else {
    // SAFETY (data races): bands partition [0, dates), so each plane cell and each
    // contribution_fraction[d] has exactly one writer; worker w alone uses worker_rows[w];
    // weights, membership and the borrowed signal are read-only; parallel_for's barrier
    // orders every write before it returns.
    ATX_TRY_VOID(pooled_date_bands(*pool, p.cfg.dates, p.cfg.instruments, p.worker_rows, rank_dates));
  }
  ++p.next; return co::Ok();
}

co::Status IcComposition::schedule_theme_masses(std::span<const IcThemeBlock> blocks) {
  if (!impl_ || impl_->finished)
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: theme schedule after finish");
  auto& p = *impl_;
  if (p.std_mass.empty() || p.residualise)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC composition: a theme schedule (theme-tsmom-v1) needs the standardise rule");
  usize last = 0;
  for (const auto& block : blocks) {
    if (block.begin < last || block.begin > p.cfg.dates || block.mass.size() != p.std_mass.size() ||
        std::any_of(block.mass.begin(), block.mass.end(), [](f64 m) { return !std::isfinite(m) || m < 0; }))
      return co::Err(co::ErrorCode::InvalidArgument, "IC composition: theme schedule blocks need a non-decreasing "
                     "begin <= dates and one finite mass >= 0 per theme");
    last = block.begin;
  }
  try {
    p.schedule.assign(blocks.begin(), blocks.end());
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: theme schedule allocation");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: theme schedule allocation");
  }
  return co::Ok();
}

co::Status IcComposition::set_theme_sleeves(std::span<const u8> fast) {
  if (!impl_ || impl_->finished)
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: theme sleeves after finish");
  auto& p = *impl_;
  if (p.std_mass.empty() || p.residualise)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC composition: theme sleeves (two-speed-v1) need the standardise rule");
  const auto fast_count = std::count(fast.begin(), fast.end(), u8{1});
  if (fast.size() != p.std_mass.size() || std::any_of(fast.begin(), fast.end(), [](u8 v) { return v > 1; }) ||
      fast_count == 0 || static_cast<usize>(fast_count) == fast.size())
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: theme sleeves need one flag (1 fast, 0 slow) "
                   "per theme and at least one theme of each");
  ATX_TRY(const auto bytes, ic_composition_working_bytes(p.cfg.dates, p.cfg.instruments, p.candidates.size(),
                                                         p.std_mass.size(), IcThemeRule::standardise, true));
  if (bytes > p.cfg.max_working_bytes)
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working budget exceeded (theme sleeves)");
  auto& out = p.result;
  try {
    // Under the standardise rule add() never writes `signal`: it is still members 0, nonmembers NaN.
    out.sleeve_fast = out.signal; out.sleeve_slow = out.signal;
    out.sleeve_fast_share.assign(p.cfg.dates, 0.0);
    p.sleeve_of.assign(fast.begin(), fast.end());
  } catch (const std::bad_alloc&) {
    std::vector<f64>().swap(out.sleeve_fast); std::vector<f64>().swap(out.sleeve_slow);
    std::vector<f64>().swap(out.sleeve_fast_share); std::vector<u8>().swap(p.sleeve_of);
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: theme sleeves allocation");
  } catch (const std::length_error&) {
    std::vector<f64>().swap(out.sleeve_fast); std::vector<f64>().swap(out.sleeve_slow);
    std::vector<f64>().swap(out.sleeve_fast_share); std::vector<u8>().swap(p.sleeve_of);
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: theme sleeves allocation");
  }
  return co::Ok();
}

co::Result<IcCompositionResult> IcComposition::finish() {
  if (!impl_ || impl_->finished || impl_->next != impl_->candidates.size())
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: incomplete/finished input");
  auto& p = *impl_; auto& out = p.result;
  // Pinned themes (else no-op): a cell with a present member adds W_theme times the
  // present-weighted mean of the theme's signed ranks; themes fold in index order and
  // their planes are released before the target pass.
  for (usize t = 0; t < p.theme_mass.size(); ++t) {
    const auto& blend = p.theme_blend[t]; const auto& present = p.theme_present[t];
    for (usize k = 0; k < blend.size(); ++k)
      if (present[k] > 0) out.signal[k] += p.theme_mass[t] * (blend[k] / present[k]);
  }
  std::vector<std::vector<f64>>().swap(p.theme_blend); std::vector<std::vector<f64>>().swap(p.theme_present);
  // ew-theme-std-v1 (else no-op): each theme's plane re-ranked per date over the names
  // with a present member, W_theme * rank added in theme index order; planes released
  // before the target pass. Nonmember cells are never present, so they stay NaN.
  // theme-resid-v1 instead residualises the re-ranked planes in index order per date.
  if (p.residualise) {
    ATX_TRY_VOID(add_theme_residualised(p.std_plane, p.std_mass, p.cfg.instruments, out.signal, p.row));
  } else if (p.schedule.empty()) {
    for (usize t = 0; t < p.std_plane.size(); ++t)
      ATX_TRY_VOID(cb::add_group_rerank(p.std_plane[t], p.cfg.instruments, 0, p.cfg.dates, p.std_mass[t],
                                        out.signal, p.row));
  } else {
    // theme-tsmom-v1: the same re-rank, times the mass in force at each date (W_theme before the
    // first block). Each cell still receives its themes in index order, so a schedule repeating
    // W_theme is the branch above bit for bit; a zero mass adds nothing.
    for (usize t = 0; t < p.std_plane.size(); ++t)
      for (usize b = 0; b <= p.schedule.size(); ++b) {
        const usize begin = b == 0 ? 0 : p.schedule[b - 1].begin;
        const usize end = b == p.schedule.size() ? p.cfg.dates : p.schedule[b].begin;
        const f64 mass = b == 0 ? p.std_mass[t] : p.schedule[b - 1].mass[t];
        if (begin == end || !(mass > 0)) continue;
        ATX_TRY_VOID(cb::add_group_rerank(p.std_plane[t], p.cfg.instruments, begin, end, mass, out.signal, p.row));
      }
  }
  // two-speed-v1 (else no-op): each theme's same re-rank times the same mass in force, added to its
  // sleeve's plane (a second pass; `signal` above is untouched), and per date d the fast themes'
  // share of the mass in force of the themes with a present member at d (Ruling PM8-16 #9: a
  // theme without one adds nothing to the blend at d, so it carries no share there), summed in
  // theme index order; no such theme: 0.
  if (!p.sleeve_of.empty()) {
    const usize names = p.cfg.instruments;
    for (usize b = 0; b <= p.schedule.size(); ++b) {
      const usize begin = b == 0 ? 0 : p.schedule[b - 1].begin;
      const usize end = b == p.schedule.size() ? p.cfg.dates : p.schedule[b].begin;
      if (begin == end) continue;
      const std::vector<f64>& mass = b == 0 ? p.std_mass : p.schedule[b - 1].mass;
      for (usize t = 0; t < p.std_plane.size(); ++t) {
        if (!(mass[t] > 0)) continue;
        auto& plane = p.sleeve_of[t] ? out.sleeve_fast : out.sleeve_slow;
        ATX_TRY_VOID(cb::add_group_rerank(p.std_plane[t], names, begin, end, mass[t], plane,
                                          p.row));
      }
      for (usize d = begin; d < end; ++d) {
        f64 fast = 0, total = 0;
        for (usize t = 0; t < p.std_plane.size(); ++t) {
          const auto row = std::span<const f64>(p.std_plane[t]).subspan(d * names, names);
          if (std::all_of(row.begin(), row.end(), [](f64 v) { return std::isnan(v); })) continue;
          total += mass[t];
          if (p.sleeve_of[t]) fast += mass[t];
        }
        out.sleeve_fast_share[d] = total > 0 ? fast / total : 0.0;
      }
    }
  }
  std::vector<IcThemeBlock>().swap(p.schedule);
  std::vector<std::vector<f64>>().swap(p.std_plane);
  for (usize d = 0; d < p.cfg.dates; ++d)
    out.contribution_fraction[d] = out.eligible_names[d]
      ? out.contribution_fraction[d] / static_cast<f64>(out.eligible_names[d]) : 0;
  for (usize d = p.cfg.decision_begin; d < p.cfg.decision_end; ++d) {
    const auto offset = d * p.cfg.instruments;
    const bool rebalance = (d - p.cfg.decision_begin) % p.cfg.cadence == 0;
    if (rebalance) {
      std::fill(p.target.begin(), p.target.end(), 0); p.row.clear();
      for (usize i = 0; i < p.cfg.instruments; ++i)
        if (p.member[offset + i]) p.row.emplace_back(out.signal[offset + i], i);
      sort_ranks(p.row);
      each_centered_rank(p.row, [&](usize i, f64 r) { p.target[i] = r; });
      f64 sum = 0;
      for (const auto& v : p.row) sum += p.target[v.second];
      const f64 mean = p.row.empty() ? 0 : sum / static_cast<f64>(p.row.size());
      f64 gross = 0;
      for (const auto& v : p.row) { p.target[v.second] -= mean; gross += std::abs(p.target[v.second]); }
      if (gross > 0) for (const auto& v : p.row) p.target[v.second] /= gross;
    }
    f64 turnover = 0, gross = 0, net = 0;
    for (usize i = 0; i < p.cfg.instruments; ++i) {
      const f64 next = !p.member[offset + i] ? 0 : rebalance
        ? p.current[i] + p.cfg.trade_fraction * (p.target[i] - p.current[i]) : p.current[i];
      turnover += std::abs(next - p.current[i]); p.current[i] = next;
      gross += std::abs(next); net += next;
    }
    out.planned_turnover[d] = turnover; out.planned_gross[d] = gross; out.planned_net[d] = net;
    out.total_planned_turnover += turnover;
    if (out.deployment_date == p.cfg.dates && gross > 0) {
      out.deployment_date = d; out.deployment_turnover = turnover;
    }
  }
  p.finished = true; return co::Ok(std::move(out));
}
} // namespace atx::impl::strategy
